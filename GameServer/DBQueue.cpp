#include "pch.h"
#include "DBQueue.h"
#include "DBConnectionPool.h"
#include "DBBind.h"
#include "GameSession.h"
#include "Player.h"
#include "Room.h"

// main에서 생성 (GRoom과 같은 이유: 전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
shared_ptr<DBQueue> GDBQueue;

void DBQueue::LoadCharacter(GameSessionRef session, int64 accountId)
{
	// 줄에서 기다리는 사이 이미 끊겼으면 DB도 안 부름
	if (session->IsConnected() == false)
		return;

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

	LogLine() << "[DB] loaded character: account " << accountId;
	GRoom->DoAsync(&Room::Enter, player);	// 입장 처리는 Room 줄에서 (세션이 아직 연결돼 있는지 한 번 더 봄)
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
