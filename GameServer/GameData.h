#pragma once

// 기획 데이터 (DB의 data_ 테이블). 서버 시작 때 main에서 한 번 읽고, 그 뒤로는 읽기만 함
//   -> 워커 스레드가 뜨기 전에 다 채우므로 여러 스레드가 락 없이 읽어도 안전

// data_monster 한 줄: 몬스터 종류별 AI 수치 (거리 m, 속도 m/s, 시간 초)
struct MonsterData
{
	int32	monsterId = 0;
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
};

// data_monster_spawn 한 줄: 어디에 어떤 몬스터가 나오나
struct MonsterSpawnData
{
	int32	spawnId = 0;
	int32	monsterId = 0;
	float	x = 0.f;
	float	z = 0.f;
};

bool							LoadGameData();						// main에서 DB 접속 후 한 번. 실패하면 이유를 찍고 false
const MonsterData*				FindMonsterData(int32 monsterId);	// 없으면 nullptr
const vector<MonsterSpawnData>&	GetMonsterSpawns();
