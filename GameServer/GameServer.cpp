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
#include "JwtVerifier.h"
#include "DBQueue.h"
#include "Player.h"	// Player::inventorySize (--inventory-size)

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

// 입장 토큰(JWT) 검증기 준비: config.ini [Auth]의 키가 아웃게임 서버와 같아야 함 (Setup-Secrets.ps1이 넣음)
bool InitAuth()
{
	const string secret = GConfigManager->GetString("Auth", "JwtSecret");
	const string issuer = GConfigManager->GetString("Auth", "Issuer");
	const string audience = GConfigManager->GetString("Auth", "Audience");

	if (GJwtVerifier.Init(secret, issuer, audience) == false)
	{
		LogLine() << "[Auth] config.ini [Auth] JwtSecret / Issuer / Audience is missing or invalid (run OutGameServer/Setup-Secrets.ps1)";
		return false;
	}

	LogLine() << "[Auth] JWT verifier ready (issuer " << issuer << ", audience " << audience << ")";
	return true;
}

// CPU 번호마다 성능 등급 (하이브리드 CPU: P코어가 높고 E코어가 낮음, 모두 같으면 하이브리드가 아님)
//   [Room] slowest tick의 "cpu a -> b"를 읽을 때 씀 (느린 틱이 E코어에서 돌았는지)
//   두 번 부르기: 처음엔 버퍼 없이 불러서 필요한 크기만 받고, 그만큼 잡아서 다시 부름 (Windows API에서 흔한 방식)
void LogCpuTopology()
{
	ULONG size = 0;
	::GetSystemCpuSetInformation(nullptr, 0, &size, ::GetCurrentProcess(), 0);
	if (size == 0)
		return;

	vector<BYTE> buffer(size);
	// reinterpret_cast: 바이트 배열을 그 안에 담긴 구조체 모양으로 보겠다는 형변환 (바이트는 그대로, 해석만 바뀜)
	auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
	if (::GetSystemCpuSetInformation(first, size, &size, ::GetCurrentProcess(), 0) == FALSE)
		return;

	// 항목 크기가 정해져 있지 않아서(Size 필드) 바이트 단위로 Size만큼씩 건너뛰며 읽음
	ostringstream line;
	for (ULONG offset = 0; offset < size;)
	{
		const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
		if (info->Type == CpuSetInformation)
			line << " " << static_cast<int32>(info->CpuSet.LogicalProcessorIndex) << ":" << static_cast<int32>(info->CpuSet.EfficiencyClass);
		offset += info->Size;
	}
	LogLine() << "[Server] cpu efficiency class (cpu:class, higher = faster core):" << line.str();
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
		LogLine() << "Unknown --monster-send: " << name << " (angle | every-tick)";
	return policy;
}

// GameServer.exe [--save-interval-sec N]  (주기 저장 간격, 기본 60초. 테스트에서 기다리지 않으려고 짧게 줌)
uint64 ParseSaveIntervalSec(int argc, char* argv[])
{
	uint64 seconds = 60;
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--save-interval-sec")
			seconds = static_cast<uint64>(::atoi(argv[i + 1]));
	}
	return seconds;
}

// GameServer.exe [--drop-lifetime-sec N]  (바닥 드랍 수명, 기본 60초. 테스트에서 만료를 기다리지 않으려고 짧게 줌)
uint64 ParseDropLifetimeSec(int argc, char* argv[])
{
	uint64 seconds = 60;
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--drop-lifetime-sec")
			seconds = static_cast<uint64>(::atoi(argv[i + 1]));
	}
	return seconds;
}

// GameServer.exe [--max-drops N]  (바닥 드랍 상한, 기본 500. 테스트에서 작게 줘서 오래된 것부터 지우는지 확인)
//   안 주면 0 -> Room이 기본값을 그대로 둠 (기본값은 Room 안에만 있게)
int32 ParseMaxDrops(int argc, char* argv[])
{
	int32 count = 0;
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--max-drops")
			count = ::atoi(argv[i + 1]);
	}
	return count;
}

// GameServer.exe [--max-visible-players N]  (한 명이 보는 다른 플레이어 상한, 기본 50. 테스트에서 작게)
//   안 주면 0 -> Room이 기본값을 그대로 둠
int32 ParseMaxVisiblePlayers(int argc, char* argv[])
{
	int32 count = 0;
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--max-visible-players")
			count = ::atoi(argv[i + 1]);
	}
	return count;
}

// GameServer.exe [--inventory-size N]  (인벤토리 칸 수, 기본 20. 테스트에서 작게 줘서 칸이 없으면 못 줍는지 확인)
//   워커 스레드가 뜨기 전에 한 번만 바꿈 -> 이후 여러 스레드가 읽기만 해서 안전
void ApplyInventorySize(int argc, char* argv[])
{
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--inventory-size")
		{
			const int32 size = ::atoi(argv[i + 1]);
			if (size > 0)
				Player::inventorySize = size;
		}
	}
}

// GameServer.exe [--load-monsters N] [--load-area M]  (부하 테스트용 몬스터 N마리를 ±M m에 추가. 기본 0마리, 100m)
void ApplyLoadMonsterOptions(int argc, char* argv[])
{
	int32 count = 0;
	float area = 100.f;
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--load-monsters")
			count = ::atoi(argv[i + 1]);
		else if (string(argv[i]) == "--load-area")
			area = static_cast<float>(::atof(argv[i + 1]));
	}
	GRoom->SetLoadMonsters(count, area);
}

// GameServer.exe [--load-boss <monster_id>]  (부하 테스트용 보스 한 마리를 (0, 0)에. 기본 0 = 없음)
//   몬스터 번호를 받음: 나중에 다른 보스 종류도 같은 옵션으로 잴 수 있게 (지금은 data_monster의 TestBoss = 2)
void ApplyLoadBossOption(int argc, char* argv[])
{
	for (int32 i = 1; i + 1 < argc; i++)
	{
		if (string(argv[i]) == "--load-boss")
			GRoom->SetLoadBoss(::atoi(argv[i + 1]));
	}
}

// 시작 순서 (메인 스레드)
//   설정(config.ini) -> 토큰 검증기([Auth]) -> DB 접속 + 확인 쿼리 -> 기획 데이터 로드(LoadGameData)
//   -> DB 줄 생성(연결 수만큼) -> Room 생성, 몬스터 스폰, 첫 틱 예약 -> 서버 열기(listen) -> 워커 스레드
//   DB와 기획 데이터를 워커보다 먼저 준비하므로, 이후 여러 스레드가 락 없이 읽기만 함
int main(int argc, char* argv[])
{
	::SetConsoleOutputCP(CP_UTF8);

	const MoveSendPolicy* monsterSendPolicy = ParseMonsterSendPolicy(argc, argv);
	if (monsterSendPolicy == nullptr)
		return -1;

	if (LoadServerConfig() == false)
		return -1;

	LogCpuTopology();	// 측정용: 어느 CPU 번호가 P / E코어인지 (틱이 튈 때 어느 코어였는지 보려고)

	if (InitAuth() == false)
		return -1;

	if (USE_DATABASE && ConnectDatabase() == false)
		return -1;

	// 기획 데이터(몬스터, 스폰, 스킬). Room::Start(몬스터 스폰)보다 먼저
	//   USE_DATABASE가 false면 건너뜀 -> 몬스터 없이 네트워크만 동작
	if (USE_DATABASE && LoadGameData() == false)
		return -1;

	ClientPacketHandler::Init();
	GDBRouter = MakeShared<DBRouter>();		// 캐릭터 불러오기 / 저장 (게임 중 DB 일은 전부 이 입구로)
	GDBRouter->Init(DB_CONNECTION_COUNT);	// DB 줄 수 = 연결 수 (줄마다 10초 [DB] 통계)
	GRoom = MakeShared<Room>();
	GRoom->SetMonsterSendPolicy(monsterSendPolicy);
	GRoom->SetSaveInterval(ParseSaveIntervalSec(argc, argv));
	GRoom->SetDropLifetime(ParseDropLifetimeSec(argc, argv));
	GRoom->SetMaxDrops(ParseMaxDrops(argc, argv));
	GRoom->SetMaxVisiblePlayers(ParseMaxVisiblePlayers(argc, argv));
	ApplyInventorySize(argc, argv);
	ApplyLoadMonsterOptions(argc, argv);
	ApplyLoadBossOption(argc, argv);
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
