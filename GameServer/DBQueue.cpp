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

void DBQueue::LoadCharacter(GameSessionRef session, int64 accountId, int64 characterId)
{
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

		_waiting[accountId] = WaitingLogin{ session, characterId };
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

void DBQueue::LeaveCharacter(CharacterData data)
{
	{
		DBConnectionGuard db(GDBConnectionPool);
		if (db && SaveCharacter(db, data))
			LogLine() << "[DB] saved character on leave: account " << data.accountId;
		else
			LogLine() << "[DB] failed to save character: account " << data.accountId;
	}	// 연결은 여기서 반납 (아래 LoadCharacter가 또 빌림)

	_online.erase(data.accountId);

	// 이 계정으로 기다리던 새 로그인 -> 저장이 끝났으니 이제 불러옴 (같은 DB 줄 안이라 바로 호출)
	auto waiting = _waiting.find(data.accountId);
	if (waiting != _waiting.end())
	{
		GameSessionRef next = waiting->second.session.lock();
		const int64 nextCharacterId = waiting->second.characterId;	// erase 전에 꺼내 둠 (지운 뒤에는 waiting을 못 씀)
		_waiting.erase(waiting);
		if (next != nullptr)
			LoadCharacter(next, data.accountId, nextCharacterId);	// 기다리는 사이 끊겼으면 첫 줄 IsConnected에서 끝남
	}
}

void DBQueue::SaveCharacters(xvector<CharacterData> rows)
{
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

// 저장 한 명: 캐릭터 줄 UPDATE + 아이템이 있는 칸마다 UPSERT
//   인벤토리는 전부 씀 (바뀐 것만 쓰면, 저장이 실패했을 때 그 변경이 다음 저장에서도 빠질 수 있음)
//   빈 칸이 된 줄 지우기는 아직 없음: 지금은 칸이 비는 일(버리기, 사용)이 없어서. 그 기능을 만들 때 같이 넣음
//   둘을 트랜잭션으로 묶지 않음: 중간에 서버가 죽으면 한쪽만 옛 값일 수 있지만, 둘 다 "덜 저장된" 쪽이라 복사(dupe)는 안 생김
bool DBQueue::SaveCharacter(DBConnection& db, const CharacterData& data)
{
	if (UpdateCharacter(db, data) == false)
		return false;
	for (const ItemSlot& item : data.items)
	{
		if (UpsertItem(db, data.characterId, item) == false)
			return false;
	}
	return true;
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
