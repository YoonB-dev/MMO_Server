#include "pch.h"
#include "Service.h"
#include "ThreadManager.h"
#include "ServerSession.h"
#include "ServerPacketHandler.h"

// 접속 확인용 더미 클라이언트: CLIENT_COUNT명이 접속해서 게임에 입장하고, 1초마다 C_PING -> S_PING 왕복 시간 출력
static const int32 CLIENT_COUNT = 3;
static const int32 IOCP_THREAD_COUNT = 2;

int main()
{
	::SetConsoleOutputCP(CP_UTF8);

	this_thread::sleep_for(1s); // 같이 실행할 때 서버가 먼저 열리도록 잠깐 대기
	ServerPacketHandler::Init();

	ClientServiceRef service = MakeShared<ClientService>(
		NetAddress(L"127.0.0.1", 7777),
		MakeShared<IocpCore>(),
		MakeShared<ServerSession>,
		CLIENT_COUNT);

	ASSERT_CRASH(service->Start());

	for (int32 i = 0; i < IOCP_THREAD_COUNT; i++)
	{
		GThreadManager->Launch([service]()
			{
				while (true)
				{
					service->GetIocpCore()->Dispatch();
				}
			});
	}

	while (true)
	{
		this_thread::sleep_for(1s);

		Protocol::C_PING pingPkt;
		pingPkt.set_client_time_us(NowUs());
		service->Brodcast(ServerPacketHandler::MakeSendBuffer(pingPkt));
	}

	GThreadManager->Join();
}
