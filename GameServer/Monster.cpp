#include "pch.h"
#include "Monster.h"
#include <cmath>
#include <random>

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
	}
	return "?";
}

Monster::Monster(float x, float z)
	: GameObject(Protocol::OBJECT_TYPE_MONSTER), _spawnX(x), _spawnZ(z)
{
	GetPosInfo()->set_x(x);
	GetPosInfo()->set_z(z);
}

bool Monster::Update(float dt)
{
	const Protocol::MoveState before = GetMoveInfo()->state();

	switch (_state)
	{
	case MonsterState::Idle:	UpdateIdle(dt);		break;
	case MonsterState::Patrol:	UpdatePatrol(dt);	break;
	}

	return GetMoveInfo()->state() != before;
}

void Monster::UpdateIdle(float dt)
{
	_idleTime -= dt;
	if (_idleTime <= 0.f)
		SetState(MonsterState::Patrol);
}

void Monster::UpdatePatrol(float dt)
{
	if (MoveTowards(_destX, _destZ, PATROL_SPEED, dt))
		SetState(MonsterState::Idle);
}

// 상태를 바꾸고, 새 상태를 시작할 때 할 일(대기 시간, 목적지 정하기)을 여기 한 곳에서 처리
void Monster::SetState(MonsterState next)
{
	const MonsterState prev = _state;
	_state = next;

	switch (next)
	{
	case MonsterState::Idle:
		_idleTime = RandomRange(IDLE_MIN_TIME, IDLE_MAX_TIME);
		break;
	case MonsterState::Patrol:
		_destX = _spawnX + RandomRange(-PATROL_RANGE, PATROL_RANGE);
		_destZ = _spawnZ + RandomRange(-PATROL_RANGE, PATROL_RANGE);
		break;
	}

	Protocol::PosInfo* pos = GetPosInfo();
	cout << "[Monster " << GetObjectId() << "] " << ToString(prev) << " -> " << ToString(next)
		<< " at (" << pos->x() << ", " << pos->z() << ")" << endl;
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
		moveInfo->set_dir_x(0.f);
		moveInfo->set_dir_z(0.f);
		moveInfo->set_speed(0.f);
		moveInfo->set_state(Protocol::MOVE_STATE_IDLE);
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
