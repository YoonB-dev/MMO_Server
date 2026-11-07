#pragma once
#include "GameObject.h"
#include "GameData.h"	// MonsterSpawnData를 값(복사본)으로 들고 있어서 크기를 알아야 함 (선언만으로는 부족)

// 몬스터 AI 상태 (서버 판단용. 클라에는 안 보내고, 클라는 MoveInfo의 IDLE/MOVING만 봄)
enum class MonsterState
{
	Idle,		// 제자리 대기
	Patrol,		// 스폰 주변 랜덤 지점으로 걸어감
	Chase,		// 타겟 플레이어를 쫓아감
	Return,		// 스폰으로 빠르게 돌아감 (플레이어 무시 + 무적, 도착하면 HP 회복)
};

// 몬스터 = 월드 객체 + AI. 세션 없이 Room 틱마다 서버가 직접 움직인다
class Monster : public GameObject
{
	// AI 수치(배회 범위, 감지 거리, 속도 등)는 몬스터 종류마다 다르게 하려고 DB(data_monster)로 옮김 -> _data에서 읽음
	static constexpr float	RAD_TO_DEG = 57.2957795f;	// Room.h와 같은 값

public:
	// data: 이 몬스터 종류의 수치 (GameData가 들고 있는 원본), spawn: 어느 스폰 줄에서 나왔나 (복사해서 보관)
	Monster(const MonsterData& data, const MonsterSpawnData& spawn);

	// Room::Tick에서 매 틱 호출 (보낼지는 Room의 전송 정책이 lastSentMove와 비교해서 정함)
	//   room: 플레이어를 찾거나 HP 회복을 알릴 때만 빌려 씀 (멤버로 들고 있지 않음)
	void	Update(Room& room, float dt);

	// Room이 쓰는 조회
	bool					IsInvulnerable() const { return _state == MonsterState::Return; }	// 돌아가는 중에는 무적
	const MonsterSpawnData&	GetSpawn() const { return _spawn; }	// 죽었을 때 같은 스폰 줄로 리스폰 예약

public:
	// Room JobQueue 안에서만 접근 -> 락 없음
	Protocol::MoveInfo	lastSentMove;	// 마지막으로 S_MOVE에 넣은 이동 (전송 정책의 비교 기준)

private:
	void	UpdateIdle(Room& room, float dt);
	void	UpdatePatrol(Room& room, float dt);
	void	UpdateChase(Room& room, float dt);
	void	UpdateReturn(Room& room, float dt);	// 도착하면 HP 회복을 알려야 해서 Room을 받음 (플레이어는 여전히 안 찾음)
	bool	TryDetect(Room& room);
	void	GiveUpChase(const char* reason);
	void	SetState(MonsterState next);
	bool	MoveTowards(float x, float z, float speed, float dt);
	void	StopMove();

private:
	// _data는 참조: unordered_map 원소는 다른 원소가 추가돼도 주소가 안 바뀌고, 같은 종류끼리 원본 하나를 같이 봄
	// _spawn은 복사본: vector(s_spawns) 원소는 vector가 커질 때 통째로 옮겨질 수 있어서, 20바이트를 복사해 두는 쪽이 안전
	const MonsterData&	_data;		// 이 몬스터 종류의 수치 (GameData가 프로그램 끝까지 들고 있음, 시작 후 안 바뀜)
	MonsterSpawnData	_spawn;		// 나온 스폰 줄: 배회 중심(x, z) + 리스폰 시간
	MonsterState	_state = MonsterState::Idle;
	float			_destX = 0.f;		// Patrol 목적지
	float			_destZ = 0.f;
	float			_idleTime = 0.f;	// Idle 남은 시간(초). 0에서 시작 -> 첫 틱에 바로 배회 시작
	uint64			_targetId = 0;		// Chase 대상 플레이어 id (0 = 없음)
};
