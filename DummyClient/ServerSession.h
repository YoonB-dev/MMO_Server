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
	set<uint64>	monsterIds;			// S_SPAWN으로 받은 몬스터 id, S_DIE로 빠짐 (S_MOVE에서 플레이어/몬스터 항목 구분용)
	float	myX = 0.f;				// 내 캐릭터의 마지막으로 받은 위치 (S_ENTER_GAME, 내 S_MOVE 항목)
	float	myZ = 0.f;
	uint64	chaseTargetId = 0;		// --chase-test: 쫓아갈 몬스터 id (0 = 아직 안 고름)
	float	chaseTargetX = 0.f;		// --chase-test: 그 몬스터의 마지막으로 받은 위치
	float	chaseTargetZ = 0.f;
	uint32	chaseSeq = 0;			// --chase-test: 보낸 이동 입력 번호
	bool	chaseStopSent = false;	// --chase-test: 5m 안에 들어와서 멈춤 입력을 보냈는지
	bool	chaseDone = false;		// --chase-test: 결과를 출력했는지
	bool	turnStrafeSent = false;	// --turn-test: 옆으로 피하는 입력을 보냈는지
	int32	lastTargetState = 0;	// --turn-test: 타겟 몬스터의 직전 항목 MoveState (MOVING -> MOVING 구분용)
	bool	attackBurstSent = false;	// --attack-test: 검증용 공격 묶음을 보냈는지 (이후 S_PING마다 자동 공격)
	int64	lastSkillUs = 0;		// --attack-test: 마지막으로 내 스킬이 확정된(S_SKILL) 시각. 클라 쪽 쿨타임 계산용
	int32	autoAttackCount = 0;	// --attack-test: 보낸 자동 공격 수 (0: 먼 몬스터, 1: 허공, 이후: 타겟)
	bool	rangedShotSent = false;	// --ranged-test: 원거리 스킬을 보냈는지
	bool	dodgeSent = false;		// --dodge-test: 몬스터 예고를 보고 피하는 입력을 보냈는지

private:
	int32 _clientId = 0;	// 이 프로세스 안에서 1, 2, 3 ... (로그 구분용)
};

// main에서 접속 전에 한 번 정하고 이후 읽기만 함
extern bool GInputTest;	// --input-test: 입장하면 검증용 입력 묶음을 한 번 보냄
extern bool GMoveTest;	// --move-test: 입장하면 오른쪽으로 약 1초 이동 후 멈추고 결과 판정
extern bool GMonsterLog;	// --monster-log: S_MOVE의 몬스터 항목을 한 줄씩 출력
extern bool GChaseTest;	// --chase-test: 가장 가까운 몬스터 쪽으로 걸어가서, 몬스터가 쫓아와 멈추는지 판정
extern bool GTurnTest;	// --turn-test: chase-test 후 옆으로 피하면서, 몬스터 방향이 꺾일 때 바로 오는 S_MOVE 확인
extern bool GFleeTest;	// --flee-test: chase-test 후 몬스터 반대 방향으로 도망 (몬스터 복귀 확인용)
extern bool GAttackTest;	// --attack-test: chase-test 후 앞에 멈춘 몬스터를 공격 (검증용 묶음 + 1초마다 기본 공격)
extern bool GRangedTest;	// --ranged-test: 몬스터에게서 10m 떨어진 곳에 멈춰 원거리 스킬 (어그로, MP 확인용)
extern bool GDodgeTest;	// --dodge-test: chase-test 후 나를 노린 몬스터 예고 공격을 보고 반대로 달려서 피함

int64 NowUs();

// 여러 IOCP 스레드가 동시에 출력해도 줄이 섞이지 않게, 완성된 한 줄을 락 안에서 한 번에 출력
void Log(const string& line);
void Log(int32 clientId, const string& message);	// "[Client 2] message"
