#pragma once

namespace Protocol { class MoveInfo; }	// 참조로만 받으니 선언만

// 몬스터 이동을 이번 틱 S_MOVE에 넣을지 정하는 규칙
//   Strategy 패턴: 규칙마다 클래스를 만들고, Room은 인터페이스(MoveSendPolicy)만 보고 호출
//   -> GameServer.exe --monster-send <Name()>으로 실행할 때 골라 끼움 (프로파일링 비교용)
class MoveSendPolicy
{
public:
	virtual ~MoveSendPolicy() = default;

	virtual const char*	Name() const = 0;
	// lastSent: 이 객체를 마지막으로 보낸 이동, now: 이번 틱 이동, resendTick: 4틱 주기 틱인지
	virtual bool		ShouldSend(const Protocol::MoveInfo& lastSent, const Protocol::MoveInfo& now, bool resendTick) const = 0;
};

// 기본: 출발/멈춤/속도 변화/10도 넘게 꺾임은 바로, 그 외 움직이는 중이면 4틱마다
class AngleSendPolicy : public MoveSendPolicy
{
	static constexpr float	COS_TURN_THRESHOLD = 0.98480775f;	// cos(10도)

public:
	virtual const char*	Name() const override { return "angle"; }
	virtual bool		ShouldSend(const Protocol::MoveInfo& lastSent, const Protocol::MoveInfo& now, bool resendTick) const override;
};

// 비교용: 움직이는 동안 매 틱(초당 20번) + 멈춘 순간
class EveryTickSendPolicy : public MoveSendPolicy
{
public:
	virtual const char*	Name() const override { return "every-tick"; }
	virtual bool		ShouldSend(const Protocol::MoveInfo& lastSent, const Protocol::MoveInfo& now, bool resendTick) const override;
};

// 이름으로 정책 찾기. 모르는 이름이면 nullptr (반환된 객체는 프로그램이 끝날 때까지 살아 있음)
const MoveSendPolicy* FindMoveSendPolicy(const string& name);
