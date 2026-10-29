#pragma once
#include "Session.h"

// 더미 클라이언트 쪽 서버 연결 하나
class ServerSession : public PacketSession
{
public:
	ServerSession();

	int32 GetClientId() const { return _clientId; }

	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;

private:
	int32 _clientId = 0;	// 이 프로세스 안에서 1, 2, 3 ... (로그 구분용)
};

int64 NowUs();

// 여러 IOCP 스레드가 동시에 출력해도 줄이 섞이지 않게, 완성된 한 줄을 락 안에서 한 번에 출력
void Log(const string& line);
void Log(int32 clientId, const string& message);	// "[Client 2] message"
