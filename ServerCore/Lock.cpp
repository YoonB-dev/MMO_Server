#include "pch.h"
#include "Lock.h"
#include "CoreTLS.h"
#include "DeadLockProfiler.h"
#include "LockStats.h"

void Lock::WriteLock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif
#if USE_LOCK_STATS
	LockStats::OnLock(name, true);
#endif

	// 동일한 스레드가 소유하고 있다면 무조건 성공
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;
	if (LthreadId == lockThreadId)
	{
		_writeCount++;
		return;
	}

	// 아무도 소유 및 공유하고 있지 않을 때, 경합해서 소유권을 얻는다. (EMPTY_FLAG)
	// 소유: 누군가가 WriteLock을 획득한 상태
	// 공유: 누군가가 ReadLock을 획득한 상태

	const uint64 beginTick = ::GetTickCount64();
	
	// 상위 16비트에 쓰레드 아이디를 넣음
	const uint32 desired = ((LthreadId << 16) & WRITE_THREAD_MASK); 

	// 측정용: 한 번이라도 실패했을 때만 기다리기 시작한 시각을 잡고, 잡은 뒤 기다린 시간을 LLockWaitUs에 더함
	//   바로 잡히면 시각을 안 잼 -> 평소 비용은 bool 비교 하나. 락이 동작하는 방식(CAS, 스핀, yield, 타임아웃)은 그대로
	bool waited = false;
	chrono::steady_clock::time_point waitStart;
	
	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; spinCount++)
		{
			uint32 expected = EMPTY_FLAG;
			if (_lockFlag.compare_exchange_strong(OUT expected, desired))
			{
				_writeCount++;
				if (waited)
					LLockWaitUs += chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - waitStart).count();
				return;
			}
			if (waited == false)
			{
				waited = true;
				waitStart = chrono::steady_clock::now();
			}
		}

		if (::GetTickCount64() - beginTick > ACQUIRE_TIMEOUT_TICK)
		{
			// 타임아웃
			CRASH("LockTimeOut");
		}

		this_thread::yield();
	}
}

void Lock::WriteUnlock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif

	// ReadLock 다 풀기 전에는 WriteUnlock 불가
	if ((_lockFlag.load() & READ_COUNT_MASK) != 0)
	{
		CRASH("ReadLockExist_WriteUnlockFail");
	}

	const int32 lockCount = --_writeCount;
	if (lockCount == 0)
	{
		_lockFlag.store(EMPTY_FLAG);
	}
}

void Lock::ReadLock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif
#if USE_LOCK_STATS
	LockStats::OnLock(name, false);
#endif

	// 동일한 스레드가 쓰기 잠금을 소유하고 있다면 무조건 성공.
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;
	if (LthreadId == lockThreadId)
	{
		_lockFlag.fetch_add(1);
		return;
	}

	// 아무도 소유하고 있지 않을 때 경합해서 공유권을 얻는다.
	const uint64 beginTick = ::GetTickCount64();

	// 측정용: WriteLock과 같음 (실패했을 때만 시각을 재서 LLockWaitUs에 더함)
	bool waited = false;
	chrono::steady_clock::time_point waitStart;

	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; spinCount++)
		{
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(OUT expected, expected + 1))
			{
				if (waited)
					LLockWaitUs += chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - waitStart).count();
				return;
			}
			if (waited == false)
			{
				waited = true;
				waitStart = chrono::steady_clock::now();
			}

			if (::GetTickCount64() - beginTick > ACQUIRE_TIMEOUT_TICK)
			{
				// 타임아웃
				CRASH("LockTimeOut");
			}

			this_thread::yield();
		}	
	}
}

void Lock::ReadUnlock(const char* name)
{
#if _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif
	// 정보 .fetch_sub(1) 의 반환값은 언락되기 전의 값이므로, 0이 된다면 이전이 01이었다는 뜻
	if ((_lockFlag.fetch_sub(1) & READ_COUNT_MASK) == 0)
	{
		CRASH("ReadUnlock_Underflow");
	}
}
