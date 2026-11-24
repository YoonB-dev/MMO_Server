#include "pch.h"
#include "DBQueue.h"
#include "DBConnectionPool.h"
#include "DBBind.h"
#include "GameSession.h"
#include "Player.h"
#include "Room.h"

// main에서 생성 (GRoom과 같은 이유: 전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
shared_ptr<DBQueue> GDBQueue;

// 흐름 (같은 계정 기준, 전부 이 줄에서 순서대로)
//   입장   LoadCharacter  -> SELECT (고른 캐릭터 + 내 계정) -> SetPlayer -> _online에 추가 -> Room::Enter
//   나감   Room::Leave    -> LeaveCharacter  -> UPDATE -> _online에서 뺌 -> (_waiting에 있으면) LoadCharacter
//   중복   LoadCharacter  -> _online에 있음  -> 옛 세션 Disconnect, 새 세션은 _waiting -> 옛 세션의 LeaveCharacter가 이어 줌
//   주기   Room::SaveAllPlayers -> SaveCharacters -> UPDATE 여러 줄

// steady_clock: 시계 맞춤으로 되돌아가지 않는 시계 (구간 재기용). int64: DoAsync에 넘기는 requestUs와 타입이 정확히 같아야 함
static int64 NowUs()
{
	return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

void DBQueue::JobStats::Add(int64 waitUs, int64 runUs)
{
	const uint64 wait = static_cast<uint64>((std::max)(waitUs, int64(0)));	// steady_clock이라 음수는 안 나오지만 혹시 몰라 0으로
	const uint64 run = static_cast<uint64>((std::max)(runUs, int64(0)));
	count++;
	waitSumUs += wait;
	runSumUs += run;
	waitMaxUs = (std::max)(waitMaxUs, wait);
	runMaxUs = (std::max)(runMaxUs, run);
}

// 만들 때 실행 시작 시각을 잡고, 범위가 끝날 때(소멸자) 대기 / 실행 시간을 stats에 더함
//   RAII: 함수 중간의 어느 return으로 나가도 소멸자는 반드시 불림 (Room.cpp의 ScopedTimerUs와 같은 방식)
struct DBQueue::ScopedJob
{
	JobStats&	stats;
	int64		requestUs;
	int64		startUs = NowUs();
	~ScopedJob() { stats.Add(startUs - requestUs, NowUs() - startUs); }
};

void DBQueue::StartStats()
{
	DoTimer(STATS_INTERVAL_MS, &DBQueue::LogStats);
}

// 요청 입구: 부른 스레드에서 지금 시각을 잡고 줄에 넣음 (멤버 함수 포인터가 private이어도 여기는 DBQueue 안이라 됨)
void DBQueue::RequestLoad(GameSessionRef session, int64 accountId, int64 characterId)
{
	DoAsync(&DBQueue::LoadCharacter, session, accountId, characterId, NowUs());
}

void DBQueue::RequestLeave(CharacterData data)
{
	DoAsync(&DBQueue::LeaveCharacter, std::move(data), NowUs());
}

void DBQueue::RequestSave(xvector<CharacterData> rows)
{
	DoAsync(&DBQueue::SaveCharacters, std::move(rows), NowUs());	// 받은 벡터를 복사하지 않고 Job으로 넘김
}

// 10초 동안 일감이 하나라도 있었을 때만 한 줄 (부하 테스트 Test/Load/Run-Load.ps1이 읽을 예정)
//   wait: 요청 ~ 이 줄에서 시작 (앞 일감을 기다린 시간), run: 그 일감 자체 (DB 쿼리 포함)
void DBQueue::LogStats()
{
	auto avgMs = [](uint64 sumUs, uint64 count) { return count > 0 ? sumUs / 1000.0 / count : 0.0; };

	if (_loadStats.count + _leaveStats.count + _saveStats.count > 0)
	{
		const JobStats& l = _loadStats;
		const JobStats& v = _leaveStats;
		const JobStats& s = _saveStats;
		LogLine() << "[DB] load " << l.count
			<< " (wait avg " << avgMs(l.waitSumUs, l.count) << "ms max " << (l.waitMaxUs / 1000.0) << "ms"
			<< ", run avg " << avgMs(l.runSumUs, l.count) << "ms max " << (l.runMaxUs / 1000.0) << "ms)"
			<< ", leave " << v.count
			<< " (wait avg " << avgMs(v.waitSumUs, v.count) << "ms max " << (v.waitMaxUs / 1000.0) << "ms"
			<< ", run avg " << avgMs(v.runSumUs, v.count) << "ms max " << (v.runMaxUs / 1000.0) << "ms)"
			<< ", save " << s.count << " (" << _savedRows << " rows"
			<< ", wait avg " << avgMs(s.waitSumUs, s.count) << "ms max " << (s.waitMaxUs / 1000.0) << "ms"
			<< ", run avg " << avgMs(s.runSumUs, s.count) << "ms max " << (s.runMaxUs / 1000.0) << "ms)";
	}

	_loadStats = JobStats{};
	_leaveStats = JobStats{};
	_saveStats = JobStats{};
	_savedRows = 0;

	DoTimer(STATS_INTERVAL_MS, &DBQueue::LogStats);
}

void DBQueue::LoadCharacter(GameSessionRef session, int64 accountId, int64 characterId, int64 requestUs)
{
	ScopedJob job{ _loadStats, requestUs };	// 아래 어느 return으로 나가도 이 일감의 시간이 기록됨

	// 줄에서 기다리는 사이 이미 끊겼으면 DB도 안 부름
	if (session->IsConnected() == false)
		return;

	// 이미 접속 중인 계정 -> 옛 세션을 끊고, 그 세션의 저장(LeaveCharacter)이 끝난 뒤 이어서 불러옴
	//   바로 불러오면 옛 세션의 마지막 저장보다 먼저 읽어서 옛날 값으로 시작할 수 있음
	auto online = _online.find(accountId);
	if (online != _online.end())
	{
		if (GameSessionRef old = online->second.lock())
			old->Disconnect(L"Duplicate login");	// 다른 스레드에서 불러도 안전 (한 번만 처리됨)

		// 그전에 기다리던 세션이 또 있으면 그것도 끊음 (마지막 로그인이 이김)
		auto waiting = _waiting.find(accountId);
		if (waiting != _waiting.end())
		{
			if (GameSessionRef prev = waiting->second.session.lock())
				prev->Disconnect(L"Duplicate login");
		}

		_waiting[accountId] = WaitingLogin{ session, characterId, requestUs };
		LogLine() << "[Login] duplicate login: account " << accountId << ", kicking old session";
		return;
	}

	CharacterData data;
	bool found = false;
	bool ok = false;
	{
		DBConnectionGuard db(GDBConnectionPool);	// 블록이 끝나면 연결 반납
		ok = db && SelectCharacter(db, accountId, characterId, data, found);
		if (ok && found)
			ok = SelectItems(db, characterId, data.items);	// 같은 연결로 이어서 (DBBind가 앞 문장의 커서를 닫고 시작)
	}

	if (ok == false)
	{
		LogLine() << "[DB] failed to load character: account " << accountId;	// 이유는 바로 위 [ODBC Error]
		session->FailEnter(Protocol::ENTER_GAME_ERROR_SERVER_ERROR);
		return;
	}
	if (found == false)
	{
		// 없는 번호이거나 남의 캐릭터 (DB가 account_id까지 같이 걸러서 둘이 같은 결과)
		LogLine() << "[DB] no such character for account " << accountId << ": character " << characterId;
		session->FailEnter(Protocol::ENTER_GAME_ERROR_INVALID_CHARACTER);
		return;
	}

	PlayerRef player = MakeShared<Player>(session, data);
	if (session->SetPlayer(player) == false)
		return;		// 불러오는 사이 끊김 -> 입장시키지 않음

	// SetPlayer 성공 = 이 세션이 끊기면 반드시 Room::Leave -> LeaveCharacter가 와서 목록에서 빠짐
	_online[accountId] = session;

	LogLine() << "[DB] loaded character: account " << accountId << ", character " << characterId;	// 닉네임은 안 찍음 (콘솔 한글)
	GRoom->DoAsync(&Room::Enter, player);	// 입장 처리는 Room 줄에서 (세션이 아직 연결돼 있는지 한 번 더 봄)
}

void DBQueue::LeaveCharacter(CharacterData data, int64 requestUs)
{
	// 시간은 이 블록(저장 + 목록 정리)만 잼. 아래에서 이어 부르는 LoadCharacter는 load로 따로 셈 (섞이지 않게)
	{
		ScopedJob job{ _leaveStats, requestUs };
		{
			DBConnectionGuard db(GDBConnectionPool);
			if (db && SaveCharacter(db, data))
				LogLine() << "[DB] saved character on leave: account " << data.accountId;
			else
				LogLine() << "[DB] failed to save character: account " << data.accountId;
		}	// 연결은 여기서 반납 (아래 LoadCharacter가 또 빌림)

		_online.erase(data.accountId);
	}

	// 이 계정으로 기다리던 새 로그인 -> 저장이 끝났으니 이제 불러옴 (같은 DB 줄 안이라 바로 호출)
	auto waiting = _waiting.find(data.accountId);
	if (waiting != _waiting.end())
	{
		GameSessionRef next = waiting->second.session.lock();
		const int64 nextCharacterId = waiting->second.characterId;	// erase 전에 꺼내 둠 (지운 뒤에는 waiting을 못 씀)
		const int64 nextRequestUs = waiting->second.requestUs;
		_waiting.erase(waiting);
		if (next != nullptr)
			LoadCharacter(next, data.accountId, nextCharacterId, nextRequestUs);	// 기다리는 사이 끊겼으면 첫 줄 IsConnected에서 끝남
	}
}

void DBQueue::SaveCharacters(xvector<CharacterData> rows, int64 requestUs)
{
	ScopedJob job{ _saveStats, requestUs };	// db보다 먼저 만듦 -> 소멸은 거꾸로라 연결 반납까지 run에 들어감

	DBConnectionGuard db(GDBConnectionPool);
	if (!db)	// explicit operator bool이라 == false로는 비교가 안 됨 (if 조건, !, && 안에서만 bool로 바뀜)
	{
		LogLine() << "[DB] periodic save skipped: no free connection";
		return;		// 다음 주기나 나갈 때 다시 저장됨
	}

	int32 saved = 0;
	for (const CharacterData& row : rows)
	{
		if (SaveCharacter(db, row))
			saved++;
	}
	_savedRows += saved;
	LogLine() << "[DB] saved " << saved << " character(s)";
}

bool DBQueue::SelectCharacter(DBConnection& db, int64 accountId, int64 characterId, CharacterData& out, bool& found)
{
	out.accountId = accountId;
	out.characterId = characterId;

	// account_id도 같이 봄 -> 토큰의 계정 캐릭터가 아니면 줄이 안 나옴 (남의 캐릭터로 입장 불가)
	DBBind<2, 8> query(db, L"SELECT nickname, pos_x, pos_z, hp, mp, level, exp, gold FROM player_character WHERE character_id = ? AND account_id = ?");
	query.BindParam(0, characterId);
	query.BindParam(1, accountId);
	query.BindCol(0, out.nickname);		// WCHAR[21] 배열 -> 템플릿이 크기(21)까지 같이 넘김
	query.BindCol(1, out.x);
	query.BindCol(2, out.z);
	query.BindCol(3, out.hp);
	query.BindCol(4, out.mp);
	query.BindCol(5, out.level);
	query.BindCol(6, out.exp);
	query.BindCol(7, out.gold);

	if (query.Execute() == false)
		return false;

	found = query.Fetch();	// 줄이 없으면 false
	return true;
}

// 인벤토리: 이 캐릭터의 player_inventory 줄 전부 (아이템이 있는 칸들)
bool DBQueue::SelectItems(DBConnection& db, int64 characterId, vector<ItemSlot>& out)
{
	ItemSlot row;

	DBBind<1, 3> query(db, L"SELECT slot, item_id, count FROM player_inventory WHERE character_id = ?");
	query.BindParam(0, characterId);
	query.BindCol(0, row.slot);
	query.BindCol(1, row.itemId);
	query.BindCol(2, row.count);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
		out.push_back(row);	// Fetch마다 row가 이번 줄 값으로 덮어써지므로 복사해서 넣음
	return true;
}

// 저장 한 명: 전부 한 트랜잭션 (캐릭터 UPDATE + 아이템이 있는 칸 UPSERT + 빈 칸이 된 줄 DELETE)
//   칸 옮기기 때문에 필요: 0번 칸에 "검"을 쓴 직후 죽으면 1번 칸의 옛 "검"과 함께 두 개가 됨 (복사)
//   -> START TRANSACTION ~ COMMIT 사이가 전부 쓰이거나 하나도 안 쓰이게
//   ServerCore를 고치지 않고 SQL 문장으로 트랜잭션을 엶 (MySQL은 START TRANSACTION부터 COMMIT / ROLLBACK까지 자동 커밋을 멈춤)
//   인벤토리는 매번 전부 씀 (바뀐 것만 쓰면, 저장이 실패했을 때 그 변경이 다음 저장에서도 빠질 수 있음)
bool DBQueue::SaveCharacter(DBConnection& db, const CharacterData& data)
{
	if (ExecuteRaw(db, L"START TRANSACTION") == false)
		return false;

	// && 단락 평가: 앞이 실패하면 뒤를 실행하지 않음 -> 나머지 쓰기를 건너뛰고 ROLLBACK
	bool ok = UpdateCharacter(db, data);
	for (const ItemSlot& item : data.items)
		ok = ok && UpsertItem(db, data.characterId, item);
	ok = ok && DeleteEmptySlots(db, data);

	// 실패하면 반드시 ROLLBACK: 연결은 풀로 돌아가서 다른 일에 쓰이므로, 열린 트랜잭션이 남으면 다음 저장까지 묶임
	ExecuteRaw(db, ok ? L"COMMIT" : L"ROLLBACK");
	return ok;
}

// 결과를 받지 않는 문장 (START TRANSACTION / COMMIT / ROLLBACK). 앞 문장의 커서를 닫고 실행
bool DBQueue::ExecuteRaw(DBConnection& db, const WCHAR* sql)
{
	db.Unbind();
	return db.Execute(sql);
}

// 아이템이 없는 칸의 줄 삭제: "지금 아이템이 있는 칸 목록"에 없는 칸은 지움 (옮기기로 빈 칸이 된 줄)
//   FIND_IN_SET(slot, '0,3,19') = 0 -> 목록에 없는 칸. 문장 하나로 끝남 (칸마다 DELETE를 보내지 않음)
//   slot < 칸 수: 칸 수를 줄였을 때(테스트의 --inventory-size) 범위 밖 줄은 건드리지 않음 (불러올 때도 건너뛰기만 함)
bool DBQueue::DeleteEmptySlots(DBConnection& db, const CharacterData& data)
{
	wstring occupied;	// 예: L"0,3,19" (아이템이 하나도 없으면 빈 문자열 -> 범위 안 줄을 전부 지움)
	for (const ItemSlot& item : data.items)
	{
		if (occupied.empty() == false)
			occupied += L",";
		occupied += to_wstring(item.slot);
	}

	int64 id = data.characterId;	// BindParam이 바꿀 수 있는 변수의 주소를 받아서 복사본으로
	int32 size = Player::inventorySize;
	DBBind<3, 0> query(db, L"DELETE FROM player_inventory WHERE character_id = ? AND slot < ? AND FIND_IN_SET(slot, ?) = 0");
	query.BindParam(0, id);
	query.BindParam(1, size);
	query.BindParam(2, occupied.c_str());	// const WCHAR* -> 문자열로 바인딩
	return query.Execute();
}

bool DBQueue::UpdateCharacter(DBConnection& db, const CharacterData& data)
{
	CharacterData row = data;	// BindParam은 값을 바꿀 수 있는 변수의 주소를 받아서 복사본으로

	DBBind<8, 0> query(db, L"UPDATE player_character SET pos_x = ?, pos_z = ?, hp = ?, mp = ?, level = ?, exp = ?, gold = ?, updated_at = UTC_TIMESTAMP() WHERE character_id = ?");
	query.BindParam(0, row.x);
	query.BindParam(1, row.z);
	query.BindParam(2, row.hp);
	query.BindParam(3, row.mp);
	query.BindParam(4, row.level);
	query.BindParam(5, row.exp);
	query.BindParam(6, row.gold);
	query.BindParam(7, row.characterId);
	return query.Execute();
}

// 그 칸에 무엇이 몇 개인지 덮어씀: 줄이 있으면 고치고, 없으면 새 줄 (PK가 (character_id, slot)이라 한 문장으로)
//   AS new: MySQL 8.0.19부터의 문법 (예전의 VALUES(count)는 앞으로 없어질 예정)
bool DBQueue::UpsertItem(DBConnection& db, int64 characterId, const ItemSlot& item)
{
	int64 id = characterId;		// BindParam이 바꿀 수 있는 변수의 주소를 받아서 복사본으로
	ItemSlot row = item;

	DBBind<4, 0> query(db,
		L"INSERT INTO player_inventory (character_id, slot, item_id, count) VALUES (?, ?, ?, ?) AS new"
		L" ON DUPLICATE KEY UPDATE item_id = new.item_id, count = new.count, updated_at = UTC_TIMESTAMP()");
	query.BindParam(0, id);
	query.BindParam(1, row.slot);
	query.BindParam(2, row.itemId);
	query.BindParam(3, row.count);
	return query.Execute();
}
