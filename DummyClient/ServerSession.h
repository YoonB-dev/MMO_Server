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

public:
	// 이 세션의 패킷 핸들러에서만 접근 (한 세션의 recv는 한 번에 하나씩 처리되므로 락 없음)
	uint64	myId = 0;				// S_ENTER_GAME으로 받은 내 캐릭터 id
	uint64	moveStartTick = 0;		// --move-test: 내가 처음 MOVING으로 찍힌 server_tick
	bool	moveStopSent = false;	// --move-test: 멈춤 입력을 보냈는지

private:
	int32 _clientId = 0;	// 이 프로세스 안에서 1, 2, 3 ... (로그 구분용)
};

// main에서 접속 전에 한 번 정하고 이후 읽기만 함
extern bool GInputTest;	// --input-test: 입장하면 검증용 입력 묶음을 한 번 보냄
extern bool GMoveTest;	// --move-test: 입장하면 오른쪽으로 약 1초 이동 후 멈추고 결과 판정

int64 NowUs();

// 여러 IOCP 스레드가 동시에 출력해도 줄이 섞이지 않게, 완성된 한 줄을 락 안에서 한 번에 출력
void Log(const string& line);
void Log(int32 clientId, const string& message);	// "[Client 2] message"
