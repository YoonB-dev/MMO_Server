#pragma once
#include "Session.h"
#include "Enum.pb.h"

// 클라이언트 연결 하나 (네트워크 단위). 게임 상태(플레이어 등)는 별도 객체로 두고 여기서 참조한다
class GameSession : public PacketSession
{
public:
	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;

	// 플레이어는 여러 스레드가 만짐 -> 락으로 보호
	//   쓰기: DB 큐 (입장할 때 SetPlayer), IOCP 스레드 (끊길 때 OnDisconnected)
	//   읽기: IOCP 스레드 (패킷 핸들러의 GetPlayer)
	//   shared_ptr의 참조 카운트는 atomic이지만, "같은 변수"를 동시에 쓰고 읽는 건 데이터 경쟁이라 락이 필요
	PlayerRef	GetPlayer();					// 락 안에서 복사본을 돌려줌
	bool		SetPlayer(PlayerRef player);	// 이미 끊긴 세션이면 false (입장시키지 말 것)

	// 입장 처리 중 / 입장함 표시 (C_ENTER_GAME 중복 방지). 처음이면 true
	bool		TryBeginEnter() { return _entering.exchange(true) == false; }
	// 입장 실패: S_ENTER_GAME{success=false, error} 응답 + 다시 시도할 수 있게 표시를 풂 (연결은 유지)
	void		FailEnter(Protocol::EnterGameError error);

private:
	Lock			_playerLock;		// _player, _closed 보호 (ServerCore의 Lock: 데드락 탐지 포함)
	PlayerRef		_player;			// 입장 전에는 nullptr
	bool			_closed = false;	// 끊김 처리됨 -> 이후 SetPlayer는 실패
	Atomic<bool>	_entering = false;	// IOCP 스레드(C_ENTER_GAME)와 DB 큐(실패 시 해제)가 같이 만져서 atomic
};
