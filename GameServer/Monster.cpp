#include "pch.h"
#include "Monster.h"
#include "GameData.h"
#include "Room.h"
#include "Player.h"
#include <cmath>
#include <random>
#include <algorithm>	// std::find

// [min, max] 사이 랜덤 실수. 엔진은 스레드마다 하나 (thread_local)
//   -> Room 틱이 어느 워커 스레드에서 돌든, 다른 스레드와 같은 엔진을 동시에 건드리지 않음
static float RandomRange(float min, float max)
{
	thread_local std::mt19937 engine{ std::random_device{}() };
	std::uniform_real_distribution<float> dist(min, max);
	return dist(engine);
}

static const char* ToString(MonsterState state)
{
	switch (state)
	{
	case MonsterState::Idle:	return "Idle";
	case MonsterState::Patrol:	return "Patrol";
	case MonsterState::Chase:	return "Chase";
	case MonsterState::Attack:	return "Attack";
	case MonsterState::Return:	return "Return";
	}
	return "?";
}

// 참조 멤버(_data)는 초기화 리스트에서만 정할 수 있음 (멤버 선언 순서와 같은 순서로)
Monster::Monster(const MonsterData& data, const MonsterSpawnData& spawn)
	: GameObject(Protocol::OBJECT_TYPE_MONSTER), _data(data), _spawn(spawn)
{
	GetPosInfo()->set_x(spawn.x);
	GetPosInfo()->set_z(spawn.z);
	GetStatInfo()->set_hp(_data.maxHp);		// DB(data_monster.max_hp)
	GetStatInfo()->set_max_hp(_data.maxHp);
	GetStatInfo()->set_level(_data.level);		// 이름표 레벨 (data_monster.level, 표시용)
}

void Monster::Update(Room& room, float dt)
{
	if (_attackCooldown > 0.f)
		_attackCooldown -= dt;

	switch (_state)
	{
	case MonsterState::Idle:	UpdateIdle(room, dt);	break;
	case MonsterState::Patrol:	UpdatePatrol(room, dt);	break;
	case MonsterState::Chase:	UpdateChase(room, dt);	break;
	case MonsterState::Attack:	UpdateAttack(room, dt);	break;
	case MonsterState::Return:	UpdateReturn(room, dt);	break;
	}
}

void Monster::UpdateIdle(Room& room, float dt)
{
	if (TryDetect(room))
		return;

	_idleTime -= dt;
	if (_idleTime <= 0.f)
		SetState(MonsterState::Patrol);
}

void Monster::UpdatePatrol(Room& room, float dt)
{
	if (TryDetect(room))
		return;

	if (MoveTowards(_destX, _destZ, _data.patrolSpeed, dt))
		SetState(MonsterState::Idle);
}

void Monster::UpdateChase(Room& room, float dt)
{
	PlayerRef target = room.FindPlayer(_targetId);
	if (target == nullptr)
		return LoseTarget(room, "left");	// 타겟이 방을 나감 -> 다른 상대가 있으면 계속 싸움
	if (target->IsDead())
		return LoseTarget(room, "dead");	// 타겟이 죽음 (시체는 안 쫓음) -> 같이 때리던 사람이 있으면 그쪽으로

	// 스폰에서 너무 멀리 끌려왔으면 포기 (몬스터를 맵 끝까지 끌고 가는 것 방지)
	const Protocol::PosInfo* pos = GetPosInfo();
	const float sx = pos->x() - _spawn.x;
	const float sz = pos->z() - _spawn.z;
	if (sx * sx + sz * sz > _data.leashRange * _data.leashRange)
		return GiveUpChase("leash");	// leash는 다른 상대를 찾지 않고 무조건 돌아감 (끌고 다니기 방지)

	const Protocol::PosInfo* targetPos = target->GetPosInfo();
	const float dx = targetPos->x() - pos->x();
	const float dz = targetPos->z() - pos->z();
	const float distSq = dx * dx + dz * dz;

	if (distSq > _data.loseRange * _data.loseRange)
		return LoseTarget(room, "lost");	// 타겟이 너무 멀어짐 -> 근처에 다른 상대가 있으면 그쪽으로

	// 사거리 안이고 쿨타임이 끝났으면 예고 공격 시작
	const SkillData& skill = *_data.skill;
	if (_attackCooldown <= 0.f && distSq <= skill.range * skill.range)
		return StartAttack(room, dx, dz);

	// 충분히 붙었으면 그 자리에서 기다림 (타겟이 멀어지면 다음 틱에 다시 쫓음)
	if (distSq <= _data.stopRange * _data.stopRange)
	{
		StopMove();
		return;
	}

	MoveTowards(targetPos->x(), targetPos->z(), _data.chaseSpeed, dt);
}

// 예고 공격 시작: 멈추고, 타겟 쪽으로 방향을 고정하고, 모두에게 예고(S_SKILL, cast_ms)를 알림
//   쿨타임은 시작할 때부터 셈 (예고 시간도 쿨타임에 포함)
void Monster::StartAttack(Room& room, float dx, float dz)
{
	StopMove();

	const float dist = std::sqrt(dx * dx + dz * dz);
	if (dist > 0.01f)
	{
		_castDirX = dx / dist;
		_castDirZ = dz / dist;
	}
	else
	{
		// 겹쳐 있으면 지금 보는 방향 그대로 (0으로 나누기 방지)
		const float yawRad = GetPosInfo()->yaw() * DEG_TO_RAD;
		_castDirX = std::sin(yawRad);
		_castDirZ = std::cos(yawRad);
	}
	GetPosInfo()->set_yaw(std::atan2(_castDirX, _castDirZ) * RAD_TO_DEG);

	const SkillData& skill = *_data.skill;
	_castRemaining = skill.castMs / 1000.f;
	_attackCooldown = skill.cooldownMs / 1000.f;

	SetState(MonsterState::Attack);

	// 지금 보는 사람에게는 아래 S_SKILL로, 나중에 보게 되는 사람에게는 S_SPAWN의 cast_info로 감
	Protocol::CastInfo* cast = GetObjectInfo().mutable_cast_info();
	cast->set_skill_id(skill.skillId);
	cast->set_remaining_ms(skill.castMs);
	cast->set_total_ms(skill.castMs);
	cast->set_yaw(GetPosInfo()->yaw());
	cast->set_range(skill.range);
	cast->set_hit_angle(skill.hitAngleDeg);

	room.BroadcastSkill(*this, skill, _targetId);
}

// 예고 중: 움직이지 않고 기다렸다가, 끝나는 순간 고정 방향 부채꼴로 판정
//   시간은 DoTimer가 아니라 틱의 dt로 잼 -> 시전 중 몬스터가 죽으면 Update가 안 불려서 판정도 자연히 사라짐
void Monster::UpdateAttack(Room& room, float dt)
{
	PlayerRef target = room.FindPlayer(_targetId);
	if (target == nullptr)
		return LoseTarget(room, "left");	// 시전 취소 -> 다른 상대가 있으면 그쪽으로
	if (target->IsDead())
		return LoseTarget(room, "dead");	// 다른 몬스터에게 죽음 -> 시전 취소, 다른 상대가 있으면 그쪽으로

	_castRemaining -= dt;

	// S_SPAWN 복사본이 항상 지금 값이 되게 (int 하나 쓰기라 매 틱 해도 비용 없음)
	GetObjectInfo().mutable_cast_info()->set_remaining_ms(static_cast<int32>(_castRemaining * 1000.f));

	if (_castRemaining > 0.f)
		return;

	const bool hit = IsInHitArea(*target->GetPosInfo());
	LogLine() << "[Monster " << GetObjectId() << "] attack " << (hit ? "hit" : "miss") << " target " << _targetId;
	if (hit)
		room.HitPlayer(target, GetObjectId(), _data.skill->damage);	// HP 0이면 Room이 죽음 처리

	SetState(MonsterState::Chase);
}

// 고정 방향 부채꼴 판정: 거리 <= 사거리, 고정 방향과 타겟 사이 각 <= 반각
//   단위 벡터끼리의 내적 = cos(사이각) -> acos 없이 cos끼리 비교 (사이각이 작을수록 cos가 큼)
bool Monster::IsInHitArea(const Protocol::PosInfo& targetPos)
{
	const SkillData& skill = *_data.skill;
	const Protocol::PosInfo* pos = GetPosInfo();
	const float dx = targetPos.x() - pos->x();
	const float dz = targetPos.z() - pos->z();
	const float dist = std::sqrt(dx * dx + dz * dz);
	if (dist > skill.range)
		return false;
	if (dist < 0.01f)
		return true;	// 겹쳐 있으면 방향과 상관없이 맞음 (0으로 나누기 방지)

	const float cosToTarget = (dx * _castDirX + dz * _castDirZ) / dist;
	return cosToTarget >= std::cos(skill.hitAngleDeg * DEG_TO_RAD);
}

// 스폰으로 돌아감. TryDetect를 안 해서 플레이어를 무시하고, 공격은 Room이 IsInvulnerable로 거절
//   도착하면 HP를 다 채움 -> 끌고 가서 깎아 둔 HP로 이득을 볼 수 없음
void Monster::UpdateReturn(Room& room, float dt)
{
	if (MoveTowards(_spawn.x, _spawn.z, _data.returnSpeed, dt) == false)
		return;

	Protocol::StatInfo* stat = GetStatInfo();
	if (stat->hp() < stat->max_hp())
		room.SetHp(*this, stat->max_hp());	// 회복: 틱 끝에 보는 사람에게 HP바 (damage 0, attacker 0)
	SetState(MonsterState::Idle);
}

// 맞았을 때 (어그로): 때린 사람을 기억 (상태와 상관없이) + Idle/Patrol이면 그 사람을 쫓음 -> 감지 거리 밖에서 원거리로 맞아도 반응
//   Chase면 지금 타겟 유지 (여러 명이 때릴 때 이리저리 바꾸지 않게). Return은 무적이라 여기까지 안 옴
//   기억한 사람은 타겟을 잃었을 때 다음 상대 후보 (FindNextTarget)
void Monster::OnDamaged(uint64 attackerId)
{
	// std::find: 앞에서부터 찾아서 있으면 그 위치, 없으면 end() -> 처음 때린 사람만 추가
	if (std::find(_attackers.begin(), _attackers.end(), attackerId) == _attackers.end())
		_attackers.push_back(attackerId);

	if (_state != MonsterState::Idle && _state != MonsterState::Patrol)
		return;

	LogLine() << "[Monster " << GetObjectId() << "] aggro on " << attackerId;
	_targetId = attackerId;
	SetState(MonsterState::Chase);	// 다음 틱 UpdateChase가 FindPlayer(_targetId)로 쫓기 시작
}

// Idle/Patrol 공통: 감지 거리 안의 가장 가까운 플레이어를 타겟으로 Chase 시작. 찾았으면 true
bool Monster::TryDetect(Room& room)
{
	const Protocol::PosInfo* pos = GetPosInfo();
	PlayerRef player = room.FindNearestPlayer(pos->x(), pos->z(), _data.detectRange);
	if (player == nullptr)
		return false;

	_targetId = player->GetObjectId();
	SetState(MonsterState::Chase);
	return true;
}

// 타겟을 잃었을 때 (dead / left / lost): 다른 상대를 찾아서 계속 싸움. 아무도 없을 때만 스폰으로
//   -> 같이 잡다가 한 명이 죽어도 돌아가서 풀피가 되지 않음
void Monster::LoseTarget(Room& room, const char* reason)
{
	PlayerRef next = FindNextTarget(room);
	if (next == nullptr)
		return GiveUpChase(reason);		// 아무도 없음 -> 지금처럼 돌아감

	LogLine() << "[Monster " << GetObjectId() << "] retarget " << _targetId << " -> " << next->GetObjectId() << " (" << reason << ")";
	_targetId = next->GetObjectId();
	SetState(MonsterState::Chase);		// Attack 중이었으면 SetState가 시전 정보를 지움 (시전 취소)
}

// 다음 상대: 1) 나를 때린 사람 중 살아 있고 lose_range 안에서 가장 가까운 사람
//            2) 없으면 감지 거리 안의 가장 가까운 사람 (FindNearestPlayer는 시체를 이미 뺌)
//   1)을 먼저 보는 이유: 원거리 스킬(10m)은 감지 거리(8m) 밖에서도 때림 -> 같이 싸우던 사람을 놓치지 않게
PlayerRef Monster::FindNextTarget(Room& room)
{
	const Protocol::PosInfo* pos = GetPosInfo();
	PlayerRef best;
	float bestSq = _data.loseRange * _data.loseRange;
	for (const uint64 id : _attackers)
	{
		PlayerRef player = room.FindPlayer(id);
		if (player == nullptr || player->IsDead() || id == _targetId)
			continue;	// 나갔거나, 죽었거나, 방금 잃은 타겟
		const float dx = player->GetPosInfo()->x() - pos->x();
		const float dz = player->GetPosInfo()->z() - pos->z();
		const float distSq = dx * dx + dz * dz;
		if (distSq <= bestSq)
		{
			best = player;
			bestSq = distSq;
		}
	}
	if (best != nullptr)
		return best;

	PlayerRef nearby = room.FindNearestPlayer(pos->x(), pos->z(), _data.detectRange);
	if (nearby != nullptr && nearby->GetObjectId() == _targetId)
		return nullptr;		// 감지 거리 안에 잃은 타겟만 있음 (lost는 12m 밖이라 실제로는 없지만, 같은 사람을 다시 잡지 않게)
	return nearby;
}

// Chase를 포기하고 스폰으로 (reason은 로그용: left / dead / leash / lost)
void Monster::GiveUpChase(const char* reason)
{
	LogLine() << "[Monster " << GetObjectId() << "] give up target " << _targetId << " (" << reason << ")";
	SetState(MonsterState::Return);
}

// 상태를 바꾸고, 새 상태를 시작할 때 할 일(대기 시간, 목적지 정하기)을 여기 한 곳에서 처리
void Monster::SetState(MonsterState next)
{
	const MonsterState prev = _state;
	_state = next;

	// 시전이 끝나는 길(판정 후 Chase, 취소 후 Return)이 여러 개라도 지우는 곳은 여기 하나
	if (prev == MonsterState::Attack && next != MonsterState::Attack)
		GetObjectInfo().clear_cast_info();

	switch (next)
	{
	case MonsterState::Idle:
		_idleTime = RandomRange(_data.idleMinSec, _data.idleMaxSec);
		break;
	case MonsterState::Patrol:
		_destX = _spawn.x + RandomRange(-_data.patrolRange, _data.patrolRange);
		_destZ = _spawn.z + RandomRange(-_data.patrolRange, _data.patrolRange);
		break;
	case MonsterState::Chase:
		break;	// 타겟(_targetId)은 TryDetect / OnDamaged가 정해 둠
	case MonsterState::Attack:
		break;	// 방향, 예고 시간은 StartAttack이 정해 둠
	case MonsterState::Return:
		_targetId = 0;	// 타겟을 잊음
		_attackers.clear();	// 싸움이 끝났으니 때린 사람 기억도 비움
		break;
	}

	Protocol::PosInfo* pos = GetPosInfo();
	LogLine() << "[Monster " << GetObjectId() << "] " << ToString(prev) << " -> " << ToString(next)
		<< " at (" << pos->x() << ", " << pos->z() << ")"
		<< (next == MonsterState::Chase || next == MonsterState::Attack ? " target " + to_string(_targetId) : "");
}

// (x, z)를 향해 speed로 한 틱만큼 이동. 이번 틱에 도착하면 그 자리에 멈추고 true
bool Monster::MoveTowards(float x, float z, float speed, float dt)
{
	Protocol::MoveInfo* moveInfo = GetMoveInfo();
	Protocol::PosInfo* pos = moveInfo->mutable_pos_info();

	const float dx = x - pos->x();
	const float dz = z - pos->z();
	const float dist = std::sqrt(dx * dx + dz * dz);
	const float step = speed * dt;

	// 남은 거리가 한 걸음 이하 -> 지나치지 않게 목표에 딱 맞추고 멈춤 (dist가 0이어도 여기서 끝나서 0으로 나누지 않음)
	if (dist <= step)
	{
		pos->set_x(x);
		pos->set_z(z);
		StopMove();
		return true;
	}

	const float dirX = dx / dist;
	const float dirZ = dz / dist;
	moveInfo->set_dir_x(dirX);
	moveInfo->set_dir_z(dirZ);
	moveInfo->set_speed(speed);
	moveInfo->set_state(Protocol::MOVE_STATE_MOVING);
	pos->set_yaw(std::atan2(dirX, dirZ) * RAD_TO_DEG);	// 플레이어와 같은 기준 (+z = 0도)

	pos->set_x(pos->x() + dirX * step);
	pos->set_z(pos->z() + dirZ * step);
	return false;
}

void Monster::StopMove()
{
	Protocol::MoveInfo* moveInfo = GetMoveInfo();
	moveInfo->set_dir_x(0.f);
	moveInfo->set_dir_z(0.f);
	moveInfo->set_speed(0.f);
	moveInfo->set_state(Protocol::MOVE_STATE_IDLE);
}
