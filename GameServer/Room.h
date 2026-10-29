#pragma once
#include "JobQueue.h"

// 게임 월드(전역 방 하나). 입장/퇴장/이동은 전부 DoAsync로 이 JobQueue에 넣어서 한 줄로 처리한다
class Room : public JobQueue
{
public:
	void	Enter(PlayerRef player);
	void	Leave(uint64 objectId);

private:
	void	Broadcast(SendBufferRef sendBuffer, uint64 exceptId = 0);
	void	SendTo(PlayerRef player, SendBufferRef sendBuffer);

private:
	xunordered_map<uint64, PlayerRef>	_players;	// JobQueue 안에서만 접근 -> 락 없음
};

extern RoomRef GRoom;
