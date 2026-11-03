#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"
#include "MoveSendPolicy.h"
#include <cmath>
#include <algorithm>
#include <chrono>

// main에서 생성 (전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
RoomRef GRoom;

// 몬스터 스폰 위치. 지금은 3마리 고정, 나중에 데이터 파일로 뺀다
//   플레이어 입장 위치 (0, 0)에서 15m 넘게 떨어뜨림 -> 들어오자마자 쫓기지 않게
struct SpawnPoint
{
	float x;
	float z;
};

static constexpr SpawnPoint MONSTER_SPAWNS[] =
{
	{ 15.f, 15.f },
	{ -20.f, 10.f },
	{ 0.f, -25.f },
};

void Room::SetMonsterSendPolicy(const MoveSendPolicy* policy)
{
	_monsterSendPolicy = policy;
	cout << "[Room] monster send policy: " << policy->Name() << endl;
}

void Room::Start()
{
	SpawnMonsters();	// 서버가 열리기 전(main 스레드만 실행 중)이라 바로 호출해도 안전

	const uint64 now = ::GetTickCount64();
	_nextTickTime = now + TICK_INTERVAL_MS;
	_logStartTime = now;
	DoTimer(TICK_INTERVAL_MS, &Room::Tick);
}

void Room::SpawnMonsters()
{
	for (const SpawnPoint& spawn : MONSTER_SPAWNS)
	{
		MonsterRef monster = MakeShared<Monster>(spawn.x, spawn.z);
		_monsters[monster->GetObjectId()] = monster;

		cout << "[Room] spawn monster " << monster->GetObjectId()
			<< " at (" << spawn.x << ", " << spawn.z << ")" << endl;
	}
}

void Room::Tick()
{
	_tick++;

	// 게임 처리에 걸린 시간 측정 (GetTickCount64는 약 15ms 단위라 짧은 시간은 steady_clock으로)
	const auto workStart = chrono::steady_clock::now();

	if (_tick % TICKS_PER_SECOND == 0)
		ResetInputCounts();

	// 이번 틱에 바뀐 이동을 한 패킷에 모아서 전체(본인 포함)에 전송
	//   플레이어 먼저 -> 몬스터는 이번 틱에 움직인 플레이어 위치를 보고 판단
	Protocol::S_MOVE pkt;
	pkt.set_server_tick(_tick);
	UpdatePlayers(pkt);
	UpdateMonsters(pkt);

	if (pkt.moves_size() > 0)
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));

	const uint64 workUs = chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - workStart).count();
	_workTimeSumUs += workUs;
	if (workUs > _workTimeMaxUs)
		_workTimeMaxUs = workUs;

	const uint64 now = ::GetTickCount64();

	if (_tick % TICK_LOG_INTERVAL == 0)
		LogStats(now);

	// 목표 시각을 누적해서 다음 틱 예약 -> 한 번 늦게 실행돼도 다음 간격이 밀리지 않음
	_nextTickTime += TICK_INTERVAL_MS;
	if (now > _nextTickTime + MAX_TICK_DELAY_MS)
		_nextTickTime = now;	// 너무 밀렸으면(서버 멈춤 등) 몰아서 따라잡지 않고 기준을 새로 잡음

	const uint64 delay = (_nextTickTime > now) ? (_nextTickTime - now) : 0;
	DoTimer(delay, &Room::Tick);
}

// 200틱(10초) 동안의 통계 출력 후 다음 구간을 위해 0으로
//   avg interval: 틱 사이 간격 (50ms면 정상), work: 틱 안에서 게임 처리에 쓴 시간, monster moves: 보낸 몬스터 이동 수
void Room::LogStats(uint64 now)
{
	const double avgMs = static_cast<double>(now - _logStartTime) / TICK_LOG_INTERVAL;
	const double workAvgMs = _workTimeSumUs / 1000.0 / TICK_LOG_INTERVAL;
	const double workMaxMs = _workTimeMaxUs / 1000.0;

	cout << "[Room] tick " << _tick << ", avg interval " << avgMs << "ms"
		<< ", work avg " << workAvgMs << "ms max " << workMaxMs << "ms"
		<< ", monster moves " << _monsterMovesSent << " (" << _monsterSendPolicy->Name() << ")" << endl;

	_logStartTime = now;
	_workTimeSumUs = 0;
	_workTimeMaxUs = 0;
	_monsterMovesSent = 0;
}

void Room::Enter(PlayerRef player)
{
	const uint64 myId = player->GetObjectId();
	_players[myId] = player;

	// 1. 나에게: 내 캐릭터 정보
	{
		Protocol::S_ENTER_GAME pkt;
		pkt.set_success(true);
		*pkt.mutable_player() = player->GetObjectInfo();
		SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
	}

	// 2. 나에게: 이미 방에 있던 객체들 (다른 플레이어 + 몬스터)
	{
		Protocol::S_SPAWN pkt;
		for (auto& [id, other] : _players)
		{
			if (id != myId)
				*pkt.add_objects() = other->GetObjectInfo();
		}

		for (auto& [id, monster] : _monsters)
			*pkt.add_objects() = monster->GetObjectInfo();

		if (pkt.objects_size() > 0)
			SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
	}

	// 3. 다른 사람들에게: 내가 들어왔다
	{
		Protocol::S_SPAWN pkt;
		*pkt.add_objects() = player->GetObjectInfo();
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt), myId);
	}
}

void Room::Leave(uint64 objectId)
{
	if (_players.erase(objectId) == 0)
		return;	// 이미 나간 플레이어

	Protocol::S_DESPAWN pkt;
	pkt.add_object_ids(objectId);
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt), objectId);
}

void Room::HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq)
{
	// 입력이 줄에서 기다리는 사이 이미 Leave가 처리된 플레이어
	if (_players.find(player->GetObjectId()) == _players.end())
		return;

	// 개수 제한: 검사보다 먼저 센다 (이상한 입력을 무한히 보내는 것도 막기 위해)
	if (++player->inputCount > MAX_INPUTS_PER_SECOND)
		return RejectInput(player, seq, "rate");

	// 순서: 이미 받은 번호 이하면 옛날 입력이거나 중복
	if (seq <= player->lastInput.seq)
		return RejectInput(player, seq, "old seq");

	// 값: NaN이나 무한대면 이후 위치 계산이 전부 망가짐
	if (std::isfinite(dirX) == false || std::isfinite(dirZ) == false)
		return RejectInput(player, seq, "invalid dir");

	// 길이 정리: 1보다 길면 방향은 유지하고 길이만 1로 (속도 핵 방지), 거의 0이면 멈춤
	const float length = std::sqrt(dirX * dirX + dirZ * dirZ);
	if (length < MIN_DIR_LENGTH)
	{
		dirX = 0.f;
		dirZ = 0.f;
	}
	else if (length > 1.f)
	{
		dirX /= length;
		dirZ /= length;
	}

	// 같은 틱 안에 또 오면 덮어씀 -> 틱은 마지막 입력 하나만 적용
	player->lastInput = MoveInput{ dirX, dirZ, buttons, seq };
	player->hasNewInput = true;
}

PlayerRef Room::FindPlayer(uint64 objectId)
{
	auto it = _players.find(objectId);
	return (it != _players.end()) ? it->second : nullptr;
}

// (x, z)에서 range 안에 있는 가장 가까운 플레이어. 없으면 nullptr
PlayerRef Room::FindNearestPlayer(float x, float z, float range)
{
	PlayerRef nearest = nullptr;
	float nearestDistSq = range * range;	// 이보다 가까워야 후보 -> 범위 밖은 자연히 빠짐

	for (auto& [id, player] : _players)
	{
		const Protocol::PosInfo* pos = player->GetPosInfo();
		const float dx = pos->x() - x;
		const float dz = pos->z() - z;
		const float distSq = dx * dx + dz * dz;
		if (distSq <= nearestDistSq)
		{
			nearest = player;
			nearestDistSq = distSq;
		}
	}
	return nearest;
}

void Room::ResetInputCounts()
{
	for (auto& [id, player] : _players)
		player->inputCount = 0;
}

void Room::UpdatePlayers(Protocol::S_MOVE& pkt)
{
	const bool resendTick = (_tick % MOVE_RESEND_TICKS == 0);

	for (auto& [id, player] : _players)
	{
		const bool changed = ApplyInput(player);
		MovePlayer(player);

		// 방향이 바뀌었으면 바로, 움직이는 중이면 4틱마다 보냄 (멈춰 있으면 안 보냄)
		const bool moving = (player->GetMoveInfo()->state() == Protocol::MOVE_STATE_MOVING);
		if (changed || (moving && resendTick))
		{
			Protocol::MoveEntry* entry = pkt.add_moves();
			entry->set_object_id(id);
			*entry->mutable_move_info() = *player->GetMoveInfo();
			entry->set_ack_seq(player->lastInput.seq);	// 받아들인 입력은 위 ApplyInput에서 전부 적용됐으므로 = 적용한 마지막 번호
		}
	}
}

void Room::UpdateMonsters(Protocol::S_MOVE& pkt)
{
	const bool resendTick = (_tick % MOVE_RESEND_TICKS == 0);

	for (auto& [id, monster] : _monsters)
	{
		monster->Update(*this, TICK_DT);

		// 보낼지는 전송 정책이 결정 (마지막으로 보낸 이동 vs 지금 이동)
		const Protocol::MoveInfo& now = *monster->GetMoveInfo();
		if (_monsterSendPolicy->ShouldSend(monster->lastSentMove, now, resendTick) == false)
			continue;

		Protocol::MoveEntry* entry = pkt.add_moves();
		entry->set_object_id(id);
		*entry->mutable_move_info() = now;
		// ack_seq는 0 그대로 (몬스터는 클라 입력이 없음)

		monster->lastSentMove = now;
		_monsterMovesSent++;
	}
}

// 우편함에 새 입력이 있으면 이동 방향/상태에 반영. 반영했으면 true
bool Room::ApplyInput(const PlayerRef& player)
{
	if (player->hasNewInput == false)
		return false;
	player->hasNewInput = false;	// 꺼냈으니 다음 틱엔 "새 입력 없음"

	const MoveInput& input = player->lastInput;
	Protocol::MoveInfo* moveInfo = player->GetMoveInfo();
	moveInfo->set_dir_x(input.dirX);
	moveInfo->set_dir_z(input.dirZ);

	// 거의 0인 방향은 HandleMoveInput에서 정확히 0으로 바꿔 두었으므로 == 비교로 충분
	const bool stop = (input.dirX == 0.f && input.dirZ == 0.f);
	moveInfo->set_state(stop ? Protocol::MOVE_STATE_IDLE : Protocol::MOVE_STATE_MOVING);
	moveInfo->set_speed(stop ? 0.f : MOVE_SPEED);

	// 바라보는 방향 (Unity 기준 +z = 0도, +x = 90도). 멈출 때는 마지막으로 보던 방향 유지
	if (stop == false)
		moveInfo->mutable_pos_info()->set_yaw(std::atan2(input.dirX, input.dirZ) * RAD_TO_DEG);

	return true;
}

void Room::MovePlayer(const PlayerRef& player)
{
	Protocol::MoveInfo* moveInfo = player->GetMoveInfo();
	if (moveInfo->state() != Protocol::MOVE_STATE_MOVING)
		return;

	const float step = MOVE_SPEED * TICK_DT;	// 한 틱에 0.25m
	Protocol::PosInfo* pos = moveInfo->mutable_pos_info();
	pos->set_x(std::clamp(pos->x() + moveInfo->dir_x() * step, -WORLD_HALF_SIZE, WORLD_HALF_SIZE));
	pos->set_z(std::clamp(pos->z() + moveInfo->dir_z() * step, -WORLD_HALF_SIZE, WORLD_HALF_SIZE));
	// y는 아직 항상 0 (점프 단계에서 vel_y로 처리)
}

void Room::RejectInput(PlayerRef player, uint32 seq, const char* reason)
{
	cout << "[Room] reject input: player " << player->GetObjectId()
		<< ", seq " << seq << ", reason " << reason << endl;
}

void Room::Broadcast(SendBufferRef sendBuffer, uint64 exceptId)
{
	for (auto& [id, player] : _players)
	{
		if (id != exceptId)
			SendTo(player, sendBuffer);
	}
}

void Room::SendTo(PlayerRef player, SendBufferRef sendBuffer)
{
	if (GameSessionRef session = player->GetSession())
		session->Send(sendBuffer);
}
