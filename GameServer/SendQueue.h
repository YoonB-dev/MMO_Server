#pragma once
#include "JobQueue.h"

// 송신 전용 줄. Room이 틱 끝에 "이번 틱에 패킷을 넣은 세션 목록"을 넘기면 워커 스레드가 세션마다 FlushSend(WSASend)
//   WSASend가 가끔 수 ms~22ms 멈춰도 Room 틱은 기다리지 않음 (Docs/LoadTest/README.md 송신 멈춤)
//   패킷 순서는 세션 안의 _sendQueue가 지킴: Room이 PushSend한 순서대로 쌓이고, FlushSend는 누가 부르든 큐를 앞에서부터 보냄
//     두 스레드가 동시에 불러도 _sendRegistered.exchange(true)로 한 스레드만 WSASend
//   줄은 SEND_QUEUE_COUNT(GameServer.cpp)개. 바깥에서는 SendRouter(아래)를 통해서만 부름
class SendQueue : public JobQueue
{
	static constexpr uint64	STATS_INTERVAL_MS = 10000;	// [Send] 통계 간격 (Room의 [Room] 줄, DB의 [DB] 줄과 같은 10초)

public:
	explicit SendQueue(int32 index) : _index(index) {}

	void	StartStats();													// 10초마다 [Send] 줄 (DBQueue::StartStats와 같은 방식)
	void	Flush(const xvector<GameSessionRef>& sessions, int64 handoffUs);	// 세션마다 FlushSend + 시간 기록 (이 줄 안에서 실행)

private:
	void	LogStats();

	int32	_index;		// 줄 번호 ([Send] 줄에 찍음)

	// 10초 통계 (이 줄 안에서만 읽고 씀 -> 락 / atomic 없음)
	uint64	_batches = 0;		// 받은 묶음 수 (틱마다 하나)
	uint64	_sessions = 0;		// 보낸 세션 수 합
	int64	_waitSumUs = 0;		// Room이 넘김 ~ 이 줄에서 시작 (워커가 집기까지 + 앞 묶음이 끝나기까지)
	int64	_waitMaxUs = 0;
	int64	_runSumUs = 0;		// 묶음 하나를 다 보내는 데 걸린 시간
	int64	_runMaxUs = 0;
	int64	_oneMaxUs = 0;		// 세션 하나가 가장 오래 걸린 시간
	uint64	_slowCount = 0;		// 1ms 넘게 걸린 세션 수
};

class SendRouter
{
public:
	void	Init(int32 queueCount);							// main에서 한 번 (워커가 뜨기 전): 줄 N개 생성 + 각 줄 StartStats
	void	RequestFlush(xvector<GameSessionRef> sessions);	// 앞에서부터 줄 수만큼 나눠서 줄마다 하나씩 (Room 줄에서 부름)

private:
	xvector<shared_ptr<SendQueue>>	_queues;
};

extern shared_ptr<SendRouter> GSendRouter;
