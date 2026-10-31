#pragma once
#include "GameObject.h"

// 클라가 보낸 이동 입력 하나 (C_MOVE_INPUT의 필드와 1:1)
struct MoveInput
{
	float	dirX = 0.f;		// 좌우 (-1 ~ 1)
	float	dirZ = 0.f;		// 앞뒤 (-1 ~ 1)
	uint32	buttons = 0;	// bit0 = 점프 (지금은 저장만)
	uint32	seq = 0;		// 클라가 붙인 입력 번호 1, 2, 3 ...
};

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

public:
	// --- 이동 입력 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	MoveInput	lastInput;				// 마지막으로 받아들인 입력: 매 틱 이 방향으로 이동 + seq로 순서 검사
	bool		hasNewInput = false;	// 아직 틱이 확인 안 한 새 입력이 있는지 (4단계: 있으면 S_MOVE 바로 전송)
	int32		inputCount = 0;			// 이번 1초 동안 받은 입력 수 (Room::Tick이 20틱마다 0으로)

private:
	weak_ptr<GameSession>	_session;	// 세션이 Player를 shared_ptr로 들고 있으므로 이쪽은 weak (순환 참조 방지)
};
