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

	// 끊김 표시 + 플레이어를 꺼냄 (락 안에서) -> 이후 DB 큐의 SetPlayer는 실패해서 유령 플레이어가 생기지 않음
	PlayerRef player;
	{
		WriteLockGuard guard(_playerLock, typeid(this).name());
		_closed = true;
		player = _player;
		_player = nullptr;
	}

	// 입장한 상태였으면 방에서 내보냄 (실제 제거와 저장 요청은 Room의 JobQueue에서). DoAsync는 락 밖에서
	if (player != nullptr)
		GRoom->DoAsync(&Room::Leave, player);
}

PlayerRef GameSession::GetPlayer()
{
	WriteLockGuard guard(_playerLock, typeid(this).name());
	return _player;
}

bool GameSession::SetPlayer(PlayerRef player)
{
	WriteLockGuard guard(_playerLock, typeid(this).name());
	if (_closed)
		return false;
	_player = player;
	return true;
}

void GameSession::FailEnter(Protocol::EnterGameError error)
{
	LogLine() << "[Enter] rejected: error " << static_cast<int32>(error);

	Protocol::S_ENTER_GAME pkt;
	pkt.set_success(false);
	pkt.set_error(error);
	Send(ClientPacketHandler::MakeSendBuffer(pkt));

	_entering = false;	// 토큰을 갱신해서 다시 보낼 수 있게
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
