#pragma once
#include "JobQueue.h"

// 게임 월드(전역 방 하나). 입장/퇴장/이동은 전부 DoAsync로 이 JobQueue에 넣어서 한 줄로 처리한다
class Room : public JobQueue
{
	enum : uint64
	{
		TICK_INTERVAL_MS = 50,		// 20Hz
		TICK_LOG_INTERVAL = 200,	// 200틱(10초)마다 틱 간격 측정 로그
		MAX_TICK_DELAY_MS = 1000,	// 이보다 밀리면 따라잡지 않고 기준 시각을 다시 잡음
	};

public:
	void	Start();				// 첫 틱 예약 (main에서 한 번)
	void	Enter(PlayerRef player);
	void	Leave(uint64 objectId);

private:
	void	Tick();
	void	Broadcast(SendBufferRef sendBuffer, uint64 exceptId = 0);
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음

	uint64	_tick = 0;				// 서버 틱 번호 (S_MOVE의 server_tick)
	uint64	_nextTickTime = 0;		// 다음 틱 목표 시각 (GetTickCount64 기준, ms)
	uint64	_logStartTime = 0;		// 틱 간격 측정 시작 시각
};

extern RoomRef GRoom;
