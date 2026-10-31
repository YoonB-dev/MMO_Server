#include "pch.h"
#include "Service.h"
#include "ThreadManager.h"
#include "ServerSession.h"
#include "ServerPacketHandler.h"

// 접속 확인용 더미 클라이언트: N명이 접속해서 게임에 입장하고, 1초마다 C_PING -> S_PING 왕복 시간 출력
//   DummyClient.exe [--count N] [--ip 127.0.0.1] [--port 7777] [--no-ping] [--input-test] [--move-test]
//   인자 없이 실행하면 3명, 127.0.0.1:7777, ping 사용 (Test/Run-Test.ps1에서 옵션을 넘겨서 사용)
static const int32 IOCP_THREAD_COUNT = 2;

struct ClientOptions
{
	wstring	ip = L"127.0.0.1";
	uint16	port = 7777;
	int32	count = 3;
	bool	ping = true;
	bool	inputTest = false;	// 입장 후 검증용 이동 입력 묶음 전송
	bool	moveTest = false;	// 입장 후 오른쪽 이동 -> 멈춤 -> 위치 판정
};

ClientOptions ParseOptions(int argc, char* argv[])
{
	ClientOptions options;

	// argv[0]은 실행 파일 경로라서 1부터
	for (int32 i = 1; i < argc; i++)
	{
		const string arg = argv[i];
		const bool hasValue = (i + 1 < argc);

		if (arg == "--count" && hasValue)
			options.count = ::atoi(argv[++i]);
		else if (arg == "--port" && hasValue)
			options.port = static_cast<uint16>(::atoi(argv[++i]));
		else if (arg == "--ip" && hasValue)
		{
			const string ip = argv[++i];
			options.ip.assign(ip.begin(), ip.end());	// IP는 ASCII라 한 글자씩 wchar로 옮겨도 됨
		}
		else if (arg == "--no-ping")
			options.ping = false;
		else if (arg == "--input-test")
			options.inputTest = true;
		else if (arg == "--move-test")
			options.moveTest = true;
		else
			Log("[Client] Unknown option: " + arg);
	}

	return options;
}

int main(int argc, char* argv[])
{
	::SetConsoleOutputCP(CP_UTF8);

	const ClientOptions options = ParseOptions(argc, argv);
	GInputTest = options.inputTest;
	GMoveTest = options.moveTest;

	this_thread::sleep_for(1s); // 같이 실행할 때 서버가 먼저 열리도록 잠깐 대기
	ServerPacketHandler::Init();

	ClientServiceRef service = MakeShared<ClientService>(
		NetAddress(options.ip, options.port),
		MakeShared<IocpCore>(),
		MakeShared<ServerSession>,
		options.count);

	if (service->Start() == false)
	{
		Log("[Client] Failed to start client service");
		return -1;
	}

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

		if (options.ping == false)
			continue;

		Protocol::C_PING pingPkt;
		pingPkt.set_client_time_us(NowUs());
		service->Brodcast(ServerPacketHandler::MakeSendBuffer(pingPkt));
	}

	GThreadManager->Join();
}
