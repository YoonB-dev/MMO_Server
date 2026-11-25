#pragma once
#include <stack>

/*-----------------
	TLS(Thread Local Storage) - 스레드 별로 가지고 있는 영역
-------------------*/

//TLS
extern thread_local uint16				LthreadId;
extern thread_local uint64				LEndTickCount; // 틱이 완료되는 시간

extern thread_local std::stack<int32>	LLockStack;
extern thread_local SendBufferChunkRef	LSendBufferChunk;

// 이 스레드가 락을 바로 못 잡고 기다린 시간 누적 (마이크로초). 측정용: 컨텐츠(Room 틱)가 앞뒤 차이를 봄
//   Lock::WriteLock / ReadLock이 한 번이라도 실패했을 때만 더함 (바로 잡히면 시각을 안 재서 평소 비용 거의 0)
extern thread_local uint64				LLockWaitUs;

// 현재 스레드가 실행중인 Job Queue가 있는지 관리하기 위한 변수
extern thread_local class JobQueue*		LCurrentJobQueue;