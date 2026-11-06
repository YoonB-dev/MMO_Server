#pragma once
#include "GameObject.h"

struct MonsterData;	// 참조로만 쓰니 선언만 (GameData.h 전체를 include하지 않음)

// 몬스터 AI 상태 (서버 판단용. 클라에는 안 보내고, 클라는 MoveInfo의 IDLE/MOVING만 봄)
enum class MonsterState
{
	Idle,		// 제자리 대기
	Patrol,		// 스폰 주변 랜덤 지점으로 걸어감
	Chase,		// 타겟 플레이어를 쫓아감
	Return,		// 스폰으로 빠르게 돌아감 (이 동안은 플레이어를 무시)
};

// 몬스터 = 월드 객체 + AI. 세션 없이 Room 틱마다 서버가 직접 움직인다
class Monster : public GameObject
{
	// AI 수치(배회 범위, 감지 거리, 속도 등)는 몬스터 종류마다 다르게 하려고 DB(data_monster)로 옮김 -> _data에서 읽음
	static constexpr float	RAD_TO_DEG = 57.2957795f;	// Room.h와 같은 값

public:
	Monster(const MonsterData& data, float x, float z);	// data: 이 몬스터 종류의 수치 (GameData가 들고 있는 원본)

	// Room::Tick에서 매 틱 호출 (보낼지는 Room의 전송 정책이 lastSentMove와 비교해서 정함)
	//   room: 플레이어를 찾을 때만 빌려 씀 (멤버로 들고 있지 않음)
	void	Update(Room& room, float dt);

public:
	// Room JobQueue 안에서만 접근 -> 락 없음
	Protocol::MoveInfo	lastSentMove;	// 마지막으로 S_MOVE에 넣은 이동 (전송 정책의 비교 기준)

private:
	void	UpdateIdle(Room& room, float dt);
	void	UpdatePatrol(Room& room, float dt);
	void	UpdateChase(Room& room, float dt);
	void	UpdateReturn(float dt);			// Room을 안 받음: 돌아가는 동안은 플레이어를 찾지 않으므로
	bool	TryDetect(Room& room);
	void	GiveUpChase(const char* reason);
	void	SetState(MonsterState next);
	bool	MoveTowards(float x, float z, float speed, float dt);
	void	StopMove();

private:
	const MonsterData&	_data;		// 이 몬스터 종류의 수치 (GameData가 프로그램 끝까지 들고 있음, 시작 후 안 바뀜)
	MonsterState	_state = MonsterState::Idle;
	float			_spawnX;			// 배회 중심
	float			_spawnZ;
	float			_destX = 0.f;		// Patrol 목적지
	float			_destZ = 0.f;
	float			_idleTime = 0.f;	// Idle 남은 시간(초). 0에서 시작 -> 첫 틱에 바로 배회 시작
	uint64			_targetId = 0;		// Chase 대상 플레이어 id (0 = 없음)
};
