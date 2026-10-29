#pragma once
#include "Session.h"

// 더미 클라이언트 쪽 서버 연결 하나
class ServerSession : public PacketSession
{
public:
	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;
};

int64 NowUs();
