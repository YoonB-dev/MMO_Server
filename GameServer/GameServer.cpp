#include "pch.h"
#include "ThreadManager.h"
#include "Service.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"
#include "DBConnectionPool.h"
#include "Room.h"
#include "MoveSendPolicy.h"

std::atomic<int64> g_inboundPacketCount(0);
std::atomic<int64> g_outboundPacketCount(0);

enum
{
	WORKER_TICK = 64,
	MAX_SESSION_COUNT = 1000,
	DB_CONNECTION_COUNT = 4,
};

// DB를 쓸 때 true로 변경 (config.ini의 [Database] 정보로 연결)
constexpr bool USE_DATABASE = false;

// 실행 파일과 같은 폴더의 config.ini 로드
bool LoadServerConfig()
{
	WCHAR exePath[MAX_PATH];
	::GetModuleFileNameW(NULL, exePath, MAX_PATH);
	std::wstring exeDir = exePath;
	exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/") + 1);

	if (GConfigManager->LoadConfig(exeDir + L"config.ini") == false)
	{
		wcout << L"Failed to load config.ini (copy GameServer/config.example.ini to GameServer/config.ini)" << endl;
		return false;
	}
	return true;
}

bool ConnectDatabase()
{
	if (GDBConnectionPool->Connect(DB_CONNECTION_COUNT, GConfigManager->GetDBConnectionString().c_str()) == false)
	{
		wcout << L"Failed to connect database (check [Database] in config.ini)" << endl;
		return false;
	}

	wcout << L"Database connected (" << DB_CONNECTION_COUNT << L" connections)" << endl;
	return true;
}

ServerServiceRef StartServerService()
{
	ServerServiceRef service = MakeShared<ServerService>(
		NetAddress(GConfigManager->GetServerIP(), GConfigManager->GetServerPort()),
		MakeShared<IocpCore>(),
		MakeShared<GameSession>,
		MAX_SESSION_COUNT
	);

	if (service->Start() == false)
	{
		wcout << L"Failed to start server on " << GConfigManager->GetServerIP() << L":" << GConfigManager->GetServerPort() << endl;
		return nullptr;
	}

	wcout << L"Game Server listening on " << GConfigManager->GetServerIP() << L":" << GConfigManager->GetServerPort() << endl;
	return service;
}

void DoWorkerJob(ServerServiceRef& service)
{
	while (true)
	{
		LEndTickCount = ::GetTickCount64() + WORKER_TICK;
		service->GetIocpCore()->Dispatch(10);

		// 예약된 일감 처리
		ThreadManager::DistributeReservedJobs();

		// 글로벌 큐
		ThreadManager::DoGlobalQueueWork();
	}
}

// GameServer.exe [--monster-send angle|every-tick]  (기본 angle, 프로파일링 비교용)
const MoveSendPolicy* ParseMonsterSendPolicy(int argc, char* argv[])
{
	string name = "angle";

	// argv[0]은 실행 파일 경로라서 1부터
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--monster-send")
			name = argv[i + 1];
	}

	const MoveSendPolicy* policy = FindMoveSendPolicy(name);
	if (policy == nullptr)
		cout << "Unknown --monster-send: " << name << " (angle | every-tick)" << endl;
	return policy;
}

int main(int argc, char* argv[])
{
	::SetConsoleOutputCP(CP_UTF8);

	const MoveSendPolicy* monsterSendPolicy = ParseMonsterSendPolicy(argc, argv);
	if (monsterSendPolicy == nullptr)
		return -1;

	if (LoadServerConfig() == false)
		return -1;

	if (USE_DATABASE && ConnectDatabase() == false)
		return -1;

	ClientPacketHandler::Init();
	GRoom = MakeShared<Room>();
	GRoom->SetMonsterSendPolicy(monsterSendPolicy);
	GRoom->Start();

	ServerServiceRef service = StartServerService();
	if (service == nullptr)
		return -1;

	int32 workerThreads = GConfigManager->GetWorkerThreadCount();
	for (int32 i = 0; i < workerThreads; i++)
	{
		GThreadManager->Launch([&service]()
			{
				DoWorkerJob(service);
			});
	}

	DoWorkerJob(service);

	GThreadManager->Join();
}
