#include "pch.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "ClientPacketHandler.h"
#include "Player.h"
#include "Room.h"

void GameSession::OnConnected()
{
	GSessionManager.Add(static_pointer_cast<GameSession>(shared_from_this()));
}

void GameSession::OnDisconnected()
{
	GSessionManager.Remove(static_pointer_cast<GameSession>(shared_from_this()));

	// 입장한 상태였으면 방에서 내보냄 (실제 제거는 Room의 JobQueue에서)
	if (_player != nullptr)
	{
		GRoom->DoAsync(&Room::Leave, _player->GetObjectId());
		_player = nullptr;
	}
}

void GameSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	PacketSessionRef session = GetPacketSessionRef();

	// 최소한 헤더보다는 커야 함 (아니면 비정상 접근으로 보고 연결을 끊음)
	if (len < sizeof(PacketHeader) || session == nullptr)
	{
		Disconnect(L"Invalid Packet Length");
		return;
	}

	ClientPacketHandler::HandlePacket(session, buffer, len);
}

void GameSession::OnSend(int32 len)
{
}
