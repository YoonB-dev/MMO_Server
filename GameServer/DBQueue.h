#pragma once
#include "JobQueue.h"

class DBConnection;
struct CharacterData;

// DB 전용 줄. 게임 중에 DB를 부르는 일은 전부 여기로 DoAsync
//   Room 줄은 DB를 기다리지 않음 (DB가 느려도 틱이 안 멈춤)
//   한 줄이라 한 번에 하나씩, 들어온 순서대로 실행 -> 같은 계정의 불러오기 / 저장 순서가 섞이지 않음
//   DB 일은 워커 스레드 하나를 잠깐 막지만, Room 줄은 다른 워커가 계속 돌림
class DBQueue : public JobQueue
{
public:
	// 입장: 그 계정의 캐릭터를 불러옴 (없으면 기본값으로 만듦) -> Player 생성 -> Room에 입장 요청
	void	LoadCharacter(GameSessionRef session, int64 accountId);

private:
	static bool	SelectCharacter(DBConnection& db, int64 accountId, CharacterData& out, bool& found);
	static bool	InsertCharacter(DBConnection& db, const CharacterData& data);
};

extern shared_ptr<DBQueue> GDBQueue;
