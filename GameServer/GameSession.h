#pragma once
#include "Session.h"

// 클라이언트 연결 하나 (네트워크 단위). 게임 상태(플레이어 등)는 별도 객체로 두고 여기서 참조한다
class GameSession : public PacketSession
{
public:
	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;
};
