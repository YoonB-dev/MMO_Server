#include "pch.h"
#include "ServerSession.h"
#include "ServerPacketHandler.h"

bool GInputTest = false;
bool GMoveTest = false;
bool GMonsterLog = false;
bool GChaseTest = false;
bool GTurnTest = false;
bool GFleeTest = false;
bool GAttackTest = false;
bool GRangedTest = false;
bool GDodgeTest = false;
bool GDeathTest = false;
std::vector<string> GAccessTokens;

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

	// 접속하자마자 게임 입장 요청 (main에서 로그인해 둔 내 번호의 토큰을 같이 보냄)
	Protocol::C_ENTER_GAME enterPkt;
	if (_clientId - 1 < static_cast<int32>(GAccessTokens.size()))
		enterPkt.set_access_token(GAccessTokens[_clientId - 1]);
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
