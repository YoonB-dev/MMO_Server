#include "pch.h"
#include "ThreadManager.h"
#include "Service.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"
#include "DBConnectionPool.h"
#include "DBBind.h"
#include "Room.h"
#include "MoveSendPolicy.h"
#include "GameData.h"

std::atomic<int64> g_inboundPacketCount(0);
std::atomic<int64> g_outboundPacketCount(0);

enum
{
	WORKER_TICK = 64,
	MAX_SESSION_COUNT = 1000,
	DB_CONNECTION_COUNT = 4,
};

// 기본 true: 시작할 때 DB 접속 + 확인 쿼리 (config.ini의 [Database]). DB 없이 네트워크만 볼 때만 false
constexpr bool USE_DATABASE = true;

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

// 접속만으로는 '쿼리가 실제로 도는지' 모름 -> 확인 쿼리 하나로 어느 DB, 어느 버전에 붙었는지 출력
bool VerifyDatabase()
{
	DBConnectionGuard db(GDBConnectionPool);	// 풀에서 하나 꺼냄 (함수가 끝나면 소멸자가 반납)
	if (!db)
	{
		wcout << L"[DB] no free connection for the startup check" << endl;
		return false;
	}

	WCHAR dbName[64] = {};
	WCHAR version[64] = {};

	DBBind<0, 2> query(db, L"SELECT DATABASE(), VERSION()");	// 파라미터 0개, 결과 컬럼 2개
	query.BindCol(0, dbName);
	query.BindCol(1, version);

	if (query.Execute() == false || query.Fetch() == false)
	{
		wcout << L"[DB] startup check query failed" << endl;	// 이유는 바로 위 [ODBC Error] 줄
		return false;
	}

	wcout << L"[DB] connected: " << dbName << L" (MySQL " << version << L"), " << DB_CONNECTION_COUNT << L" connections" << endl;
	return true;
}

bool ConnectDatabase()
{
	if (GDBConnectionPool->Connect(DB_CONNECTION_COUNT, GConfigManager->GetDBConnectionString().c_str()) == false)
	{
		// 실패 이유는 바로 위 [ODBC Error] 줄 (DB가 없으면 Unknown database, MySQL이 꺼져 있으면 Can't connect)
		wcout << L"Failed to connect database (check [Database] in config.ini, run Database/Setup-Database.ps1 if the database does not exist)" << endl;
		return false;
	}

	return VerifyDatabase();
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

// 시작 순서 (메인 스레드)
//   설정(config.ini) -> DB 접속 + 확인 쿼리 -> 기획 데이터 로드(LoadGameData)
//   -> Room 생성, 몬스터 스폰, 첫 틱 예약 -> 서버 열기(listen) -> 워커 스레드
//   DB와 기획 데이터를 워커보다 먼저 준비하므로, 이후 여러 스레드가 락 없이 읽기만 함
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

	// 기획 데이터(몬스터, 스폰, 스킬). Room::Start(몬스터 스폰)보다 먼저
	//   USE_DATABASE가 false면 건너뜀 -> 몬스터 없이 네트워크만 동작
	if (USE_DATABASE && LoadGameData() == false)
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
