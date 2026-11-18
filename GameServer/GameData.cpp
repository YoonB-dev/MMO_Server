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
static vector<int64>						s_expToNext;	// [level - 1] = 그 레벨의 필요 경험치 (레벨은 1부터 빠짐없이라 배열 번호로 바로 찾음)
static unordered_map<int32, ItemData>		s_items;

static bool LoadMonsters(DBConnection& db)
{
	MonsterData row;	// BindCol로 각 필드의 주소를 알려 두면, Fetch할 때마다 이번 줄의 값이 여기에 채워짐

	DBBind<0, 17> query(db,
		L"SELECT monster_id, patrol_range, patrol_speed, idle_min_sec, idle_max_sec,"
		L" detect_range, stop_range, chase_speed, lose_range, leash_range, return_speed, max_hp, skill_id,"
		L" level, exp_reward, gold_min, gold_max"
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
	query.BindCol(11, row.maxHp);
	query.BindCol(12, row.skillId);
	query.BindCol(13, row.level);
	query.BindCol(14, row.expReward);
	query.BindCol(15, row.goldMin);
	query.BindCol(16, row.goldMax);

	if (query.Execute() == false)
		return false;	// 이유는 바로 위 [ODBC Error] (테이블이 없으면 doesn't exist)

	while (query.Fetch())
	{
		// 테이블끼리 FK가 없어서(monster.sql 주석) 서버가 확인. 찾은 스킬은 포인터로 연결 -> Monster가 매번 찾지 않음
		row.skill = FindSkillData(row.skillId);
		if (row.skill == nullptr)
		{
			LogLine() << "[GameData] monster " << row.monsterId << ": unknown skill_id " << row.skillId;
			return false;
		}
		s_monsters[row.monsterId] = row;	// 복사해서 저장 (row는 다음 Fetch에서 덮어써짐)
	}

	return true;
}

static bool LoadItems(DBConnection& db)
{
	ItemData row;

	DBBind<0, 2> query(db, L"SELECT item_id, max_stack FROM data_item");
	query.BindCol(0, row.itemId);
	query.BindCol(1, row.maxStack);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
		s_items[row.itemId] = row;
	return true;
}

// data_monster_drop: 몬스터마다 드랍표를 MonsterData.drops에 붙임
//   monster_id는 DB FK가 막지만, item_id는 다른 파일(item.sql)이라 FK가 없어서 서버가 확인 (skill_id와 같은 이유)
static bool LoadMonsterDrops(DBConnection& db)
{
	int32 monsterId = 0;
	MonsterDropData row;

	DBBind<0, 5> query(db, L"SELECT monster_id, item_id, chance_permille, min_count, max_count FROM data_monster_drop ORDER BY monster_id, item_id");
	query.BindCol(0, monsterId);
	query.BindCol(1, row.itemId);
	query.BindCol(2, row.chancePermille);
	query.BindCol(3, row.minCount);
	query.BindCol(4, row.maxCount);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
	{
		if (FindItemData(row.itemId) == nullptr)
		{
			LogLine() << "[GameData] monster " << monsterId << " drop: unknown item_id " << row.itemId;
			return false;
		}
		auto monster = s_monsters.find(monsterId);
		if (monster == s_monsters.end())
		{
			LogLine() << "[GameData] drop for unknown monster_id " << monsterId;
			return false;
		}
		monster->second.drops.push_back(row);
	}
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
			LogLine() << "[GameData] spawn " << row.spawnId << ": unknown monster_id " << row.monsterId;
			return false;
		}
		s_spawns.push_back(row);
	}
	return true;
}

static bool LoadSkills(DBConnection& db)
{
	SkillData row;
	int32 requiresTarget = 0;	// TINYINT(0/1). DBBind에 bool 바인딩이 없어서 int32로 받아 bool로 바꿈

	DBBind<0, 8> query(db,
		L"SELECT skill_id, cooldown_ms, range_m, damage, mp_cost, requires_target, cast_ms, hit_angle_deg FROM data_skill");
	query.BindCol(0, row.skillId);
	query.BindCol(1, row.cooldownMs);
	query.BindCol(2, row.range);
	query.BindCol(3, row.damage);
	query.BindCol(4, row.mpCost);
	query.BindCol(5, requiresTarget);
	query.BindCol(6, row.castMs);
	query.BindCol(7, row.hitAngleDeg);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
	{
		row.requiresTarget = (requiresTarget != 0);
		s_skills[row.skillId] = row;
	}

	return true;
}

// data_level: 줄 사이 규칙은 CHECK로 못 막아서(level.sql 주석) 여기서 확인
//   1부터 빠짐없이, 마지막 줄만 0 (최대 레벨), 나머지는 0보다 큼
static bool LoadLevels(DBConnection& db)
{
	int32 level = 0;
	int64 expToNext = 0;

	DBBind<0, 2> query(db, L"SELECT level, exp_to_next FROM data_level ORDER BY level");
	query.BindCol(0, level);
	query.BindCol(1, expToNext);

	if (query.Execute() == false)
		return false;

	while (query.Fetch())
	{
		if (level != static_cast<int32>(s_expToNext.size()) + 1)
		{
			LogLine() << "[GameData] data_level: level " << level << " is out of order (levels must start at 1 with no gaps)";
			return false;
		}
		s_expToNext.push_back(expToNext);
	}

	if (s_expToNext.empty())
	{
		LogLine() << "[GameData] data_level is empty";
		return false;
	}
	if (s_expToNext.back() != 0)
	{
		LogLine() << "[GameData] data_level: the last level " << s_expToNext.size() << " must have exp_to_next 0 (max level)";
		return false;
	}
	for (size_t i = 0; i + 1 < s_expToNext.size(); i++)
	{
		if (s_expToNext[i] <= 0)
		{
			LogLine() << "[GameData] data_level: level " << (i + 1) << " has exp_to_next 0 but is not the last level";
			return false;
		}
	}
	return true;
}

bool LoadGameData()
{
	DBConnectionGuard db(GDBConnectionPool);
	if (!db)
	{
		LogLine() << "[GameData] no free DB connection";
		return false;
	}

	// 순서 중요: 몬스터가 스킬을, 드랍표가 아이템과 몬스터를, 스폰이 몬스터 종류를 확인
	//   스킬 -> 레벨 -> 아이템 -> 몬스터 -> 몬스터 드랍 -> 스폰
	if (LoadSkills(db) == false || LoadLevels(db) == false || LoadItems(db) == false || LoadMonsters(db) == false
		|| LoadMonsterDrops(db) == false || LoadMonsterSpawns(db) == false)
	{
		LogLine() << "[GameData] failed to load (run Database/Setup-Database.ps1 to create data tables)";
		return false;
	}

	LogLine() << "[GameData] loaded " << s_monsters.size() << " monsters, " << s_spawns.size() << " spawns, "
		<< s_skills.size() << " skills, " << s_expToNext.size() << " levels, " << s_items.size() << " items";
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

const ItemData* FindItemData(int32 itemId)
{
	auto it = s_items.find(itemId);
	return (it != s_items.end()) ? &it->second : nullptr;
}

int32 GetMaxLevel()
{
	return static_cast<int32>(s_expToNext.size());
}

int64 GetExpToNext(int32 level)
{
	if (level < 1 || level > GetMaxLevel())
		return 0;
	return s_expToNext[level - 1];
}
