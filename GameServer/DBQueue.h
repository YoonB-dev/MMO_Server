#pragma once
#include "JobQueue.h"

class DBConnection;
struct CharacterData;
struct ItemSlot;

// DB 전용 줄 하나. 줄은 DB 연결 수만큼 있고, 바깥에서는 DBRouter(아래)를 통해서만 부름
//   Room 줄은 DB를 기다리지 않음 (DB가 느려도 틱이 안 멈춤)
//   계정 번호로 줄이 정해짐 -> 같은 계정은 항상 같은 줄에서 한 번에 하나씩, 들어온 순서대로 -> 불러오기 / 저장 순서가 섞이지 않음
//   다른 계정은 다른 줄에서 동시에 돌 수 있음 (연결 4개를 같이 씀)
//   DB 일은 그 일을 실행하는 워커 스레드를 잠깐 막음 -> 줄 4개가 다 바쁘면 워커 4개가 막힘 (나머지가 Room 줄을 돌림)
//
// 접속 중인 계정 목록(_online)도 줄마다 자기 계정 몫만 들고 있음 -> 같은 계정은 "마지막 저장 -> 다음 불러오기" 순서가 항상 지켜짐
//   중복 로그인: 옛 세션을 끊고 새 세션은 _waiting에 둠 -> 옛 세션의 LeaveCharacter(저장)가 끝나면 이어서 불러옴
class DBQueue : public JobQueue
{
	static constexpr uint64	STATS_INTERVAL_MS = 10000;	// [DB] 통계 간격 (Room의 [Room] 줄과 같은 10초)
	static constexpr size_t	SAVE_BATCH_SIZE = 50;		// 주기 저장: 트랜잭션 하나에 몇 명 (COMMIT 수 = 인원 / 50)
														//   COMMIT마다 디스크 기록을 기다려서(약 5ms) 한 명씩 COMMIT하면 400명에 2초 (Docs/LoadTest/README.md DB 기준선)

	// 일감 종류 하나의 10초 통계 (이 줄 안에서만 읽고 씀 -> 락 / atomic 없음)
	struct JobStats
	{
		uint64	count = 0;
		uint64	waitSumUs = 0;	// 요청 ~ 이 줄에서 실행 시작 (줄에서 기다린 시간)
		uint64	waitMaxUs = 0;
		uint64	runSumUs = 0;	// 실행 시작 ~ 끝 (DB 쿼리 포함)
		uint64	runMaxUs = 0;
		void	Add(int64 waitUs, int64 runUs);
	};
	struct ScopedJob;	// 일감 하나의 대기 / 실행 시간을 재는 RAII (DBQueue.cpp에 정의)

public:
	// explicit: 숫자 하나가 몰래 DBQueue로 바뀌는 일을 막음 (DBQueue q = 3; 같은 코드가 컴파일되지 않게)
	explicit DBQueue(int32 index) : _index(index) {}

	void	StartStats();	// DBRouter::Init에서 한 번: 10초마다 LogStats 예약

	// 요청 입구 (DBRouter가 고른 줄에서 부름): 지금 시각을 붙여서 이 줄에 넣음 -> 줄에서 기다린 시간을 잴 수 있음
	//   실제 일 함수는 private. 바깥(핸들러, Room)은 DBRouter만 부름
	void	RequestLoad(GameSessionRef session, int64 accountId, int64 characterId);
	void	RequestLeave(CharacterData data);
	void	RequestSave(xvector<CharacterData> rows);

private:
	// 아래 세 개는 이 줄 안에서만 실행됨. requestUs: 요청한 시각 (NowUs, 대기 시간 계산용)
	// 입장: 고른 캐릭터를 불러옴 (그 계정의 캐릭터가 아니거나 없으면 INVALID_CHARACTER) -> Player 생성 -> Room에 입장 요청
	//   캐릭터는 아웃게임 서버(POST /characters)가 만듦. 게임 서버는 불러오고 저장만 함
	//   이미 접속 중인 계정이면(다른 캐릭터여도) 옛 세션을 끊고, 그 세션의 저장이 끝날 때까지 기다림
	void	LoadCharacter(GameSessionRef session, int64 accountId, int64 characterId, int64 requestUs);
	// 나감 (Room::Leave가 보냄): 저장 + 접속 목록에서 뺌 + 이 계정으로 기다리던 로그인이 있으면 이어서 불러옴
	void	LeaveCharacter(CharacterData data, int64 requestUs);
	// 주기 저장 (Room::SaveAllPlayers가 보냄): 연결 하나로 SAVE_BATCH_SIZE명씩 트랜잭션 하나에 묶어서 저장
	void	SaveCharacters(xvector<CharacterData> rows, int64 requestUs);
	// 10초 통계 한 줄 + 0으로 + 다음 예약 (Room::Tick처럼 DoTimer로 자기 자신을 다시 예약)
	void	LogStats();

private:
	static bool	SelectCharacter(DBConnection& db, int64 accountId, int64 characterId, CharacterData& out, bool& found);
	static bool	SelectItems(DBConnection& db, int64 characterId, vector<ItemSlot>& out);
	static bool	SaveCharacter(DBConnection& db, const CharacterData& data);		// 한 명을 트랜잭션 하나로 (나감 저장, 묶음이 실패했을 때)
	static bool	SaveBatch(DBConnection& db, const xvector<CharacterData>& rows, size_t begin, size_t end);	// rows[begin, end)를 트랜잭션 하나로 (주기 저장)
	static bool	WriteCharacter(DBConnection& db, const CharacterData& data);	// 트랜잭션 없이 쓰기만: UpdateCharacter + UpsertItem + DeleteEmptySlots
	static bool	EndTransaction(DBConnection& db, bool ok);						// ok면 COMMIT, 아니면(또는 COMMIT 실패) ROLLBACK. COMMIT까지 성공해야 true
	static bool	ExecuteRaw(DBConnection& db, const WCHAR* sql);					// START TRANSACTION / COMMIT / ROLLBACK
	static bool	DeleteEmptySlots(DBConnection& db, const CharacterData& data);
	static bool	UpdateCharacter(DBConnection& db, const CharacterData& data);
	static bool	UpsertItem(DBConnection& db, int64 characterId, const ItemSlot& item);

private:
	// 옛 세션의 저장을 기다리는 새 로그인: 세션 + 어느 캐릭터로 들어오려 했는지 (옛 세션과 다른 캐릭터일 수 있음)
	struct WaitingLogin
	{
		weak_ptr<GameSession>	session;
		int64					characterId = 0;
		int64					requestUs = 0;	// 처음 요청한 시각 -> 이어서 불러올 때 대기 시간에 옛 세션 저장을 기다린 시간까지 들어감
	};

	// DB 줄 안에서만 접근 -> 락 없음. 세션은 weak_ptr (목록 때문에 세션이 안 사라지는 일이 없게)
	//   둘 다 계정 기준: 같은 계정은 캐릭터가 달라도 한 번에 한 세션만
	xunordered_map<int64, weak_ptr<GameSession>>	_online;	// 입장한 계정 -> 그 세션 (SetPlayer 성공 ~ LeaveCharacter)
	xunordered_map<int64, WaitingLogin>				_waiting;	// 계정 -> 옛 세션의 저장을 기다리는 새 로그인

	int32		_index = 0;		// 몇 번 줄인지 ([DB] 통계 줄 끝의 "queue N")

	// 10초 통계 (LogStats가 찍고 0으로)
	JobStats	_loadStats;
	JobStats	_leaveStats;
	JobStats	_saveStats;
	uint64		_savedRows = 0;	// 주기 저장으로 쓴 캐릭터 수
};

// DB 일의 입구: 계정 번호로 줄을 골라서 넘기기만 함 (같은 계정 = 항상 같은 줄 -> 그 계정의 순서는 그대로)
//   줄 수는 DB 연결 수와 같게: 한 줄은 한 번에 연결 하나만 빌리므로, 줄이 더 많으면 Pop이 빈 연결을 기다리며 워커를 막음
//   JobQueue가 아님: Init 뒤로는 _queues를 읽기만 함 -> 여러 스레드가 동시에 불러도 락이 필요 없음 (기획 데이터와 같은 이유)
class DBRouter
{
public:
	void	Init(int32 queueCount);		// main에서 한 번 (워커가 뜨기 전): 줄 N개 생성 + 각 줄 StartStats
	void	RequestLoad(GameSessionRef session, int64 accountId, int64 characterId);
	void	RequestLeave(CharacterData data);
	void	RequestSave(xvector<CharacterData> rows);	// 계정별로 나눠서 줄마다 하나씩 (줄 안에서는 다시 50명씩 묶음)

private:
	size_t	QueueIndex(int64 accountId) const;

	xvector<shared_ptr<DBQueue>>	_queues;
};

extern shared_ptr<DBRouter> GDBRouter;
