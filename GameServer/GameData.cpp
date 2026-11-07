#include "pch.h"
#include "GameData.h"
#include "DBConnectionPool.h"
#include "DBBind.h"

// 서버 시작 때 한 번 채우고 이후 읽기만 함
//   x 컨테이너(xvector 등, GMemory 사용) 대신 std를 쓰는 이유: 시작에 실패해서 main이 끝나면 전역 변수가 지워지는데,
//   파일끼리의 소멸 순서는 정해져 있지 않아서 GMemory가 먼저 지워진 뒤 이 컨테이너가 메모리를 돌려주려 할 수 있음
static unordered_map<int32, MonsterData>	s_monsters;
static vector<MonsterSpawnData>				s_spawns;
static unordered_map<int32, SkillData>		s_skills;

static bool LoadMonsters(DBConnection& db)
{
	MonsterData row;	// BindCol로 각 필드의 주소를 알려 두면, Fetch할 때마다 이번 줄의 값이 여기에 채워짐

	DBBind<0, 12> query(db,
		L"SELECT monster_id, patrol_range, patrol_speed, idle_min_sec, idle_max_sec,"
		L" detect_range, stop_range, chase_speed, lose_range, leash_range, return_speed, max_hp"
		L" FROM data_monster");
	query.BindCol(0, row.monsterId);
	query.BindCol(1, row.patrolRange);
	query.BindCol(2, row.patrolSpeed);
	query.BindCol(3, row.idleMinSec);
	query.BindCol(4, row.idleMaxSec);
	query.BindCol(5, row.detectRange);
	query.BindCol(6, row.stopRange);
	query.BindCol(7, row.chaseSpeed);
	query.BindCol(8, row.loseRange);
	query.BindCol(9, row.leashRange);
	query.BindCol(10, row.returnSpeed);
	query.BindCol(11, row.maxHp);	// 맨 뒤에 붙여서 기존 번호(0~10)는 그대로. 필드 순서와 SELECT 순서는 같을 필요 없음

	if (query.Execute() == false)
		return false;	// 이유는 바로 위 [ODBC Error] (테이블이 없으면 doesn't exist)

	while (query.Fetch())
		s_monsters[row.monsterId] = row;	// 복사해서 저장 (row는 다음 Fetch에서 덮어써짐)

	return true;
}

static bool LoadMonsterSpawns(DBConnection& db)
{
	MonsterSpawnData row;

	DBBind<0, 5> query(db, L"SELECT spawn_id, monster_id, pos_x, pos_z, respawn_ms FROM data_monster_spawn ORDER BY spawn_id");
	query.BindCol(0, row.spawnId);
	query.BindCol(1, row.monsterId);
	query.BindCol(2, row.x);
	query.BindCol(3, row.z);
	query.BindCol(4, row.respawnMs);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
	{
		// DB의 FK가 이미 막지만, 서버도 한 번 더 확인 (SpawnMonsters가 nullptr을 받지 않게)
		if (FindMonsterData(row.monsterId) == nullptr)
		{
			cout << "[GameData] spawn " << row.spawnId << ": unknown monster_id " << row.monsterId << endl;
			return false;
		}
		s_spawns.push_back(row);
	}
	return true;
}

static bool LoadSkills(DBConnection& db)
{
	SkillData row;

	DBBind<0, 4> query(db, L"SELECT skill_id, cooldown_ms, range_m, damage FROM data_skill");
	query.BindCol(0, row.skillId);
	query.BindCol(1, row.cooldownMs);
	query.BindCol(2, row.range);
	query.BindCol(3, row.damage);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
		s_skills[row.skillId] = row;

	return true;
}

bool LoadGameData()
{
	DBConnectionGuard db(GDBConnectionPool);
	if (!db)
	{
		cout << "[GameData] no free DB connection" << endl;
		return false;
	}

	// 순서 중요: 스폰이 몬스터 종류를 확인하므로 종류 먼저
	if (LoadMonsters(db) == false || LoadMonsterSpawns(db) == false || LoadSkills(db) == false)
	{
		cout << "[GameData] failed to load (run Database/Setup-Database.ps1 to create data tables)" << endl;
		return false;
	}

	cout << "[GameData] loaded " << s_monsters.size() << " monsters, " << s_spawns.size() << " spawns, "
		<< s_skills.size() << " skills" << endl;
	return true;
}

const MonsterData* FindMonsterData(int32 monsterId)
{
	auto it = s_monsters.find(monsterId);
	return (it != s_monsters.end()) ? &it->second : nullptr;
}

const vector<MonsterSpawnData>& GetMonsterSpawns()
{
	return s_spawns;
}

const SkillData* FindSkillData(int32 skillId)
{
	auto it = s_skills.find(skillId);
	return (it != s_skills.end()) ? &it->second : nullptr;
}
