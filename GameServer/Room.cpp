#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"

// main에서 생성 (전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
RoomRef GRoom;

void Room::Start()
{
	const uint64 now = ::GetTickCount64();
	_nextTickTime = now + TICK_INTERVAL_MS;
	_logStartTime = now;
	DoTimer(TICK_INTERVAL_MS, &Room::Tick);
}

void Room::Tick()
{
	_tick++;

	// (3~4단계) 여기서 입력 적용 -> 이동 계산 -> S_MOVE 전송

	const uint64 now = ::GetTickCount64();

	if (_tick % TICK_LOG_INTERVAL == 0)
	{
		const double avgMs = static_cast<double>(now - _logStartTime) / TICK_LOG_INTERVAL;
		cout << "[Room] tick " << _tick << ", avg interval " << avgMs << "ms" << endl;
		_logStartTime = now;
	}

	// 목표 시각을 누적해서 다음 틱 예약 -> 한 번 늦게 실행돼도 다음 간격이 밀리지 않음
	_nextTickTime += TICK_INTERVAL_MS;
	if (now > _nextTickTime + MAX_TICK_DELAY_MS)
		_nextTickTime = now;	// 너무 밀렸으면(서버 멈춤 등) 몰아서 따라잡지 않고 기준을 새로 잡음

	const uint64 delay = (_nextTickTime > now) ? (_nextTickTime - now) : 0;
	DoTimer(delay, &Room::Tick);
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

	// 2. 나에게: 이미 방에 있던 사람들
	{
		Protocol::S_SPAWN pkt;
		for (auto& [id, other] : _players)
		{
			if (id != myId)
				*pkt.add_objects() = other->GetObjectInfo();
		}

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
