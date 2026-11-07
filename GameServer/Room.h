#pragma once
#include "JobQueue.h"

// 매개변수로만 쓰는 타입은 선언만 (무거운 헤더를 include하지 않으려고, 정의는 Room.cpp에서 include)
namespace Protocol { class S_MOVE; }
class MoveSendPolicy;
struct MonsterSpawnData;
struct SkillData;

// 게임 월드(전역 방 하나). 입장/퇴장/이동/전투는 전부 DoAsync로 이 JobQueue에 넣어서 한 줄로 처리한다 (흐름은 Room.cpp 맨 위)
class Room : public JobQueue
{
	enum : uint64
	{
		TICK_INTERVAL_MS = 50,		// 20Hz
		TICKS_PER_SECOND = 1000 / TICK_INTERVAL_MS,	// 20
		TICK_LOG_INTERVAL = 200,	// 200틱(10초)마다 통계 로그 (틱 간격, 처리 시간, 몬스터 전송 수)
		MAX_TICK_DELAY_MS = 1000,	// 이보다 밀리면 따라잡지 않고 기준 시각을 다시 잡음
		MOVE_RESEND_TICKS = 4,		// 움직이는 중에는 4틱(200ms)마다 위치를 다시 보냄
		COOLDOWN_SLACK_TICKS = 1,	// 쿨타임 여유. 틱이 몇 ms 늦게 돌아도 정상 클라가 거절되지 않게
	};

	static constexpr int32	MAX_INPUTS_PER_SECOND = 30;	// 정상 클라는 바뀔 때만 보내서 보통 10개 이하
	static constexpr float	MIN_DIR_LENGTH = 0.01f;		// 이보다 짧은 방향은 0(멈춤)으로 봄
	static constexpr float	MOVE_SPEED = 5.f;							// m/s
	static constexpr float	TICK_DT = TICK_INTERVAL_MS / 1000.f;		// 한 틱 = 0.05초 (실제 간격과 관계없이 고정 -> 재현 가능)
	static constexpr float	WORLD_HALF_SIZE = 100.f;					// 월드 200x200, 가운데가 (0, 0)
	static constexpr float	RAD_TO_DEG = 57.2957795f;					// 라디안 -> 도 (180 / π)
	static constexpr float	SKILL_RANGE_SLACK = 1.f;	// 사거리 여유 (m). 클라가 보는 위치는 지연만큼 다름 (몬스터 4m/s x 왕복 200ms = 0.8m)

	// 타겟 검사 결과 (HandleSkill의 4~6번)
	enum class TargetCheck { Ok, NoTarget, BadTarget, Invulnerable, OutOfRange };

	// 타겟 문제 이유별 횟수. 거절(대상 필수 스킬)과 빗나감(대상 없어도 되는 스킬)에 같은 모양으로 씀
	struct TargetFailStats
	{
		uint64	noTarget = 0;		// target_id = 0
		uint64	badTarget = 0;		// 없는 id, 이미 죽은 몬스터, 플레이어 id
		uint64	invulnerable = 0;	// 복귀(Return) 중인 몬스터
		uint64	outOfRange = 0;		// 서버 위치 기준 사거리 + 여유 밖
	};

	// 스킬을 아예 못 쓴 요청 (S_SKILL 없음). 한 줄씩 찍지 않고 세기만 함 -> 핵이 초당 수천 개를 보내도 로그가 넘치지 않음
	struct SkillRejectStats
	{
		uint64			unknownSkill = 0;	// 표(data_skill)에 없는 skill_id
		uint64			cooldown = 0;		// 쿨타임 중
		uint64			noMp = 0;			// MP 부족
		TargetFailStats	target;				// 대상 필수 스킬의 타겟 문제
	};

public:
	void	SetMonsterSendPolicy(const MoveSendPolicy* policy);	// Start 전에 main에서 한 번
	void	Start();				// 첫 틱 예약 (main에서 한 번)
	void	Enter(PlayerRef player);
	void	Leave(uint64 objectId);
	void	HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq);
	void	HandleSkill(PlayerRef player, int32 skillId, uint64 targetId);

	// 몬스터 AI가 쓰는 조회/알림 (Room JobQueue 안, 틱 도중에만 호출)
	PlayerRef	FindPlayer(uint64 objectId);
	PlayerRef	FindNearestPlayer(float x, float z, float range);
	void		BroadcastHp(GameObject& target, int32 damage, uint64 attackerId);	// target의 지금 HP를 모두에게 (데미지, 회복 공통)

private:
	void	SpawnMonsters();
	void	SpawnMonster(MonsterSpawnData spawn);				// 한 마리 생성 + S_SPAWN. 시작할 때와 리스폰 때 같이 씀
	void	KillMonster(MonsterRef monster, uint64 killerId);	// 방에서 빼기 + S_DIE + 리스폰 예약
	void	Tick();
	void	LogStats(uint64 now);
	void	ResetInputCounts();
	void	UpdatePlayerStats();					// 1초마다 스탯 갱신 (자연 회복)
	void	SendMp(const PlayerRef& player);		// S_CHANGE_MP를 본인에게
	void	UpdatePlayers(Protocol::S_MOVE& pkt);
	void	UpdateMonsters(Protocol::S_MOVE& pkt);
	bool	ApplyInput(const PlayerRef& player);
	void	MovePlayer(const PlayerRef& player);
	void	RejectInput(PlayerRef player, uint32 seq, const char* reason);
	TargetCheck	CheckTarget(const PlayerRef& player, const SkillData& skill, uint64 targetId, MonsterRef& outTarget);
	static void	CountTargetFail(TargetFailStats& stats, TargetCheck result);
	void	ApplyDamage(GameObject& target, uint64 attackerId, int32 damage);
	void	Broadcast(SendBufferRef sendBuffer, uint64 exceptId = 0);
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음
	xunordered_map<uint64, MonsterRef>	_monsters;	// 위와 같음 (Start에서 채움)

	uint64	_tick = 0;				// 서버 틱 번호 (S_MOVE의 server_tick)
	uint64	_nextTickTime = 0;		// 다음 틱 목표 시각 (GetTickCount64 기준, ms)
	uint64	_logStartTime = 0;		// 틱 간격 측정 시작 시각

	const MoveSendPolicy*	_monsterSendPolicy = nullptr;	// 몬스터 S_MOVE 전송 규칙. static 정책 객체를 빌려 씀 (소유 안 함)

	// 10초(200틱) 통계 -> LogStats에서 출력 후 0으로
	uint64	_workTimeSumUs = 0;		// 틱 처리 시간 합 (마이크로초)
	uint64	_workTimeMaxUs = 0;		// 가장 오래 걸린 틱
	uint64	_monsterMovesSent = 0;	// S_MOVE에 넣은 몬스터 항목 수
	SkillRejectStats	_skillRejects;	// 스킬 요청 거절 이유별 횟수
	TargetFailStats		_skillMisses;	// 빗나감 이유별 횟수 (S_SKILL target 0으로 나간 것)
};

extern RoomRef GRoom;
