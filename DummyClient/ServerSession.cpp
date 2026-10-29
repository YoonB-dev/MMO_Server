#include "pch.h"
#include "ServerSession.h"
#include "ServerPacketHandler.h"

int64 NowUs()
{
	return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

void Log(const string& line)
{
	static Mutex s_logLock;

	LockGuard guard(s_logLock);
	cout << line << endl;	// endl이 flush까지 해서, 파일로 리다이렉트해도 바로 기록됨
}

void Log(int32 clientId, const string& message)
{
	Log("[Client " + to_string(clientId) + "] " + message);
}

static Atomic<int32> s_nextClientId = 1;

ServerSession::ServerSession()
	: _clientId(s_nextClientId.fetch_add(1))
{
}

void ServerSession::OnConnected()
{
	Log(_clientId, "Connected to server");

	// 접속하자마자 게임 입장 요청
	Protocol::C_ENTER_GAME enterPkt;
	Send(ServerPacketHandler::MakeSendBuffer(enterPkt));
}

void ServerSession::OnDisconnected()
{
	Log(_clientId, "Disconnected");
}

void ServerSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	PacketSessionRef session = GetPacketSessionRef();
	ServerPacketHandler::HandlePacket(session, buffer, len);
}

void ServerSession::OnSend(int32 len)
{
}
