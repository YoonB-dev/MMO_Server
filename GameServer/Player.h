#pragma once
#include "GameObject.h"

// 클라가 보낸 이동 입력 하나 (C_MOVE_INPUT의 필드와 1:1)
struct MoveInput
{
	float	dirX = 0.f;		// 좌우 (-1 ~ 1)
	float	dirZ = 0.f;		// 앞뒤 (-1 ~ 1)
	uint32	buttons = 0;	// bit0 = 점프 (저장만 하고 미사용)
	uint32	seq = 0;		// 클라가 붙인 입력 번호 1, 2, 3 ...
};

// 플레이어 = 월드 객체 + 접속 중인 세션
class Player : public GameObject
{
	// 플레이어 기본 수치 (몬스터, 스킬과 달리 DB가 아니라 코드 상수)
	static constexpr int32	MAX_HP = 100;

	// 1초당 자연 회복량. 회복 규칙은 Regen() 한 곳에만 있음
	static constexpr int32	HP_REGEN_PER_SECOND = 1;
	static constexpr int32	MP_REGEN_PER_SECOND = 1;	// 원거리 스킬 최대 소모(초당 2)보다 작아야 MP가 의미 있음

public:
	// MP는 본인만 보는 자원 -> ObjectInfo가 아니라 Player 멤버 (S_ENTER_GAME / S_CHANGE_MP로 본인에게만)
	static constexpr int32	MAX_MP = 100;

	// 죽음 / 부활
	static constexpr uint64	REVIVE_DELAY_MS = 5000;	// 죽고 나서 부활까지 (DoTimer 시간이라 uint64)
	static constexpr float	SPAWN_X = 0.f;			// 입장, 부활 위치
	static constexpr float	SPAWN_Z = 0.f;

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
		GetPosInfo()->set_x(SPAWN_X);
		GetPosInfo()->set_z(SPAWN_Z);
		GetStatInfo()->set_hp(MAX_HP);
		GetStatInfo()->set_max_hp(MAX_HP);
	}

	// 연결이 이미 끊겼으면 nullptr
	GameSessionRef	GetSession() { return _session.lock(); }

	// 죽음 = HP 0. 따로 상태를 두지 않음 (HP와 상태가 어긋날 일이 없게)
	bool	IsDead() { return GetStatInfo()->hp() == 0; }

	// 시작 위치에서 다시 살아남: 정지, HP 가득, MP 절반. 패킷은 Room이 보냄
	void Revive()
	{
		Protocol::MoveInfo* moveInfo = GetMoveInfo();
		moveInfo->set_dir_x(0.f);
		moveInfo->set_dir_z(0.f);
		moveInfo->set_speed(0.f);
		moveInfo->set_state(Protocol::MOVE_STATE_IDLE);
		moveInfo->mutable_pos_info()->set_x(SPAWN_X);
		moveInfo->mutable_pos_info()->set_z(SPAWN_Z);

		lastInput.dirX = 0.f;	// seq는 그대로 (클라가 이어서 보내는 번호)
		lastInput.dirZ = 0.f;
		hasNewInput = false;

		GetStatInfo()->set_hp(MAX_HP);
		mp = MAX_MP / 2;
	}

	// 1초치 자연 회복 (Room::UpdatePlayerStats가 호출). 값만 바꾸고 무엇이 바뀌었는지 돌려줌 -> 보내는 건 Room
	RegenResult Regen()
	{
		RegenResult result;
		if (IsDead())
			return result;	// 죽은 동안은 회복 안 함 (부활은 Revive로만)

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
	bool		hasNewInput = false;	// 이번 틱에 적용할 새 입력이 있는지 (있으면 이번 S_MOVE에 바로 실림)
	int32		inputCount = 0;			// 이번 1초 동안 받은 입력 수 (Room::Tick이 20틱마다 0으로)

	// --- 스킬 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	xunordered_map<int32, uint64>	skillReadyTick;	// 스킬 id -> 다시 쓸 수 있는 서버 틱. 처음 쓰는 스킬은 항목이 없음 = 0 = 바로 사용 가능
	int32	mp = MAX_MP;	// 지금 MP (입장할 때 가득)

private:
	weak_ptr<GameSession>	_session;	// 세션이 Player를 shared_ptr로 들고 있으므로 이쪽은 weak (순환 참조 방지)
};
