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
	static constexpr int32	LOAD_BOSS_SPAWN_ID = 900000;	// 부하 테스트 보스 스폰 줄 번호 (기획 데이터, 부하 몬스터와 안 겹치게)
	// 시야 (AOI): 이 거리 안에 들어오면 S_SPAWN, 나가는 거리 밖으로 가면 S_DESPAWN. 사이에서는 그대로 (경계에서 깜빡임 방지)
	//   들어오는 반경은 상호작용 거리보다 커야 함 (원거리 스킬 10m + 여유 1m, 몬스터 추적 포기 12m)
	static constexpr float	VIEW_ENTER_RADIUS = 30.f;
	static constexpr float	VIEW_LEAVE_RADIUS = 35.f;	// 5m = 1초 동안 걷는 거리. Grid 칸 크기로도 씀 (3x3칸이면 이 반경을 다 덮음)
	static constexpr float	VIEW_EVENT_SLACK = 1.f;	// 이벤트 받는 사람 찾을 때 여유 (틱 도중에는 위치가 시야 계산 뒤로 한 틱만큼 움직였을 수 있음)
	static constexpr float	SKILL_RANGE_SLACK = 1.f;	// 사거리 여유 (m). 클라가 보는 위치는 지연만큼 다름 (몬스터 4m/s x 왕복 200ms = 0.8m)
	// 몰린 상황 대책 (필드보스 주변 등): 다른 플레이어는 상한까지만 보이고, 멀수록 이동을 덜 자주 보냄
	static constexpr int32	DEFAULT_MAX_VISIBLE_PLAYERS = 50;	// 한 명이 동시에 보는 다른 플레이어 상한 (--max-visible-players). 몬스터, 드랍은 상한 없음
	static constexpr float	VIEW_STICKY_BONUS = 10.f;			// 작은 시야 안에서 이미 보이던 사람은 이만큼(m) 가깝다고 침 -> 순위 경계에서 보였다 안 보였다 반복하지 않게
	static constexpr float	VIEW_NEAR_ENTER_RADIUS = 10.f;		// 작은 시야: 이 안에 들어온 사람은 상한이 꽉 차 있어도 바깥 사람을 밀어내고 보임 (전투 거리)
	static constexpr float	VIEW_NEAR_LEAVE_RADIUS = 12.f;		// 작은 시야에서 나가는 반경 (큰 시야 30/35처럼 경계에서 왔다갔다 방지)
	static constexpr float	VIEW_TIER_STEP = 100.f;				// 구역 사이 점수 간격 (시야 최대 35m + 끈적임보다 커야 구역 순서가 안 섞임)
	static constexpr uint64	VIEW_RESELECT_INTERVAL_TICKS = 10;	// 500ms. 상한을 넘을 때 순위를 다시 매기는 간격 (그 사이는 보이던 사람을 그대로 두고 빈자리만 채움)
	static constexpr float	LOD_NEAR_RADIUS = 10.f;				// 이 안은 매 틱 (전투 거리)
	static constexpr float	LOD_MID_RADIUS = 20.f;				// 이 안은 LOD_MID_INTERVAL_TICKS마다, 밖은 LOD_FAR_INTERVAL_TICKS마다
	static constexpr uint64	LOD_MID_INTERVAL_TICKS = 2;			// 100ms
	static constexpr uint64	LOD_FAR_INTERVAL_TICKS = 4;			// 200ms (그 사이는 클라가 dir * speed로 예측)
	// HP바: 맞을 때마다가 아니라 바뀐 객체를 간격마다 한 번 (보스를 400명이 때려도 보스 HP 송신은 간격당 보는 사람 수만큼)
	static constexpr uint64	HP_SYNC_INTERVAL_TICKS = 2;			// 100ms. 사이는 클라가 보간. 데미지 숫자(당사자)와 죽음(S_DIE)은 이와 상관없이 바로
	// 바닥 드랍
	static constexpr int32	DEFAULT_MAX_DROPS = 500;	// 방 전체 상한 기본값. 넘으면 가장 오래된 것부터 지움 (몬스터가 아주 많을 때 대비)
	static constexpr float	DROP_SCATTER_RADIUS = 1.f;	// 몬스터 자리에서 이 반경 안에 흩뿌림 (m)
	static constexpr uint64	DROP_PICKUP_DELAY_TICKS = 10;	// 떨어진 뒤 0.5초 동안 못 주움
	//   이 사이에 UpdateView가 주변에 S_SPAWN을 보냄 -> 아무도 못 본 드랍을 줍는 일이 없음 (줍기 연타 매크로 포함)
	// 줍기
	static constexpr float	PICKUP_RANGE = 2.f;			// 이 거리 안의 드랍만 주움 (m). 서버 위치 기준 + SKILL_RANGE_SLACK
	static constexpr uint64	PICKUP_COOLDOWN_TICKS = 4;	// 0.2초에 한 번 (키를 누르고 있어도 한 번에 하나씩)
	// 채팅 제한
	static constexpr int32	CHAT_MAX_CHARS = 100;		// 글자 수 (바이트 아님. 한글도 1글자)
	static constexpr uint64	CHAT_COOLDOWN_TICKS = 10;	// 0.5초에 한 번

	// 시야 후보 다른 플레이어 하나 (SelectVisiblePlayers). 포인터는 함수 안에서만 씀 (객체는 _players가 들고 있음)
	struct ViewCandidate
	{
		Player*	player = nullptr;
		float	distSq = 0.f;
		bool	wasVisible = false;
		float	score = 0.f;		// 우선순위 점수 (작을수록 먼저). 상한을 넘을 때만 계산
	};

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
	void	SetLoadBoss(int32 monsterId) { _loadBossType = monsterId; }	// 부하 테스트용 보스 종류 (Start 전에 main에서 한 번, 0이면 없음)
	void	SetDropLifetime(uint64 seconds);			// 바닥 드랍 수명 (Start 전에 main에서 한 번, 기본 60초)
	void	SetMaxDrops(int32 count);					// 바닥 드랍 상한 (Start 전에 main에서 한 번, 0 이하면 기본 500 그대로)
	void	SetMaxVisiblePlayers(int32 count);			// 보이는 다른 플레이어 상한 (Start 전에 main에서 한 번, 0 이하면 기본 50 그대로)
	void	Start();				// 첫 틱 예약 (main에서 한 번)
	void	Enter(PlayerRef player);	// DB 줄이 부르는 입구: 다음 틱까지 여유가 있으면 바로, 아니면 입장 대기 목록에
	void	Leave(PlayerRef player);	// 방에서 빼기 + 저장 요청 (저장할 값을 꺼내야 해서 id가 아니라 Player)
	void	HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq);
	void	HandleSkill(PlayerRef player, int32 skillId, uint64 targetId);
	void	HandleChat(PlayerRef player, Protocol::ChatChannel channel, string message);	// C_CHAT
	void	HandlePickup(PlayerRef player);															// C_PICKUP
	void	HandleMoveItem(PlayerRef player, int32 from, int32 to);									// C_MOVE_ITEM

	// 몬스터 AI가 쓰는 조회/알림 (Room JobQueue 안, 틱 도중에만 호출)
	PlayerRef	FindPlayer(uint64 objectId);
	PlayerRef	FindNearestPlayer(float x, float z, float range);
	void		SetHp(GameObject& target, int32 hp);								// HP를 바꾸는 한 입구: 값 + hpDirty (보는 사람에게는 FlushHpChanges가 간격마다)
	void		BroadcastSkill(GameObject& caster, const SkillData& skill, uint64 targetId);	// S_SKILL (플레이어, 몬스터 공통)
	void		HitPlayer(const PlayerRef& target, uint64 attackerId, int32 damage);			// 데미지 + HP 0이면 KillPlayer
	void		HitMonster(const MonsterRef& target, uint64 attackerId, int32 damage);		// 데미지 + HP 0이면 KillMonster, 아니면 어그로 (몬스터를 때리는 길은 전부 여기로)

private:
	void	SpawnMonsters();
	void	SpawnLoadMonsters();								// 부하 테스트용 (SetLoadMonsters로 정한 수만큼)
	void	SpawnLoadBoss();									// 부하 테스트용 보스 한 마리 (SetLoadBoss로 정한 종류, (0, 0))
	void	SpawnMonster(MonsterSpawnData spawn);				// 한 마리 생성 + S_SPAWN. 시작할 때와 리스폰 때 같이 씀
	void	KillMonster(MonsterRef monster, uint64 killerId);	// 방에서 빼기 + S_DIE + 처치 보상 + 리스폰 예약
	void	GiveExp(uint64 killerId, int32 amount);				// 처치 경험치: 처치자에게 S_CHANGE_EXP, 레벨업이면 주변에 S_LEVEL_UP
	void	SpawnDrops(Monster& monster);						// 처치 드랍: 골드 한 더미 + 드랍표 확률 아이템을 몬스터 자리에
	void	AddDrop(int32 itemId, int32 amount, float x, float z);	// 하나 만들기 (상한이면 가장 오래된 것부터 지움)
	void	RemoveDrop(DropRef drop, uint64 pickerId = 0);		// 바닥에서 없애기: 보던 사람에게 S_DESPAWN (주운 거면 S_PICKUP)
	void	GiveGold(const PlayerRef& player, int64 amount);	// 골드 더하기 + 본인에게 S_CHANGE_GOLD
	void	GiveItem(const PlayerRef& player, int32 itemId, int32 amount);	// 아이템 더하기 + 본인에게 S_CHANGE_ITEM
	void	SendSlots(const PlayerRef& player, int32 slotA, int32 slotB);	// 두 칸의 지금 상태를 본인에게 S_INVENTORY_SLOTS
	void	ExpireDrops();										// 1초마다: 수명이 지난 드랍 제거
	void	KillPlayer(const PlayerRef& player, uint64 killerId);	// 방에 남김(시체) + 정지 + S_DIE + 부활 예약
	void	RevivePlayer(uint64 playerId);						// 부활 (KillPlayer의 DoTimer). id로 받음: 그사이 나갔으면 못 찾고 끝
	void	ApplyDamage(GameObject& target, uint64 attackerId, int32 damage);	// HP 깎기 + 데미지 숫자를 당사자(때린 / 맞은 플레이어)에게만 바로
	SendBufferRef	MakeHpPacket(GameObject& target, int32 damage, uint64 attackerId);	// S_CHANGE_HP (target의 지금 HP)
	void	FlushHpChanges();												// 틱 끝: hpDirty인 객체를 간격마다 보는 사람에게 (damage 0 = HP바만)
	uint64	HpSyncIntervalFor(GameObject& object);							// HP바 갱신 간격 (틱). 확장: 종류별 (보스는 길게, 파티원은 매 틱 등)
	void	Tick();
	void	LogStats(uint64 now);
	// 틱 하나의 조각별 시간을 10초 통계에 더하고, 이번 10초에서 가장 느린 틱이면 내역을 남김
	//   partLockUs: 그 조각 동안 이 스레드가 락을 기다린 시간 (ServerCore LLockWaitUs의 앞뒤 차이)
	struct FlushStats;	// 앞에서 이름만 알림 (정의는 아래 통계 구조체들과 같이). 참조로만 받아서 이걸로 충분
	//   tickCycles / flushCycles: 틱 전체 / 송신 동안 이 스레드가 CPU에서 돈 사이클, flush: 송신을 세션별로 잰 결과
	void	RecordTickCost(int64 workUs, const int64 (&partUs)[7], const int64 (&partLockUs)[7], int64 cpuUs, uint32 cpuStart, uint32 cpuEnd, uint64 viewChanges,
				uint64 tickCycles, uint64 flushCycles, const FlushStats& flush);
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
	void	ProcessEnter(PlayerRef player);									// 실제 입장: S_ENTER_GAME + EnterView + 바로 송신 + 입장 구간 기록
	void	DrainPendingEnters();											// 틱 끝: 입장 대기 목록을 다음 틱 전(ENTER_DEADLINE_US)까지 처리
	void	EnterView(const PlayerRef& player);								// 입장 즉시: 주변을 나에게, 나를 주변에게 (그 사람의 상한이 남았을 때)
	void	SelectVisiblePlayers(Player& viewer, float x, float z);			// 보일 다른 플레이어를 _viewCandidates에 (상한이면 우선순위 순으로 자름)
	float	ViewPriority(Player& viewer, Player& other, float distSq, bool wasVisible);	// 작을수록 먼저 보임 (확장: 파티원, 공격 대상 등)
	uint64	MoveIntervalFor(GameObject& object, float distSq);				// 거리별 이동 전송 간격 (틱). 확장: 객체 종류별 (보스는 매 틱 등)
	void	ShowInView(Player& viewer, GameObject& other, float distSq, Protocol::S_SPAWN& spawnPkt, Protocol::S_MOVE& movePkt);	// 이번 틱에 보일 객체 하나
	template<typename Func>
	void	ForEachViewer(GameObject& object, Func&& func);					// object가 보이는 플레이어마다 func (object가 플레이어면 본인 포함)
	void	BroadcastToViewers(GameObject& object, SendBufferRef sendBuffer);	// object가 보이는 사람에게 (본인 포함)
	void	SendMoves(const PlayerRef& player, Protocol::S_MOVE& all);				// 큰 S_MOVE를 나눠서 한 명에게
	void	SendSpawns(const PlayerRef& player, const Protocol::S_SPAWN& all);	// 큰 S_SPAWN을 나눠서 한 명에게
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);	// 큐에 넣기만 (PushSend) + 이번 틱 보낼 목록에 기록
	void	FlushSends(FlushStats& out);						// 틱 맨 끝: 목록의 세션마다 FlushSend (WSASend 한 번) + 세션별 시간을 out에

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음
	xunordered_map<uint64, MonsterRef>	_monsters;	// 위와 같음 (Start에서 채움)
	xmap<uint64, DropRef>				_drops;		// 바닥 드랍. 정렬된 map: id 순 = 만든 순서 (id가 계속 커짐) -> begin()이 가장 오래된 드랍
	uint64	_dropLifetimeTicks = 60 * TICKS_PER_SECOND;	// 드랍 수명 (--drop-lifetime-sec, 테스트에서 짧게)
	int32	_maxDrops = DEFAULT_MAX_DROPS;				// 드랍 상한 (--max-drops, 테스트에서 작게)
	int32	_maxVisiblePlayers = DEFAULT_MAX_VISIBLE_PLAYERS;	// 보이는 다른 플레이어 상한 (--max-visible-players)
	xvector<GameSessionRef>	_pendingFlush;		// 이번 틱에 PushSend한 세션 (틱 끝 FlushSends가 비움)
	xvector<ViewCandidate>	_viewCandidates;	// SelectVisiblePlayers가 매번 다시 채우는 작업용 목록 (멤버로 둬서 플레이어마다, 틱마다 메모리를 새로 잡지 않음)
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
	int32	_loadBossType = 0;		// --load-boss <monster_id>. 0이면 없음

	// 시간 한 종류의 10초 통계 (Room 줄 안에서만 -> 락 없음)
	struct Span
	{
		uint64	count = 0;
		uint64	sumUs = 0;
		uint64	maxUs = 0;
		void	Add(int64 us);		// 음수는 0으로 (DBQueue::JobStats::Add와 같음)
	};
	// 입장 구간 다섯 조각: C_ENTER_GAME 받음 -(db)-> 불러옴 -(roomWait)-> Enter 시작 -(run)-> Enter 끝 -(sendWait)-> 송신
	//   total = 받음 ~ 송신 (서버 안에서 쓴 시간 전부). 클라의 enter_ms - total = 네트워크 + 클라 처리
	//   sendWait는 입장 응답을 틱 끝에 보내던 때(190d2fe까지)의 값. 지금은 Enter가 바로 보내서 0 (줄 모양은 비교용으로 유지)
	struct EnterStats
	{
		Span	db;
		Span	roomWait;
		Span	run;
		Span	sendWait;
		Span	total;
		// Enter 안 (run을 쪼갬) + 바로 송신: ProcessEnter / EnterView에서. [Room] enter cost 줄
		Span	enterPkt;		// ① _players, Grid에 넣기 + S_ENTER_GAME 만들기
		Span	select;			// ② SelectVisiblePlayers + 고른 사람을 aroundPkt에
		Span	notify;			// ③ 주변 플레이어에게 나 알리기 (보이는 플레이어 수는 Player::VisiblePlayerCount로 읽기만)
		Span	objects;		// ④ 몬스터, 드랍
		Span	spawnSend;		// ⑤ SendSpawns (나눠 담기 + 직렬화)
		Span	flush;			// ⑥ FlushSend (WSASend). run에는 안 들어감
		uint64	notifyOthers = 0;	// ③에서 확인한 주변 플레이어 수 (합)
	};

	// 입장은 틱 시작부터 이 시각까지만 (다음 틱 5ms 전). 400명이 몰려도 Enter들이 다음 틱을 밀지 않게
	//   (입장 구간 측정: Enter 400개가 Room 줄을 약 350ms 잡아 틱이 250ms 밀렸음)
	//   45'000: '는 숫자 구분자 (C++14, 값은 45000)
	static constexpr int64	ENTER_DEADLINE_US = 45'000;
	xdeque<PlayerRef>	_pendingEnters;		// 입장 대기 (들어온 순서. 뒤에 넣고 앞에서 빼서 deque)

	// 틱 7조각 (10초 통계): 1초마다 일 / 주기 저장 / 이동 / 몬스터 / 시야 / HP / 송신
	//   stats, save는 안 한 틱에도 0으로 들어감 -> 평균은 "틱당 평균", 볼 값은 최대
	static constexpr int32	TICK_PART_COUNT = 7;
	struct TickStats
	{
		Span	part[TICK_PART_COUNT];
		Span	lock[TICK_PART_COUNT];	// 그중 락을 기다린 시간 (송신이 락 대기인지 WSASend 자체인지 가르려고)
	};
	// 틱 끝 송신을 세션별로 잰 결과 (FlushSends가 채움 -> 가장 느린 틱 줄에 남김)
	//   "모두 조금씩 느렸는지" vs "몇 명이 크게 막혔는지"를 가르려고 최대값, 1ms 넘은 수, 그 합만 (세션마다 남기면 로그가 너무 큼)
	struct FlushStats
	{
		int32	sessions = 0;		// FlushSend를 부른 세션 수
		int32	closed = 0;			// 그중 이미 끊긴 세션 수
		int64	maxUs = 0;			// 세션 하나가 가장 오래 걸린 시간
		int32	slowCount = 0;		// 1ms 넘게 걸린 세션 수
		int64	slowSumUs = 0;		// 그 세션들이 걸린 시간 합
		int64	sumUs = 0;			// 모든 세션의 FlushSend 시간 합 (송신 구간 - 이 값 - clearUs = 루프 자체 비용)
		int64	clearUs = 0;		// 끝의 _pendingFlush.clear() 시간 (마지막 참조였던 세션은 여기서 소멸 -> closesocket)
	};
	// 10초 동안 가장 느린 틱 하나의 내역 (조각별 최대는 서로 다른 틱일 수 있어서 같은 틱의 값을 따로 남김)
	//   workUs(실제로 흐른 시간)와 cpuUs(이 스레드가 CPU를 쓴 시간)의 차이 = 틱이 일을 못 하고 멈춰 있던 시간
	struct SlowTick
	{
		uint64	tick = 0;
		int64	workUs = 0;
		int64	cpuUs = 0;							// GetThreadTimes 차이 (약 15.6ms 단위로만 올라감)
		int64	partUs[TICK_PART_COUNT] = {};
		int64	partLockUs[TICK_PART_COUNT] = {};	// 조각마다 그중 락을 기다린 시간
		uint32	cpuStart = 0;						// 시작할 때 CPU 번호 (시작 로그의 [Server] cpu efficiency class로 P / E코어를 봄)
		uint32	cpuEnd = 0;							// 끝날 때 CPU 번호
		uint64	viewChanges = 0;					// 그 틱의 S_SPAWN + S_DESPAWN 항목 수
		uint64	tickCycles = 0;						// 틱 동안 이 스레드가 CPU에서 돈 사이클 (QueryThreadCycleTime 차이)
		uint64	flushCycles = 0;					// 그중 송신(FlushSends) 동안
		FlushStats	flush;							// 송신을 세션별로 잰 결과
	};
	double	_cyclesPerUs = 0;		// 쉬지 않고 돌 때 1µs에 몇 사이클 (Start에서 한 번 잼). 사이클 / (시간 x 이 값) = CPU를 쓴 비율

	// 10초(200틱) 통계 -> LogStats에서 출력 후 0으로
	TickStats	_tickStats;
	SlowTick	_slowTick;
	EnterStats	_enterStats;			// ProcessEnter가 다섯 조각 모두 기록 (입장 응답을 바로 보내서 sendWait는 0, roomWait에 대기 목록 시간 포함)
	uint64	_enterDeferred = 0;		// 대기 목록으로 간 입장 수
	uint64	_enterQueueMax = 0;		// 대기 목록이 가장 길었을 때
	int64	_lastTickStartUs = 0;	// 지난 틱 시작 시각 (NowUs) -> 틱 사이 간격 (0이면 첫 틱)
	uint64	_intervalMaxUs = 0;		// 가장 길었던 틱 간격 (평균에 묻히는 한 번의 큰 밀림을 보려고)
	uint64	_workTimeSumUs = 0;		// 틱 처리 시간 합 (마이크로초)
	uint64	_workTimeMaxUs = 0;		// 가장 오래 걸린 틱
	uint64	_monsterMovesSent = 0;	// S_MOVE에 넣은 몬스터 항목 수
	uint64	_sentPackets = 0;		// 보낸 패킷 수 (SendTo 호출 수 = 받는 사람 수만큼)
	uint64	_sentBytes = 0;			// 보낸 바이트 수 (패킷 헤더 포함)
	uint64	_viewSpawned = 0;		// UpdateView에서 S_SPAWN에 넣은 항목 수 (새로 보이게 됨: 상한 안으로 들어옴 + 반경 안으로 들어옴)
	uint64	_viewDespawned = 0;		// UpdateView에서 S_DESPAWN에 넣은 항목 수 (안 보이게 됨: 상한 밖으로 밀림 + 반경 밖으로 나감)
	uint64	_skillHits = 0;			// 플레이어 스킬이 몬스터를 맞힌 횟수 (HandleSkill -> HitMonster)
	uint64	_skillWorkUs = 0;		// HandleSkill에 쓴 시간 (틱 밖에서 도는 잡이라 work에 안 잡힘. 보스전에서 브로드캐스트 비용을 보려고)
	SkillRejectStats	_skillRejects;	// 스킬 요청 거절 이유별 횟수
	TargetFailStats		_skillMisses;	// 빗나감 이유별 횟수 (S_SKILL target 0으로 나간 것)
};

extern RoomRef GRoom;
