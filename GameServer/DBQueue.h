#pragma once
#include "JobQueue.h"

class DBConnection;
struct CharacterData;
struct ItemSlot;

// DB 전용 줄. 게임 중에 DB를 부르는 일은 전부 여기로 DoAsync
//   Room 줄은 DB를 기다리지 않음 (DB가 느려도 틱이 안 멈춤)
//   한 줄이라 한 번에 하나씩, 들어온 순서대로 실행 -> 같은 계정의 불러오기 / 저장 순서가 섞이지 않음
//   DB 일은 워커 스레드 하나를 잠깐 막지만, Room 줄은 다른 워커가 계속 돌림
//
// 접속 중인 계정 목록(_online)도 이 줄이 들고 있음 -> 같은 계정은 "마지막 저장 -> 다음 불러오기" 순서가 항상 지켜짐
//   중복 로그인: 옛 세션을 끊고 새 세션은 _waiting에 둠 -> 옛 세션의 LeaveCharacter(저장)가 끝나면 이어서 불러옴
class DBQueue : public JobQueue
{
public:
	// 입장: 고른 캐릭터를 불러옴 (그 계정의 캐릭터가 아니거나 없으면 INVALID_CHARACTER) -> Player 생성 -> Room에 입장 요청
	//   캐릭터는 아웃게임 서버(POST /characters)가 만듦. 게임 서버는 불러오고 저장만 함
	//   이미 접속 중인 계정이면(다른 캐릭터여도) 옛 세션을 끊고, 그 세션의 저장이 끝날 때까지 기다림
	void	LoadCharacter(GameSessionRef session, int64 accountId, int64 characterId);
	// 나감 (Room::Leave가 보냄): 저장 + 접속 목록에서 뺌 + 이 계정으로 기다리던 로그인이 있으면 이어서 불러옴
	void	LeaveCharacter(CharacterData data);
	// 주기 저장 (Room::SaveAllPlayers가 보냄): 여러 명을 연결 하나로 차례로 UPDATE
	void	SaveCharacters(xvector<CharacterData> rows);

private:
	static bool	SelectCharacter(DBConnection& db, int64 accountId, int64 characterId, CharacterData& out, bool& found);
	static bool	SelectItems(DBConnection& db, int64 characterId, vector<ItemSlot>& out);
	static bool	SaveCharacter(DBConnection& db, const CharacterData& data);		// UpdateCharacter + 아이템마다 UpsertItem
	static bool	UpdateCharacter(DBConnection& db, const CharacterData& data);
	static bool	UpsertItem(DBConnection& db, int64 characterId, const ItemSlot& item);

private:
	// 옛 세션의 저장을 기다리는 새 로그인: 세션 + 어느 캐릭터로 들어오려 했는지 (옛 세션과 다른 캐릭터일 수 있음)
	struct WaitingLogin
	{
		weak_ptr<GameSession>	session;
		int64					characterId = 0;
	};

	// DB 줄 안에서만 접근 -> 락 없음. 세션은 weak_ptr (목록 때문에 세션이 안 사라지는 일이 없게)
	//   둘 다 계정 기준: 같은 계정은 캐릭터가 달라도 한 번에 한 세션만
	xunordered_map<int64, weak_ptr<GameSession>>	_online;	// 입장한 계정 -> 그 세션 (SetPlayer 성공 ~ LeaveCharacter)
	xunordered_map<int64, WaitingLogin>				_waiting;	// 계정 -> 옛 세션의 저장을 기다리는 새 로그인
};

extern shared_ptr<DBQueue> GDBQueue;
