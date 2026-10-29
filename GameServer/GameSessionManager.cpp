#include "pch.h"
#include "GameSessionManager.h"
#include "GameSession.h"

GameSessionManager GSessionManager;

void GameSessionManager::Add(GameSessionRef session)
{
	WRITE_LOCK;
	_sessions.insert(session);
}

void GameSessionManager::Remove(GameSessionRef session)
{
	WRITE_LOCK;
	_sessions.erase(session);
}

void GameSessionManager::Broadcast(SendBufferRef sendBuffer)
{
	// 락 안에서는 대상만 모으고, Send(세션 락)는 락 밖에서
	vector<GameSessionRef> targets;
	{
		READ_LOCK;
		targets.assign(_sessions.begin(), _sessions.end());
	}

	for (GameSessionRef& session : targets)
	{
		session->Send(sendBuffer);
	}
}
