#pragma once
#include "JobQueue.h"
#include "Grid.h"
#include "Enum.pb.h"	// Protocol::ChatChannel (enum은 class처럼 이름만 미리 선언해 둘 수 없어서 헤더를 포함)
#include <random>		// _rng (드랍 확률)

// 매개변수로만 쓰는 타입은 선언만 (무거운 헤더를 include하지 않으려고, 정의는 Room.cpp에서 include)
namespace Protocol { class S_MOVE; class S_SPAWN; }
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
	static constexpr size_t	MAX_PACKET_BODY_BYTES = 4000;	// 나눠 보내는 패킷 한 개의 최대 크기 (SendBuffer 청크 6000 안에 여유 있게)
	static constexpr int32	LOAD_MONSTER_TYPE = 1;			// 부하 테스트 몬스터 종류 (data_monster의 슬라임)
	static constexpr int32	LOAD_SPAWN_ID_BASE = 100000;	// 부하 테스트 스폰 줄 번호 (기획 데이터 spawn_id와 안 겹치게)
	static constexpr int32	LOAD_RESPAWN_MS = 10000;
	static constexpr uint32	LOAD_MONSTER_SEED = 12345;		// 고정 -> 매번 같은 배치
	// 시야 (AOI): 이 거리 안에 들어오면 S_SPAWN, 나가는 거리 밖으로 가면 S_DESPAWN. 사이에서는 그대로 (경계에서 깜빡임 방지)
	//   들어오는 반경은 상호작용 거리보다 커야 함 (원거리 스킬 10m + 여유 1m, 몬스터 추적 포기 12m)
	static constexpr float	VIEW_ENTER_RADIUS = 30.f;
	static constexpr float	VIEW_LEAVE_RADIUS = 35.f;	// 5m = 1초 동안 걷는 거리. Grid 칸 크기로도 씀 (3x3칸이면 이 반경을 다 덮음)
	static constexpr float	VIEW_EVENT_SLACK = 1.f;	// 이벤트 받는 사람 찾을 때 여유 (틱 도중에는 위치가 시야 계산 뒤로 한 틱만큼 움직였을 수 있음)
	static constexpr float	SKILL_RANGE_SLACK = 1.f;	// 사거리 여유 (m). 클라가 보는 위치는 지연만큼 다름 (몬스터 4m/s x 왕복 200ms = 0.8m)
	// 바닥 드랍
	static constexpr int32	MAX_DROPS = 500;			// 방 전체 상한. 넘으면 가장 오래된 것부터 지움 (몬스터가 아주 많을 때 대비)
	static constexpr float	DROP_SCATTER_RADIUS = 1.f;	// 몬스터 자리에서 이 반경 안에 흩뿌림 (m)
	// 채팅 제한
	static constexpr int32	CHAT_MAX_CHARS = 100;		// 글자 수 (바이트 아님. 한글도 1글자)
	static constexpr uint64	CHAT_COOLDOWN_TICKS = 10;	// 0.5초에 한 번

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
		uint64			dead = 0;			// 죽은 플레이어의 요청
		uint64			unknownSkill = 0;	// 표(data_skill)에 없거나 몬스터 전용인 skill_id
		uint64			cooldown = 0;		// 쿨타임 중
		uint64			noMp = 0;			// MP 부족
		TargetFailStats	target;				// 대상 필수 스킬의 타겟 문제
	};

public:
	void	SetMonsterSendPolicy(const MoveSendPolicy* policy);	// Start 전에 main에서 한 번
	void	SetSaveInterval(uint64 seconds);	// 주기 저장 간격 (Start 전에 main에서 한 번, 기본 60초)
	void	SetLoadMonsters(int32 count, float area);	// 부하 테스트용 몬스터 추가 (Start 전에 main에서 한 번, 기본 0마리)
	void	SetDropLifetime(uint64 seconds);			// 바닥 드랍 수명 (Start 전에 main에서 한 번, 기본 60초)
	void	Start();				// 첫 틱 예약 (main에서 한 번)
	void	Enter(PlayerRef player);
	void	Leave(PlayerRef player);	// 방에서 빼기 + 저장 요청 (저장할 값을 꺼내야 해서 id가 아니라 Player)
	void	HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq);
	void	HandleSkill(PlayerRef player, int32 skillId, uint64 targetId);
	void	HandleChat(PlayerRef player, Protocol::ChatChannel channel, string message);	// C_CHAT

	// 몬스터 AI가 쓰는 조회/알림 (Room JobQueue 안, 틱 도중에만 호출)
	PlayerRef	FindPlayer(uint64 objectId);
	PlayerRef	FindNearestPlayer(float x, float z, float range);
	void		BroadcastHp(GameObject& target, int32 damage, uint64 attackerId);	// target의 지금 HP를 target이 보이는 사람에게 (데미지, 회복 공통)
	void		BroadcastSkill(GameObject& caster, const SkillData& skill, uint64 targetId);	// S_SKILL (플레이어, 몬스터 공통)
	void		HitPlayer(const PlayerRef& target, uint64 attackerId, int32 damage);			// 데미지 + HP 0이면 KillPlayer

private:
	void	SpawnMonsters();
	void	SpawnLoadMonsters();								// 부하 테스트용 (SetLoadMonsters로 정한 수만큼)
	void	SpawnMonster(MonsterSpawnData spawn);				// 한 마리 생성 + S_SPAWN. 시작할 때와 리스폰 때 같이 씀
	void	KillMonster(MonsterRef monster, uint64 killerId);	// 방에서 빼기 + S_DIE + 처치 보상 + 리스폰 예약
	void	GiveExp(uint64 killerId, int32 amount);				// 처치 경험치: 처치자에게 S_CHANGE_EXP, 레벨업이면 주변에 S_LEVEL_UP
	void	SpawnDrops(Monster& monster);						// 처치 드랍: 골드 한 더미 + 드랍표 확률 아이템을 몬스터 자리에
	void	AddDrop(int32 itemId, int32 amount, float x, float z);	// 하나 만들기 (상한이면 가장 오래된 것부터 지움)
	void	RemoveDrop(DropRef drop);							// 바닥에서 없애기: 보던 사람에게 S_DESPAWN
	void	ExpireDrops();										// 1초마다: 수명이 지난 드랍 제거
	void	KillPlayer(const PlayerRef& player, uint64 killerId);	// 방에 남김(시체) + 정지 + S_DIE + 부활 예약
	void	RevivePlayer(uint64 playerId);						// 부활 (KillPlayer의 DoTimer). id로 받음: 그사이 나갔으면 못 찾고 끝
	void	ApplyDamage(GameObject& target, uint64 attackerId, int32 damage);
	void	Tick();
	void	LogStats(uint64 now);
	void	ResetInputCounts();
	void	UpdatePlayerStats();					// 1초마다 스탯 갱신 (자연 회복)
	void	SaveAllPlayers();						// 주기 저장: 전원 값을 복사해서 DB 줄로
	void	SendMp(const PlayerRef& player);		// S_CHANGE_MP를 본인에게
	void	UpdatePlayers(Protocol::S_MOVE& pkt);
	void	UpdateMonsters(Protocol::S_MOVE& pkt);
	bool	ApplyInput(const PlayerRef& player);
	void	MovePlayer(const PlayerRef& player);
	void	RejectInput(PlayerRef player, uint32 seq, const char* reason);
	TargetCheck	CheckTarget(const PlayerRef& player, const SkillData& skill, uint64 targetId, MonsterRef& outTarget);
	static void	CountTargetFail(TargetFailStats& stats, TargetCheck result);
	void	UpdateViews(const Protocol::S_MOVE& tickMoves);						// 매 틱 끝: 플레이어마다 UpdateView
	void	UpdateView(const PlayerRef& player, const Protocol::S_MOVE& tickMoves);	// 시야 다시 계산 -> S_DESPAWN, S_SPAWN, S_MOVE를 이 사람에게
	void	EnterView(const PlayerRef& player);								// 입장 즉시: 주변을 나에게, 나를 주변에게
	template<typename Func>
	void	ForEachViewer(GameObject& object, Func&& func);					// object가 보이는 플레이어마다 func (object가 플레이어면 본인 포함)
	void	BroadcastToViewers(GameObject& object, SendBufferRef sendBuffer);	// object가 보이는 사람에게 (본인 포함)
	void	SendMoves(const PlayerRef& player, Protocol::S_MOVE& all);				// 큰 S_MOVE를 나눠서 한 명에게
	void	SendSpawns(const PlayerRef& player, const Protocol::S_SPAWN& all);	// 큰 S_SPAWN을 나눠서 한 명에게
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음
	xunordered_map<uint64, MonsterRef>	_monsters;	// 위와 같음 (Start에서 채움)
	xmap<uint64, DropRef>				_drops;		// 바닥 드랍. 정렬된 map: id 순 = 만든 순서 (id가 계속 커짐) -> begin()이 가장 오래된 드랍
	uint64	_dropLifetimeTicks = 60 * TICKS_PER_SECOND;	// 드랍 수명 (--drop-lifetime-sec, 테스트에서 짧게)
	std::mt19937	_rng{ std::random_device{}() };		// 드랍 확률, 골드 양, 흩뿌릴 자리 (Room 줄에서만 써서 하나로 충분)

	uint64	_tick = 0;				// 서버 틱 번호 (S_MOVE의 server_tick)
	uint64	_nextTickTime = 0;		// 다음 틱 목표 시각 (GetTickCount64 기준, ms)
	uint64	_logStartTime = 0;		// 틱 간격 측정 시작 시각
	uint64	_saveIntervalTicks = 60 * TICKS_PER_SECOND;	// 주기 저장 간격 (틱). 서버가 죽어도 잃는 건 이만큼

	Grid	_grid{ WORLD_HALF_SIZE, VIEW_LEAVE_RADIUS };	// 시야 / 몬스터 감지에서 주변 후보만 찾을 때 씀 (_players, _monsters와 같은 객체를 칸별로)

	const MoveSendPolicy*	_monsterSendPolicy = nullptr;	// 몬스터 S_MOVE 전송 규칙. static 정책 객체를 빌려 씀 (소유 안 함)

	// 부하 테스트용 몬스터 (--load-monsters, --load-area). 기획 데이터(data_monster_spawn)와 별도로 SpawnMonsters에서 추가
	int32	_loadMonsterCount = 0;
	float	_loadMonsterArea = 0.f;

	// 10초(200틱) 통계 -> LogStats에서 출력 후 0으로
	uint64	_workTimeSumUs = 0;		// 틱 처리 시간 합 (마이크로초)
	uint64	_workTimeMaxUs = 0;		// 가장 오래 걸린 틱
	uint64	_monsterMovesSent = 0;	// S_MOVE에 넣은 몬스터 항목 수
	uint64	_sentPackets = 0;		// 보낸 패킷 수 (SendTo 호출 수 = 받는 사람 수만큼)
	uint64	_sentBytes = 0;			// 보낸 바이트 수 (패킷 헤더 포함)
	SkillRejectStats	_skillRejects;	// 스킬 요청 거절 이유별 횟수
	TargetFailStats		_skillMisses;	// 빗나감 이유별 횟수 (S_SKILL target 0으로 나간 것)
};

extern RoomRef GRoom;
