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
//   입장   LoadCharacter  -> SELECT / INSERT -> SetPlayer -> _online에 추가 -> Room::Enter
//   나감   Room::Leave    -> LeaveCharacter  -> UPDATE -> _online에서 뺌 -> (_waiting에 있으면) LoadCharacter
//   중복   LoadCharacter  -> _online에 있음  -> 옛 세션 Disconnect, 새 세션은 _waiting -> 옛 세션의 LeaveCharacter가 이어 줌
//   주기   Room::SaveAllPlayers -> SaveCharacters -> UPDATE 여러 줄

void DBQueue::LoadCharacter(GameSessionRef session, int64 accountId)
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
			if (GameSessionRef prev = waiting->second.lock())
				prev->Disconnect(L"Duplicate login");
		}

		_waiting[accountId] = session;
		LogLine() << "[Login] duplicate login: account " << accountId << ", kicking old session";
		return;
	}

	CharacterData data;
	bool ok = false;
	{
		DBConnectionGuard db(GDBConnectionPool);	// 블록이 끝나면 연결 반납
		bool found = false;
		if (db && SelectCharacter(db, accountId, data, found))
		{
			if (found)
				ok = true;
			else
			{
				// 처음 들어온 계정 -> 기본값(시작 위치, HP/MP 가득)으로 만듦
				data = Player::NewCharacter(accountId);
				ok = InsertCharacter(db, data);
			}
		}
	}

	if (ok == false)
	{
		LogLine() << "[DB] failed to load character: account " << accountId;	// 이유는 바로 위 [ODBC Error]
		session->FailEnter(Protocol::ENTER_GAME_ERROR_SERVER_ERROR);
		return;
	}

	PlayerRef player = MakeShared<Player>(session, data);
	if (session->SetPlayer(player) == false)
		return;		// 불러오는 사이 끊김 -> 입장시키지 않음

	// SetPlayer 성공 = 이 세션이 끊기면 반드시 Room::Leave -> LeaveCharacter가 와서 목록에서 빠짐
	_online[accountId] = session;

	LogLine() << "[DB] loaded character: account " << accountId;
	GRoom->DoAsync(&Room::Enter, player);	// 입장 처리는 Room 줄에서 (세션이 아직 연결돼 있는지 한 번 더 봄)
}

void DBQueue::LeaveCharacter(CharacterData data)
{
	{
		DBConnectionGuard db(GDBConnectionPool);
		if (db && UpdateCharacter(db, data))
			LogLine() << "[DB] saved character on leave: account " << data.accountId;
		else
			LogLine() << "[DB] failed to save character: account " << data.accountId;
	}	// 연결은 여기서 반납 (아래 LoadCharacter가 또 빌림)

	_online.erase(data.accountId);

	// 이 계정으로 기다리던 새 로그인 -> 저장이 끝났으니 이제 불러옴 (같은 DB 줄 안이라 바로 호출)
	auto waiting = _waiting.find(data.accountId);
	if (waiting != _waiting.end())
	{
		GameSessionRef next = waiting->second.lock();
		_waiting.erase(waiting);
		if (next != nullptr)
			LoadCharacter(next, data.accountId);	// 기다리는 사이 끊겼으면 첫 줄 IsConnected에서 끝남
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
		if (UpdateCharacter(db, row))
			saved++;
	}
	LogLine() << "[DB] saved " << saved << " character(s)";
}

bool DBQueue::SelectCharacter(DBConnection& db, int64 accountId, CharacterData& out, bool& found)
{
	out.accountId = accountId;

	DBBind<1, 4> query(db, L"SELECT pos_x, pos_z, hp, mp FROM player_character WHERE account_id = ?");
	query.BindParam(0, accountId);
	query.BindCol(0, out.x);
	query.BindCol(1, out.z);
	query.BindCol(2, out.hp);
	query.BindCol(3, out.mp);

	if (query.Execute() == false)
		return false;

	found = query.Fetch();	// 줄이 없으면 false
	return true;
}

bool DBQueue::InsertCharacter(DBConnection& db, const CharacterData& data)
{
	CharacterData row = data;	// BindParam은 값을 바꿀 수 있는 변수의 주소를 받아서 복사본으로

	DBBind<5, 0> query(db, L"INSERT INTO player_character (account_id, pos_x, pos_z, hp, mp) VALUES (?, ?, ?, ?, ?)");
	query.BindParam(0, row.accountId);
	query.BindParam(1, row.x);
	query.BindParam(2, row.z);
	query.BindParam(3, row.hp);
	query.BindParam(4, row.mp);
	return query.Execute();
}

bool DBQueue::UpdateCharacter(DBConnection& db, const CharacterData& data)
{
	CharacterData row = data;	// InsertCharacter와 같은 이유로 복사본

	DBBind<5, 0> query(db, L"UPDATE player_character SET pos_x = ?, pos_z = ?, hp = ?, mp = ?, updated_at = UTC_TIMESTAMP() WHERE account_id = ?");
	query.BindParam(0, row.x);
	query.BindParam(1, row.z);
	query.BindParam(2, row.hp);
	query.BindParam(3, row.mp);
	query.BindParam(4, row.accountId);
	return query.Execute();
}
