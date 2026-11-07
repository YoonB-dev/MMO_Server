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
	// 지금은 아무도 플레이어를 안 때리지만, 클라 HP바가 0/0으로 보이지 않게 채워 둠
	//   (몬스터와 달리 코드 상수: 플레이어 수치는 유저 데이터 단계에서 레벨/캐릭터 테이블로 정함)
	static constexpr int32	MAX_HP = 100;

	// 1초당 자연 회복량. 회복량을 쓰는 곳은 Regen() 한 곳뿐
	//   -> 나중에 회복량 스탯(장비, 레벨)이 생기면 이 상수 대신 그 값을 쓰도록 Regen()만 바꾸면 됨
	static constexpr int32	HP_REGEN_PER_SECOND = 1;
	static constexpr int32	MP_REGEN_PER_SECOND = 1;	// 원거리를 쿨타임마다 쓰면 초당 2 소모 -> 회복이 그보다 작아야 MP가 의미 있음

public:
	// MP: 본인만 보는 자원이라 ObjectInfo(모두에게 보이는 정보)가 아니라 Player 멤버로 (S_ENTER_GAME / S_CHANGE_MP로 본인에게만)
	static constexpr int32	MAX_MP = 100;	// Room이 S_ENTER_GAME의 max_mp에 씀

	// Regen()의 결과: 실제로 바뀐 것만 true (꽉 차 있으면 false -> Room이 패킷을 안 보냄)
	struct RegenResult
	{
		bool	hpChanged = false;
		bool	mpChanged = false;
	};

public:
	Player(GameSessionRef session)
		: GameObject(Protocol::OBJECT_TYPE_PLAYER), _session(session)
	{
		GetStatInfo()->set_hp(MAX_HP);
		GetStatInfo()->set_max_hp(MAX_HP);
	}

	// 연결이 이미 끊겼으면 nullptr
	GameSessionRef	GetSession() { return _session.lock(); }

	// 1초치 자연 회복 (Room::UpdatePlayerStats가 1초마다). 값만 바꾸고 패킷은 Room이 보냄
	//   (나중에 '죽은 상태면 회복 안 함', '전투 중엔 회복 안 함' 같은 규칙도 여기에)
	RegenResult Regen()
	{
		RegenResult result;

		Protocol::StatInfo* stat = GetStatInfo();
		if (stat->hp() < stat->max_hp())
		{
			const int32 hp = stat->hp() + HP_REGEN_PER_SECOND;
			stat->set_hp(hp > stat->max_hp() ? stat->max_hp() : hp);	// std::min은 Windows.h의 min 매크로와 부딪혀서 안 씀
			result.hpChanged = true;
		}

		if (mp < MAX_MP)
		{
			mp += MP_REGEN_PER_SECOND;
			if (mp > MAX_MP)
				mp = MAX_MP;
			result.mpChanged = true;
		}
		return result;
	}

public:
	// --- 이동 입력 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	MoveInput	lastInput;				// 마지막으로 받아들인 입력: 매 틱 이 방향으로 이동 + seq로 순서 검사
	bool		hasNewInput = false;	// 아직 틱이 확인 안 한 새 입력이 있는지 (4단계: 있으면 S_MOVE 바로 전송)
	int32		inputCount = 0;			// 이번 1초 동안 받은 입력 수 (Room::Tick이 20틱마다 0으로)

	// --- 스킬 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	xunordered_map<int32, uint64>	skillReadyTick;	// 스킬 id -> 다시 쓸 수 있는 서버 틱. 처음 쓰는 스킬은 항목이 없음 = 0 = 바로 사용 가능
	int32	mp = MAX_MP;	// 지금 MP (입장할 때 가득)

private:
	weak_ptr<GameSession>	_session;	// 세션이 Player를 shared_ptr로 들고 있으므로 이쪽은 weak (순환 참조 방지)
};
