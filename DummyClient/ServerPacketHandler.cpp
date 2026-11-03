#include "pch.h"
#include "ServerPacketHandler.h"
#include "ServerSession.h"
#include <sstream>

// ServerPacketHandler.h는 빌드할 때마다 Protocol.proto에서 생성된다
//   -> S_로 시작하는 패킷을 proto에 추가하면 여기에 Handle_S_XXX를 만들어야 링크된다
PacketHandleFunc GPacketHandler[UINT16_MAX];

static int32 ClientIdOf(PacketSessionRef& session)
{
	return static_pointer_cast<ServerSession>(session)->GetClientId();
}

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	Log(ClientIdOf(session), "Invalid packet - ID: " + to_string(header->id));
	return false;
}

bool Handle_S_PING(PacketSessionRef& session, Protocol::S_PING& pkt)
{
	double rttMs = (NowUs() - pkt.client_time_us()) / 1000.0;

	ostringstream line;
	line << "S_PING RTT " << rttMs << "ms";
	Log(ClientIdOf(session), line.str());
	return true;
}

static void SendMoveInput(PacketSessionRef& session, float dirX, float dirZ, uint32 seq)
{
	Protocol::C_MOVE_INPUT pkt;
	pkt.set_dir_x(dirX);
	pkt.set_dir_z(dirZ);
	pkt.set_seq(seq);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// 서버 입력 검증 확인용 (Test/Scenarios/MoveInputValidation.ps1)
static void SendInputTestBurst(PacketSessionRef& session)
{
	const float nan = numeric_limits<float>::quiet_NaN();

	SendMoveInput(session, 1.f, 0.f, 1);		// 통과
	SendMoveInput(session, 100.f, 0.f, 2);		// 통과 (길이 1로 줄어듦)
	SendMoveInput(session, nan, 0.f, 3);		// reject: invalid dir
	SendMoveInput(session, 0.f, 1.f, 2);		// reject: old seq
	SendMoveInput(session, 0.f, 0.f, 4);		// 통과 (멈춤)

	// 개수 제한: 위 5개 포함 총 70개를 한꺼번에 보냄
	//   -> 중간에 1초 경계(카운트 초기화)가 껴서 둘로 나뉘어도 한쪽은 35개 이상이라 rate가 반드시 생김
	for (uint32 seq = 10; seq < 75; seq++)
		SendMoveInput(session, 1.f, 0.f, seq);
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	ostringstream line;
	line << "S_ENTER_GAME success=" << pkt.success() << " myId=" << pkt.player().object_id();
	Log(ClientIdOf(session), line.str());

	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	me->myId = pkt.player().object_id();
	me->myX = pkt.player().move_info().pos_info().x();
	me->myZ = pkt.player().move_info().pos_info().z();

	if (GInputTest)
		SendInputTestBurst(session);
	if (GMoveTest)
		SendMoveInput(session, 100.f, 0.f, 1);	// 오른쪽. 길이 100 -> 서버가 1로 줄여야 정상 속도
	return true;
}

// 몬스터 추적 확인용 (Test/Scenarios/MonsterChase.ps1)
//   S_SPAWN 몬스터 중 가장 가까운 것 쪽으로 걷기 -> 5m 안이면 멈춤 -> 몬스터가 다가와 멈추면 거리 판정
//   서버는 1.5m(STOP_RANGE)에서 멈추고, 내 위치/몬스터 위치 모두 패킷 기준이라 여유를 두고 2m 이하면 ok
static void AimAtChaseTarget(PacketSessionRef& session)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	SendMoveInput(session, me->chaseTargetX - me->myX, me->chaseTargetZ - me->myZ, ++me->chaseSeq);	// 길이는 서버가 1로 줄임
}

static void StartChaseTest(PacketSessionRef& session, const Protocol::S_SPAWN& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	float bestDistSq = (numeric_limits<float>::max)();	// 괄호: Windows.h의 max 매크로로 바뀌지 않게
	for (const Protocol::ObjectInfo& object : pkt.objects())
	{
		if (object.object_type() != Protocol::OBJECT_TYPE_MONSTER)
			continue;

		const Protocol::PosInfo& pos = object.move_info().pos_info();
		const float dx = pos.x() - me->myX;
		const float dz = pos.z() - me->myZ;
		const float distSq = dx * dx + dz * dz;
		if (distSq < bestDistSq)
		{
			bestDistSq = distSq;
			me->chaseTargetId = object.object_id();
			me->chaseTargetX = pos.x();
			me->chaseTargetZ = pos.z();
		}
	}

	if (me->chaseTargetId != 0)
		AimAtChaseTarget(session);
}

// 방향 꺾임 전송 확인용 (Test/Scenarios/MonsterTurnSend.ps1, --turn-test = 위 추적 + 아래)
//   몬스터가 앞에 멈춘 뒤 몬스터 쪽과 수직으로 걸어감 -> 쫓아오는 몬스터의 방향이 계속 꺾임
//   4틱 주기 틱도 아니고 출발/멈춤도 아닌데(이전 항목도 MOVING) 몬스터 항목이 오면
//   = 서버 전송 정책(angle)이 꺾임을 보고 바로 보낸 것 -> 'TURN_SEND'
static constexpr uint64 SERVER_RESEND_TICKS = 4;	// 서버 Room::MOVE_RESEND_TICKS와 같은 값

static void UpdateChaseTest(PacketSessionRef& session, uint64 serverTick, const Protocol::MoveEntry& entry)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	const Protocol::MoveInfo& move = entry.move_info();
	me->chaseTargetX = move.pos_info().x();
	me->chaseTargetZ = move.pos_info().z();

	const int32 prevState = me->lastTargetState;
	me->lastTargetState = move.state();

	const float dx = me->chaseTargetX - me->myX;
	const float dz = me->chaseTargetZ - me->myZ;
	const float dist = std::sqrt(dx * dx + dz * dz);

	// 걸어가는 중: 5m 안이면 멈춤, 아니면 몬스터가 움직였을 수 있으니 다시 조준
	if (me->chaseStopSent == false)
	{
		if (dist <= 5.f)
		{
			SendMoveInput(session, 0.f, 0.f, ++me->chaseSeq);
			me->chaseStopSent = true;
		}
		else
		{
			AimAtChaseTarget(session);
		}
		return;
	}

	// 멈춘 뒤: 몬스터가 다가와서 멈추면(IDLE) 판정
	if (me->chaseDone == false)
	{
		if (move.state() == Protocol::MOVE_STATE_IDLE)
		{
			ostringstream line;
			line << "CHASE_TEST " << (dist <= 2.f ? "ok" : "fail") << " target=" << entry.object_id() << " dist=" << dist;
			Log(me->GetClientId(), line.str());
			me->chaseDone = true;

			// --turn-test: 몬스터 쪽 방향 (dx, dz)에 수직인 (-dz, dx)로 피함
			if (GTurnTest)
			{
				SendMoveInput(session, -dz, dx, ++me->chaseSeq);
				me->turnStrafeSent = true;
			}
		}
		return;
	}

	const bool movingToMoving = (prevState == Protocol::MOVE_STATE_MOVING && move.state() == Protocol::MOVE_STATE_MOVING);
	if (me->turnStrafeSent && movingToMoving && serverTick % SERVER_RESEND_TICKS != 0)
	{
		ostringstream line;
		line << "TURN_SEND tick=" << serverTick << " target=" << entry.object_id() << " dist=" << dist;
		Log(me->GetClientId(), line.str());
	}
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	ostringstream line;
	line << "S_SPAWN ids:";
	for (const Protocol::ObjectInfo& object : pkt.objects())
	{
		line << " " << object.object_id();
		if (object.object_type() == Protocol::OBJECT_TYPE_MONSTER)
		{
			line << "(monster)";
			me->monsterIds.insert(object.object_id());
		}
	}
	Log(me->GetClientId(), line.str());

	if (GChaseTest && me->chaseTargetId == 0)
		StartChaseTest(session, pkt);
	return true;
}

// 서버 이동 확인용 (Test/Scenarios/Move.ps1)
//   seq 1 (오른쪽) -> 처음 MOVING이 찍힌 틱부터 20틱 이상 지난 S_MOVE를 받으면 seq 2 (멈춤)
//   -> 멈춤(IDLE, ack 2)이 오면 최종 위치 판정
//   멈춤은 4틱마다 오는 S_MOVE를 받고 보내고, 서버에 도착하는 데 0~1틱이 더 걸려서 x는 정확히 5가 아니라 5.25 ~ 6.5
static void UpdateMoveTest(PacketSessionRef& session, uint64 serverTick, const Protocol::MoveEntry& entry)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	const Protocol::MoveInfo& move = entry.move_info();

	if (me->moveStartTick == 0 && move.state() == Protocol::MOVE_STATE_MOVING)
		me->moveStartTick = serverTick;

	if (me->moveStopSent == false && me->moveStartTick != 0 && serverTick >= me->moveStartTick + 20)
	{
		SendMoveInput(session, 0.f, 0.f, 2);
		me->moveStopSent = true;
		return;
	}

	if (me->moveStopSent && move.state() == Protocol::MOVE_STATE_IDLE && entry.ack_seq() == 2)
	{
		const Protocol::PosInfo& pos = move.pos_info();
		const bool ok = (pos.x() >= 5.f && pos.x() <= 6.5f)
			&& std::fabs(pos.z()) < 0.001f
			&& std::fabs(pos.yaw() - 90.f) < 0.01f;

		ostringstream line;
		line << "MOVE_TEST " << (ok ? "ok" : "fail") << " x=" << pos.x() << " z=" << pos.z() << " yaw=" << pos.yaw();
		Log(me->GetClientId(), line.str());
	}
}

bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	// 플레이어/몬스터 항목 수를 나눠서 출력 (몬스터는 S_SPAWN에서 기록해 둔 id로 구분)
	int32 monsterCount = 0;
	for (const Protocol::MoveEntry& entry : pkt.moves())
	{
		if (me->monsterIds.count(entry.object_id()) > 0)
			monsterCount++;
	}

	ostringstream line;
	line << "S_MOVE tick=" << pkt.server_tick()
		<< " players=" << (pkt.moves_size() - monsterCount) << " monsters=" << monsterCount;
	Log(me->GetClientId(), line.str());

	// 몬스터 항목(--monster-log)과 내 캐릭터 항목만 따로 출력
	for (const Protocol::MoveEntry& entry : pkt.moves())
	{
		const Protocol::MoveInfo& move = entry.move_info();

		if (me->monsterIds.count(entry.object_id()) > 0)
		{
			if (GMonsterLog)
			{
				ostringstream monsterLine;
				monsterLine << "MONSTER_MOVE tick=" << pkt.server_tick() << " id=" << entry.object_id()
					<< " x=" << move.pos_info().x() << " z=" << move.pos_info().z()
					<< " state=" << move.state() << " speed=" << move.speed();
				Log(me->GetClientId(), monsterLine.str());
			}

			if (GChaseTest && entry.object_id() == me->chaseTargetId)
				UpdateChaseTest(session, pkt.server_tick(), entry);
			continue;
		}

		if (entry.object_id() != me->myId)
			continue;

		// 플레이어 항목이 몬스터 항목보다 앞에 오므로(서버 UpdatePlayers -> UpdateMonsters), 추적 판정 전에 내 위치가 먼저 갱신됨
		me->myX = move.pos_info().x();
		me->myZ = move.pos_info().z();

		ostringstream myLine;
		myLine << "MY_MOVE tick=" << pkt.server_tick()
			<< " x=" << move.pos_info().x() << " z=" << move.pos_info().z() << " yaw=" << move.pos_info().yaw()
			<< " state=" << move.state() << " ack=" << entry.ack_seq();
		Log(me->GetClientId(), myLine.str());

		if (GMoveTest)
			UpdateMoveTest(session, pkt.server_tick(), entry);
	}
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	ostringstream line;
	line << "S_DESPAWN ids:";
	for (uint64 objectId : pkt.object_ids())
		line << " " << objectId;
	Log(ClientIdOf(session), line.str());
	return true;
}
