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
