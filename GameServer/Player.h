#pragma once
#include "GameObject.h"

// 플레이어 = 월드 객체 + 접속 중인 세션
class Player : public GameObject
{
public:
	Player(GameSessionRef session)
		: GameObject(Protocol::OBJECT_TYPE_PLAYER), _session(session)
	{
	}

	// 연결이 이미 끊겼으면 nullptr
	GameSessionRef	GetSession() { return _session.lock(); }

private:
	weak_ptr<GameSession>	_session;	// 세션이 Player를 shared_ptr로 들고 있으므로 이쪽은 weak (순환 참조 방지)
};
