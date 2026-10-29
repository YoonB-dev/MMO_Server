#pragma once

// 접속 중인 전체 세션 목록
class GameSessionManager
{
public:
	void Add(GameSessionRef session);
	void Remove(GameSessionRef session);
	void Broadcast(SendBufferRef sendBuffer);

	int32 GetSessionCount()
	{
		READ_LOCK;
		return static_cast<int32>(_sessions.size());
	}

private:
	USE_LOCK;
	xset<GameSessionRef> _sessions;
};

extern GameSessionManager GSessionManager;
