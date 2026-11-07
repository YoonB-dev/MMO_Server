#include "pch.h"
#include "Monster.h"
#include "GameData.h"
#include "Room.h"
#include "Player.h"
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
	case MonsterState::Chase:	return "Chase";
	case MonsterState::Return:	return "Return";
	}
	return "?";
}

// _data는 참조라서 나중에 대입할 수 없음 -> 초기화 리스트에서만 정할 수 있음 (멤버 선언 순서와 같은 순서로)
Monster::Monster(const MonsterData& data, const MonsterSpawnData& spawn)
	: GameObject(Protocol::OBJECT_TYPE_MONSTER), _data(data), _spawn(spawn)
{
	GetPosInfo()->set_x(spawn.x);
	GetPosInfo()->set_z(spawn.z);
	GetStatInfo()->set_hp(_data.maxHp);		// DB(data_monster.max_hp)
	GetStatInfo()->set_max_hp(_data.maxHp);
}

void Monster::Update(Room& room, float dt)
{
	switch (_state)
	{
	case MonsterState::Idle:	UpdateIdle(room, dt);	break;
	case MonsterState::Patrol:	UpdatePatrol(room, dt);	break;
	case MonsterState::Chase:	UpdateChase(room, dt);	break;
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
		return GiveUpChase("left");		// 타겟이 방을 나감

	// 스폰에서 너무 멀리 끌려왔으면 포기 (몬스터를 맵 끝까지 끌고 가는 것 방지)
	const Protocol::PosInfo* pos = GetPosInfo();
	const float sx = pos->x() - _spawn.x;
	const float sz = pos->z() - _spawn.z;
	if (sx * sx + sz * sz > _data.leashRange * _data.leashRange)
		return GiveUpChase("leash");

	const Protocol::PosInfo* targetPos = target->GetPosInfo();
	const float dx = targetPos->x() - pos->x();
	const float dz = targetPos->z() - pos->z();
	const float distSq = dx * dx + dz * dz;

	if (distSq > _data.loseRange * _data.loseRange)
		return GiveUpChase("lost");		// 타겟이 너무 멀어짐

	// 충분히 붙었으면 그 자리에서 기다림 (타겟이 멀어지면 다음 틱에 다시 쫓음)
	if (distSq <= _data.stopRange * _data.stopRange)
	{
		StopMove();
		return;
	}

	MoveTowards(targetPos->x(), targetPos->z(), _data.chaseSpeed, dt);
}

// 스폰으로 돌아감. 이 동안은 TryDetect를 안 해서 플레이어를 무시하고 (돌아가다 다시 붙는 것 방지),
//   맞지도 않음 (Room::HandleSkill이 IsInvulnerable로 거절)
//   도착하면 HP를 다 채움 -> 끌고 가서 깎아 둔 HP로 이득을 볼 수 없음
void Monster::UpdateReturn(Room& room, float dt)
{
	if (MoveTowards(_spawn.x, _spawn.z, _data.returnSpeed, dt) == false)
		return;

	Protocol::StatInfo* stat = GetStatInfo();
	if (stat->hp() < stat->max_hp())
	{
		stat->set_hp(stat->max_hp());
		room.BroadcastHp(*this, 0, 0);	// 회복: damage 0, attacker 0. *this(Monster)를 GameObject&로 넘김
	}
	SetState(MonsterState::Idle);	// 도착 -> 다시 Seek
}

// 맞았을 때 반응: 아직 아무도 안 쫓고 있으면(Idle/Patrol) 때린 사람을 쫓음 -> 감지 거리(8m) 밖에서 원거리로 맞아도 반응
//   Chase면 지금 타겟 유지 (여러 명이 때릴 때 이리저리 바꾸지 않게). Return은 무적이라 여기까지 안 옴
void Monster::OnDamaged(uint64 attackerId)
{
	if (_state != MonsterState::Idle && _state != MonsterState::Patrol)
		return;

	cout << "[Monster " << GetObjectId() << "] aggro on " << attackerId << endl;
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

// Chase를 포기하고 스폰으로 (reason은 로그용: left / leash / lost)
void Monster::GiveUpChase(const char* reason)
{
	cout << "[Monster " << GetObjectId() << "] give up target " << _targetId << " (" << reason << ")" << endl;
	SetState(MonsterState::Return);
}

// 상태를 바꾸고, 새 상태를 시작할 때 할 일(대기 시간, 목적지 정하기)을 여기 한 곳에서 처리
void Monster::SetState(MonsterState next)
{
	const MonsterState prev = _state;
	_state = next;

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
		break;	// 타겟(_targetId)은 TryDetect가 정해 둠
	case MonsterState::Return:
		_targetId = 0;	// 타겟을 잊음
		break;
	}

	Protocol::PosInfo* pos = GetPosInfo();
	cout << "[Monster " << GetObjectId() << "] " << ToString(prev) << " -> " << ToString(next)
		<< " at (" << pos->x() << ", " << pos->z() << ")"
		<< (next == MonsterState::Chase ? " target " + to_string(_targetId) : "") << endl;
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
