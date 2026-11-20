#include "pch.h"
#include "ClientPacketHandler.h"
#include "GameSession.h"

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
