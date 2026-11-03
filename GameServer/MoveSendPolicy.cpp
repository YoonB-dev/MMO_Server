#include "pch.h"
#include "MoveSendPolicy.h"
#include "Struct.pb.h"

bool AngleSendPolicy::ShouldSend(const Protocol::MoveInfo& lastSent, const Protocol::MoveInfo& now, bool resendTick) const
{
	if (now.state() != lastSent.state())
		return true;	// 출발/멈춤
	if (now.state() != Protocol::MOVE_STATE_MOVING)
		return false;	// 계속 멈춰 있음
	if (now.speed() != lastSent.speed())
		return true;	// 걷기 <-> 뛰기

	// 두 방향 모두 길이 1 -> 내적 = cos(사이각). cos이 작을수록 많이 꺾임 (acos 없이 비교)
	const float dot = lastSent.dir_x() * now.dir_x() + lastSent.dir_z() * now.dir_z();
	if (dot < COS_TURN_THRESHOLD)
		return true;

	return resendTick;
}

bool EveryTickSendPolicy::ShouldSend(const Protocol::MoveInfo& lastSent, const Protocol::MoveInfo& now, bool resendTick) const
{
	return now.state() == Protocol::MOVE_STATE_MOVING || now.state() != lastSent.state();
}

// 정책은 상태가 없어서 하나씩만 만들어 두고 같이 씀
static const AngleSendPolicy		s_anglePolicy;
static const EveryTickSendPolicy	s_everyTickPolicy;

const MoveSendPolicy* FindMoveSendPolicy(const string& name)
{
	if (name == s_anglePolicy.Name())
		return &s_anglePolicy;
	if (name == s_everyTickPolicy.Name())
		return &s_everyTickPolicy;
	return nullptr;
}
