#include "pch.h"
#include "SendQueue.h"
#include "GameSession.h"
#include <iterator>

// main에서 생성 (GDBRouter와 같은 이유: 전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
shared_ptr<SendRouter> GSendRouter;

// 흐름
//   Room 줄: 틱 끝 Room::FlushSends -> RequestFlush(목록) -> 줄마다 DoAsync
//     Room 줄 안에서 부르므로 DoAsync가 바로 실행되지 않고 GlobalQueue로 감 (JobQueue::Push: LCurrentJobQueue가 Room)
//   다른 워커: DoGlobalQueueWork -> SendQueue::Execute -> Flush -> 세션마다 FlushSend -> RegisterSend -> WSASend
//   IOCP 워커: 송신 완료 ProcessSend -> 큐에 남은 게 있으면 이어서 RegisterSend (그대로)

void SendRouter::Init(int32 queueCount)
{
	for (int32 i = 0; i < queueCount; i++)
	{
		shared_ptr<SendQueue> queue = MakeShared<SendQueue>(i);
		queue->StartStats();		// shared_from_this를 쓰므로 MakeShared 다음에
		_queues.push_back(queue);
	}
}

void SendRouter::RequestFlush(xvector<GameSessionRef> sessions)
{
	if (sessions.empty())
		return;

	const int64 handoffUs = NowUs();
	const size_t count = _queues.size();
	const size_t chunk = (sessions.size() + count - 1) / count;	// 올림 나눗셈: 400명 / 2줄 = 200명씩, 3명 / 2줄 = 2명씩
	for (size_t q = 0; q < count; q++)
	{
		const size_t begin = q * chunk;
		if (begin >= sessions.size())
			break;
		const size_t end = (std::min)(begin + chunk, sessions.size());

		// make_move_iterator: 이 구간의 shared_ptr를 복사하지 않고 옮겨서 새 목록으로 (참조 수를 올렸다 내리지 않음)
		xvector<GameSessionRef> part(std::make_move_iterator(sessions.begin() + begin), std::make_move_iterator(sessions.begin() + end));

		// 이동 캡처 [part = std::move(part)]: part를 람다 안으로 옮김 -> 바깥 part는 빈 목록
		//   람다(와 그 안의 목록)는 Job에 담겨 송신 줄에서 실행됨. 목록의 마지막 참조도 거기서 놓임 (끊긴 세션의 소멸도 Room 밖)
		shared_ptr<SendQueue> queue = _queues[q];
		queue->DoAsync([queue, part = std::move(part), handoffUs]()
			{
				queue->Flush(part, handoffUs);
			});
	}
}

void SendQueue::StartStats()
{
	DoTimer(STATS_INTERVAL_MS, &SendQueue::LogStats);
}

void SendQueue::Flush(const xvector<GameSessionRef>& sessions, int64 handoffUs)
{
	const int64 startUs = NowUs();
	for (const GameSessionRef& session : sessions)
	{
		const int64 oneStartUs = NowUs();
		session->FlushSend();		// 끊긴 세션이면 바로 끝남. 큐가 비었거나 이미 보내는 중이어도 아무것도 안 함
		const int64 us = NowUs() - oneStartUs;
		_oneMaxUs = (std::max)(_oneMaxUs, us);
		if (us >= 1'000)
			_slowCount++;
	}
	const int64 doneUs = NowUs();

	const int64 waitUs = startUs - handoffUs;
	const int64 runUs = doneUs - startUs;
	_batches++;
	_sessions += sessions.size();
	_waitSumUs += waitUs;
	_waitMaxUs = (std::max)(_waitMaxUs, waitUs);
	_runSumUs += runUs;
	_runMaxUs = (std::max)(_runMaxUs, runUs);
}

// 10초마다 (DoTimer로 이 줄에서 실행 -> Flush와 같은 줄이라 통계를 같이 만져도 락 없음)
void SendQueue::LogStats()
{
	if (_batches > 0)
	{
		const double batches = static_cast<double>(_batches);
		LogLine() << "[Send] queue " << _index << ": batches " << _batches << ", sessions " << _sessions
			<< ", wait avg " << (_waitSumUs / 1000.0 / batches) << " max " << (_waitMaxUs / 1000.0) << "ms"
			<< ", run avg " << (_runSumUs / 1000.0 / batches) << " max " << (_runMaxUs / 1000.0) << "ms"
			<< ", one session max " << (_oneMaxUs / 1000.0) << "ms, over 1ms " << _slowCount;
	}

	_batches = 0;
	_sessions = 0;
	_waitSumUs = 0;
	_waitMaxUs = 0;
	_runSumUs = 0;
	_runMaxUs = 0;
	_oneMaxUs = 0;
	_slowCount = 0;

	DoTimer(STATS_INTERVAL_MS, &SendQueue::LogStats);
}
