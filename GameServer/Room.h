#pragma once
#include "JobQueue.h"

// 게임 월드(전역 방 하나). 입장/퇴장/이동은 전부 DoAsync로 이 JobQueue에 넣어서 한 줄로 처리한다
class Room : public JobQueue
{
	enum : uint64
	{
		TICK_INTERVAL_MS = 50,		// 20Hz
		TICKS_PER_SECOND = 1000 / TICK_INTERVAL_MS,	// 20
		TICK_LOG_INTERVAL = 200,	// 200틱(10초)마다 틱 간격 측정 로그
		MAX_TICK_DELAY_MS = 1000,	// 이보다 밀리면 따라잡지 않고 기준 시각을 다시 잡음
		MOVE_RESEND_TICKS = 4,		// 움직이는 중에는 4틱(200ms)마다 위치를 다시 보냄
	};

	static constexpr int32	MAX_INPUTS_PER_SECOND = 30;	// 정상 클라는 바뀔 때만 보내서 보통 10개 이하
	static constexpr float	MIN_DIR_LENGTH = 0.01f;		// 이보다 짧은 방향은 0(멈춤)으로 봄
	static constexpr float	MOVE_SPEED = 5.f;							// m/s
	static constexpr float	TICK_DT = TICK_INTERVAL_MS / 1000.f;		// 한 틱 = 0.05초 (실제 간격과 관계없이 고정 -> 재현 가능)
	static constexpr float	WORLD_HALF_SIZE = 100.f;					// 월드 200x200, 가운데가 (0, 0)
	static constexpr float	RAD_TO_DEG = 57.2957795f;					// 라디안 -> 도 (180 / π)

public:
	void	Start();				// 첫 틱 예약 (main에서 한 번)
	void	Enter(PlayerRef player);
	void	Leave(uint64 objectId);
	void	HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq);

private:
	void	Tick();
	void	ResetInputCounts();
	void	UpdateMovement();
	bool	ApplyInput(const PlayerRef& player);
	void	MovePlayer(const PlayerRef& player);
	void	RejectInput(PlayerRef player, uint32 seq, const char* reason);
	void	Broadcast(SendBufferRef sendBuffer, uint64 exceptId = 0);
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음

	uint64	_tick = 0;				// 서버 틱 번호 (S_MOVE의 server_tick)
	uint64	_nextTickTime = 0;		// 다음 틱 목표 시각 (GetTickCount64 기준, ms)
	uint64	_logStartTime = 0;		// 틱 간격 측정 시작 시각
};

extern RoomRef GRoom;
