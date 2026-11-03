#pragma once
#include "GameObject.h"

// 몬스터 AI 상태 (서버 판단용. 클라에는 안 보내고, 클라는 MoveInfo의 IDLE/MOVING만 봄)
enum class MonsterState
{
	Idle,		// 제자리 대기
	Patrol,		// 스폰 주변 랜덤 지점으로 걸어감
	Chase,		// 타겟 플레이어를 쫓아감
};

// 몬스터 = 월드 객체 + AI. 세션 없이 Room 틱마다 서버가 직접 움직인다
class Monster : public GameObject
{
	static constexpr float	PATROL_RANGE = 5.f;			// 스폰 기준 ±5m 사각형 안에서 배회
	static constexpr float	PATROL_SPEED = 2.f;			// m/s (플레이어는 5)
	static constexpr float	IDLE_MIN_TIME = 1.f;		// 배회 사이 대기 1~3초
	static constexpr float	IDLE_MAX_TIME = 3.f;
	static constexpr float	DETECT_RANGE = 8.f;			// 이 안에 플레이어가 들어오면 Chase
	static constexpr float	STOP_RANGE = 1.5f;			// 타겟과 이만큼 가까우면 멈춤 (겹치지 않게)
	static constexpr float	CHASE_SPEED = 4.f;			// m/s (플레이어 5보다 느리게 -> 도망칠 수 있음)
	static constexpr float	RAD_TO_DEG = 57.2957795f;	// Room.h와 같은 값

public:
	Monster(float x, float z);

	// Room::Tick에서 매 틱 호출. 출발하거나 멈췄으면(IDLE <-> MOVING) true -> 바로 S_MOVE에 넣음
	//   room: 플레이어를 찾을 때만 빌려 씀 (멤버로 들고 있지 않음)
	bool	Update(Room& room, float dt);

private:
	void	UpdateIdle(Room& room, float dt);
	void	UpdatePatrol(Room& room, float dt);
	void	UpdateChase(Room& room, float dt);
	bool	TryDetect(Room& room);
	void	SetState(MonsterState next);
	bool	MoveTowards(float x, float z, float speed, float dt);
	void	StopMove();

private:
	MonsterState	_state = MonsterState::Idle;
	float			_spawnX;			// 배회 중심
	float			_spawnZ;
	float			_destX = 0.f;		// Patrol 목적지
	float			_destZ = 0.f;
	float			_idleTime = 0.f;	// Idle 남은 시간(초). 0에서 시작 -> 첫 틱에 바로 배회 시작
	uint64			_targetId = 0;		// Chase 대상 플레이어 id (0 = 없음)
};
