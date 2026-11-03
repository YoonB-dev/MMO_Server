#include "pch.h"
#include "ClientPacketHandler.h"
#include "GameSession.h"
#include "Player.h"
#include "Room.h"

// ClientPacketHandler.h는 빌드할 때마다 Protocol.proto에서 생성된다 (Common/protoc-21.12-win64/bin/GenPackets.bat)
//   -> C_로 시작하는 패킷을 proto에 추가하면 여기에 Handle_C_XXX를 만들어야 링크된다
PacketHandleFunc GPacketHandler[UINT16_MAX];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	cout << "Invalid packet received - ID: " << header->id << ", Size: " << header->size << endl;
	return false;
}

bool Handle_C_PING(PacketSessionRef& session, Protocol::C_PING& pkt)
{
	Protocol::S_PING pongPkt;
	pongPkt.set_client_time_us(pkt.client_time_us());
	session->Send(ClientPacketHandler::MakeSendBuffer(pongPkt));
	return true;
}

bool Handle_C_ENTER_GAME(PacketSessionRef& session, Protocol::C_ENTER_GAME& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	if (gameSession->GetPlayer() != nullptr)
		return false;	// 이미 입장한 세션

	PlayerRef player = MakeShared<Player>(gameSession);
	gameSession->SetPlayer(player);

	GRoom->DoAsync(&Room::Enter, player);	// 실제 입장은 Room의 줄에서
	return true;
}

bool Handle_C_MOVE_INPUT(PacketSessionRef& session, Protocol::C_MOVE_INPUT& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	PlayerRef player = gameSession->GetPlayer();
	if (player == nullptr)
		return false;	// 아직 입장 안 한 세션의 입력

	// 검증과 저장은 Room의 줄에서 (Player의 입력 상태는 Room 안에서만 만짐)
	GRoom->DoAsync(&Room::HandleMoveInput, player, pkt.dir_x(), pkt.dir_z(), pkt.buttons(), pkt.seq());
	return true;
}
