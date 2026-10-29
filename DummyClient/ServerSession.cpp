#include "pch.h"
#include "ServerSession.h"
#include "ServerPacketHandler.h"

int64 NowUs()
{
	return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

void ServerSession::OnConnected()
{
	cout << "[Client] Connected to server" << endl;

	// 접속하자마자 게임 입장 요청
	Protocol::C_ENTER_GAME enterPkt;
	Send(ServerPacketHandler::MakeSendBuffer(enterPkt));
}

void ServerSession::OnDisconnected()
{
	cout << "[Client] Disconnected" << endl;
}

void ServerSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	PacketSessionRef session = GetPacketSessionRef();
	ServerPacketHandler::HandlePacket(session, buffer, len);
}

void ServerSession::OnSend(int32 len)
{
}
