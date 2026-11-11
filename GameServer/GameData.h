#pragma once

// 기획 데이터 (DB의 data_ 테이블). 서버 시작 때 main에서 한 번 읽고, 그 뒤로는 읽기만 함
//   -> 워커 스레드가 뜨기 전에 다 채우므로 여러 스레드가 락 없이 읽어도 안전

struct SkillData;

constexpr int32 MONSTER_SKILL_ID_BEGIN = 101;	// 이 번호부터는 몬스터 스킬 (플레이어 C_SKILL은 거절). skill.sql과 맞춤

// data_monster 한 줄: 몬스터 종류별 AI 수치 (거리 m, 속도 m/s, 시간 초)
struct MonsterData
{
	int32	monsterId = 0;
	int32	maxHp = 0;			// 스폰할 때 HP
	float	patrolRange = 0.f;	// 스폰 기준 ±범위 안에서 배회
	float	patrolSpeed = 0.f;
	float	idleMinSec = 0.f;	// 배회 사이 대기 시간
	float	idleMaxSec = 0.f;
	float	detectRange = 0.f;	// 이 안에 플레이어가 들어오면 Chase
	float	stopRange = 0.f;	// 타겟과 이만큼 가까우면 멈춤 (겹치지 않게)
	float	chaseSpeed = 0.f;
	float	loseRange = 0.f;	// 타겟이 이보다 멀어지면 포기
	float	leashRange = 0.f;	// 스폰에서 이보다 멀리 끌려오면 포기
	float	returnSpeed = 0.f;
	int32	skillId = 0;				// 공격 스킬 (data_skill)
	const SkillData*	skill = nullptr;	// 로드할 때 skillId로 찾아서 연결해 둠 (s_skills 원소, 주소가 안 바뀜)
};

// data_monster_spawn 한 줄: 어디에 어떤 몬스터가 나오나
struct MonsterSpawnData
{
	int32	spawnId = 0;
	int32	monsterId = 0;
	float	x = 0.f;
	float	z = 0.f;
	int32	respawnMs = 0;	// 죽은 뒤 같은 자리에 다시 나오기까지 (ms)
};

// data_skill 한 줄: 스킬 하나의 고정 수치. 클라도 같은 skill_id 번호를 씀
struct SkillData
{
	int32	skillId = 0;
	int32	cooldownMs = 0;	// 다시 쓸 수 있을 때까지 (ms). Room이 틱 수로 바꿔서 씀
	float	range = 0.f;	// 사거리 (m). 서버 위치 기준, Room이 지연 여유를 더해서 검사
	int32	damage = 0;
	int32	mpCost = 0;		// 쓸 때 드는 MP
	bool	requiresTarget = false;	// true: 대상이 없거나 잘못되면 거절, false: 빗나감으로 나감 (허공 공격)
	int32	castMs = 0;				// 예고 시간 (0 = 즉시). 지나면 판정
	float	hitAngleDeg = 0.f;		// 예고 공격 판정 부채꼴의 좌우 반각 (도)
};

bool							LoadGameData();						// main에서 DB 접속 후 한 번. 실패하면 이유를 찍고 false
const MonsterData*				FindMonsterData(int32 monsterId);	// 없으면 nullptr
const vector<MonsterSpawnData>&	GetMonsterSpawns();
const SkillData*				FindSkillData(int32 skillId);		// 없으면 nullptr
