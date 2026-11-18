#include "pch.h"
#include "ServerPacketHandler.h"
#include "ServerSession.h"
#include <sstream>
#include <random>

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

static constexpr int32	BASIC_ATTACK_SKILL_ID = 1;			// Database/Data/skill.sql의 기본 공격 skill_id
static constexpr int64	BASIC_ATTACK_COOLDOWN_US = 500'000;	// 같은 줄의 cooldown_ms(500)와 맞춰야 함 (DB를 바꾸면 여기도)
static constexpr int32	RANGED_SKILL_ID = 2;					// 원거리 스킬 (대상 필수)

static void SendSkill(PacketSessionRef& session, int32 skillId, uint64 targetId)
{
	Protocol::C_SKILL pkt;
	pkt.set_skill_id(skillId);
	pkt.set_target_id(targetId);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// 타겟이 아닌 다른 스폰의 몬스터 (스폰끼리 멀리 떨어져 있어서 기본 공격 사거리 밖). 없으면 0
static uint64 FindFarMonster(const shared_ptr<ServerSession>& me)
{
	for (uint64 id : me->monsterIds)
	{
		if (id != me->chaseTargetId)
			return id;
	}
	return 0;
}

// 공격 검증 확인용 (Test/Scenarios/MonsterAttack.ps1)
//   추적 테스트로 몬스터가 앞에 멈춘 뒤 한 번에 보냄. 서버는 거절/빗나감 이유별 횟수를 10초 통계에 출력
//   기본 공격은 빗나가도 쿨타임을 쓰므로, 기본 공격의 빗나감은 이후 자동 공격(TryAutoAttack)에서 하나씩 확인
static void SendAttackTestBurst(PacketSessionRef& session)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	SendSkill(session, 99, me->chaseTargetId);						// 거절: unknown-skill
	SendSkill(session, RANGED_SKILL_ID, 0);							// 거절: no-target (원거리는 대상 필수)
	SendSkill(session, RANGED_SKILL_ID, me->myId);					// 거절: bad-target (플레이어 id)
	SendSkill(session, BASIC_ATTACK_SKILL_ID, me->chaseTargetId);	// 통과 -> S_SKILL, S_CHANGE_HP hp=190
	SendSkill(session, BASIC_ATTACK_SKILL_ID, me->chaseTargetId);	// 거절: cooldown (0.5초 안에 또)

	me->attackBurstSent = true;
	me->lastSkillUs = NowUs();	// S_SKILL보다 S_PING이 먼저 도착해도 바로 또 보내지 않게
}

// --attack-test: 1초마다 오는 S_PING을 타이머 대신 써서 기본 공격
//   진짜 클라처럼 내 S_SKILL을 받은 뒤 쿨타임(0.5초)이 지났을 때만 보냄 -> 서버에서 거절되면 안 됨
//   1번째: 먼 몬스터 (빗나감 out-of-range), 2번째: 허공 target 0 (빗나감 no-target), 그다음부터 타겟
static void TryAutoAttack(PacketSessionRef& session)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	if (me->attackBurstSent == false)
		return;		// 아직 몬스터 앞에 도착 전
	if (me->monsterIds.count(me->chaseTargetId) == 0)
		return;		// 타겟이 죽음 (S_DIE에서 지움) -> 더 안 때림
	if (NowUs() - me->lastSkillUs < BASIC_ATTACK_COOLDOWN_US)
		return;

	uint64 targetId = me->chaseTargetId;
	if (me->autoAttackCount == 0)
		targetId = FindFarMonster(me);
	else if (me->autoAttackCount == 1)
		targetId = 0;	// 허공

	SendSkill(session, BASIC_ATTACK_SKILL_ID, targetId);
	me->autoAttackCount++;
	me->lastSkillUs = NowUs();	// 빗나감도 S_SKILL이 오지만, 그 전에 다음 S_PING이 와도 쿨타임 안에 또 보내지 않게
}

bool Handle_S_PING(PacketSessionRef& session, Protocol::S_PING& pkt)
{
	double rttMs = (NowUs() - pkt.client_time_us()) / 1000.0;

	ostringstream line;
	line << "S_PING RTT " << rttMs << "ms";
	Log(ClientIdOf(session), line.str());

	if (GAttackTest)
		TryAutoAttack(session);
	if (GPickupTest)	// 주울 게 없으면 서버가 무시 (응답 없음)
	{
		Protocol::C_PICKUP pickupPkt;	// MakeSendBuffer가 non-const 참조를 받아서 임시 객체 대신 변수로
		session->Send(ServerPacketHandler::MakeSendBuffer(pickupPkt));
	}
	return true;
}

static void SendChat(PacketSessionRef& session, Protocol::ChatChannel channel, const string& message)
{
	Protocol::C_CHAT pkt;
	pkt.set_channel(channel);
	pkt.set_message(message);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// UTF-8 바이트를 16진수로 (한글 이름은 콘솔/로그 파일 인코딩에 따라 깨져 보여서, 테스트는 이 값으로 비교)
static string HexOf(const string& text)
{
	static const char* digits = "0123456789abcdef";
	string hex;
	for (const unsigned char c : text)
	{
		hex.push_back(digits[c >> 4]);
		hex.push_back(digits[c & 0x0F]);
	}
	return hex;
}

// 채팅 확인용 (Test/Scenarios/Chat.ps1, --chat-test)
//   거절될 것 3개(빈 문자열, 101글자, 줄바꿈) -> 주변 "hello"(통과) -> 바로 방 "again"(0.5초 쿨타임으로 거절)
//   서버는 내용 검사에서 거절된 건 쿨타임을 안 쓰므로 "hello"는 통과해야 함
static void SendChatTestBurst(PacketSessionRef& session)
{
	SendChat(session, Protocol::CHAT_CHANNEL_NEARBY, "");
	SendChat(session, Protocol::CHAT_CHANNEL_NEARBY, string(101, 'x'));
	SendChat(session, Protocol::CHAT_CHANNEL_NEARBY, "a\nb");
	SendChat(session, Protocol::CHAT_CHANNEL_NEARBY, "hello");
	SendChat(session, Protocol::CHAT_CHANNEL_ROOM, "again");
}

// 서버가 알려 준 칸 하나를 내 인벤토리에 반영 (item_id 0 = 빈 칸)
static void ApplySlot(shared_ptr<ServerSession>& me, const Protocol::InventorySlot& slot)
{
	if (slot.item_id() == 0)
		me->slots.erase(slot.slot());
	else
		me->slots[slot.slot()] = { slot.item_id(), slot.count() };
}

static void SendMoveItem(PacketSessionRef& session, int32 from, int32 to)
{
	Protocol::C_MOVE_ITEM pkt;
	pkt.set_from_slot(from);
	pkt.set_to_slot(to);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// 칸 옮기기 확인용 (Test/Scenarios/InventoryMove.ps1, --move-item-test)
//   인벤토리가 정확히 젤리 50(a), 젤리 10(b), 검(c), 검(d) 네 칸이 되면 한 번에 보냄 (서버는 받은 순서대로 처리)
//     1. b -> 19(빈 칸)  옮기기: b 빈 칸, 19 = 젤리 10
//     2. c -> a           바꾸기: c = 젤리 50, a = 검
//     3. c -> 19          합치기: 19 = 50 (10 + 40), c = 10 남음
//     4. a -> a / b -> a / 99 -> 0   거절 3종 (같은 칸, 빈 칸에서, 범위 밖)
//   끝난 뒤: a 검, c 젤리 10, d 검, 19 젤리 50, b는 빈 칸 (젤리 60, 검 2개 그대로)
static void TryMoveItemTest(PacketSessionRef& session)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	if (GMoveItemTest == false || me->moveItemTestSent || me->slots.size() != 4)
		return;

	int32 a = -1, b = -1, c = -1, d = -1;
	for (auto& [slot, item] : me->slots)
	{
		if (item.first == 1001 && item.second == 50)
			a = slot;
		else if (item.first == 1001 && item.second == 10)
			b = slot;
		else if (item.first == 2001)
			(c < 0 ? c : d) = slot;	// 검 두 칸 중 앞 칸이 c
	}
	if (a < 0 || b < 0 || c < 0 || d < 0 || me->slots.count(19) > 0)
		return;

	me->moveItemTestSent = true;
	SendMoveItem(session, b, 19);
	SendMoveItem(session, c, a);
	SendMoveItem(session, c, 19);
	SendMoveItem(session, a, a);
	SendMoveItem(session, b, a);
	SendMoveItem(session, 99, 0);

	ostringstream line;
	line << "MOVE_ITEM_TEST sent a=" << a << " b=" << b << " c=" << c << " d=" << d;
	Log(me->GetClientId(), line.str());
}

static void SendMoveInput(PacketSessionRef& session, float dirX, float dirZ, uint32 seq)
{
	Protocol::C_MOVE_INPUT pkt;
	pkt.set_dir_x(dirX);
	pkt.set_dir_z(dirZ);
	pkt.set_seq(seq);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// 부하 테스트 봇 (--bot, Test/Load/Run-Load.ps1)
//   ±GBotArea 안의 무작위 지점을 골라 그쪽으로 걷고, 도착하면(2m 안) 다음 지점을 고름
//   입력은 목표를 바꿀 때만 보냄 (진짜 유저처럼 방향이 바뀔 때만) -> 부하의 대부분은 서버가 보내는 S_MOVE 쪽
//   죽었거나 벽에 막혀 10초 안에 못 가면 새로 고름 (죽은 동안 보낸 입력은 서버가 거절함)
static constexpr float	BOT_ARRIVE_DIST = 2.f;
static constexpr int64	BOT_REPICK_US = 10'000'000;

static void PickBotTarget(PacketSessionRef& session)
{
	// thread_local: 스레드마다 따로 하나씩 생기는 변수
	//   mt19937(난수 생성기)은 여러 스레드가 같이 쓰면 안전하지 않음 -> 락 대신 IOCP 스레드마다 하나씩 둠
	//   (ServerCore의 LCurrentJobQueue도 같은 방식). 처음 쓰는 순간 그 스레드에서 한 번만 초기화됨
	thread_local mt19937 rng{ random_device{}() };
	uniform_real_distribution<float> pos(-GBotArea, GBotArea);

	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	me->botTargetX = pos(rng);
	me->botTargetZ = pos(rng);
	me->botPickUs = NowUs();
	SendMoveInput(session, me->botTargetX - me->myX, me->botTargetZ - me->myZ, ++me->chaseSeq);	// 길이는 서버가 1로 줄임
}

static void UpdateBot(PacketSessionRef& session)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	const float dx = me->botTargetX - me->myX;
	const float dz = me->botTargetZ - me->myZ;
	const bool arrived = (dx * dx + dz * dz <= BOT_ARRIVE_DIST * BOT_ARRIVE_DIST);
	if (arrived || NowUs() - me->botPickUs >= BOT_REPICK_US)
		PickBotTarget(session);
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
	line << "S_ENTER_GAME success=" << pkt.success() << " myId=" << pkt.player().object_id()
		<< " mp=" << pkt.mp() << "/" << pkt.max_mp();
	if (pkt.success())
	{
		const Protocol::PosInfo& pos = pkt.player().move_info().pos_info();
		line << " pos=(" << pos.x() << ", " << pos.z() << ") hp=" << pkt.player().stat_info().hp();	// 저장된 캐릭터를 불러왔는지 확인용
		line << " name=" << pkt.player().name() << " name_hex=" << HexOf(pkt.player().name());	// 한글 이름은 name_hex로 비교
		line << " level=" << pkt.player().stat_info().level() << " exp=" << pkt.exp() << " next=" << pkt.exp_to_next();	// 저장된 레벨 / 경험치 확인용
		line << " gold=" << pkt.gold() << " inv=" << pkt.inventory_capacity() << " items=";	// 저장된 골드 / 인벤토리 확인용 (칸:아이템x개수)
		for (int32 i = 0; i < pkt.inventory_size(); i++)	// inventory_size() = repeated inventory의 개수 (protobuf가 만듦)
			line << (i > 0 ? "," : "") << pkt.inventory(i).slot() << ":" << pkt.inventory(i).item_id() << "x" << pkt.inventory(i).count();
	}
	if (pkt.success() == false)
		line << " error=" << pkt.error();	// Enum.proto EnterGameError (1 토큰 오류, 2 만료, 3 중복 요청, 4 서버 오류, 5 캐릭터 없음/남의 것)
	Log(ClientIdOf(session), line.str());

	if (pkt.success() == false)
		return true;	// 입장 실패: 연결은 유지, 테스트 동작은 시작하지 않음

	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	me->myId = pkt.player().object_id();
	me->myX = pkt.player().move_info().pos_info().x();
	me->myZ = pkt.player().move_info().pos_info().z();
	for (const Protocol::InventorySlot& slot : pkt.inventory())	// 내 인벤토리 (아이템이 있는 칸만 옴)
		ApplySlot(me, slot);

	if (GInputTest)
		SendInputTestBurst(session);
	if (GMoveTest)
		SendMoveInput(session, 100.f, 0.f, 1);	// 오른쪽. 길이 100 -> 서버가 1로 줄여야 정상 속도
	if (GBot)
		PickBotTarget(session);
	if (GChatTest)
		SendChatTestBurst(session);
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

// --flee-test 도망 속도 비율: 서버 이동 속도 5m/s x 0.8 = 4m/s (몬스터 추격 3.5m/s보다 조금만 빠름)
//   거리가 초당 0.5m씩만 벌어져 lost(12m)까지 약 20초 -> 그 전에 몬스터가 스폰에서 20m(leash)를 넘음
//   (전에는 5m/s로 도망쳐 약 7초 만에 lost -> 몬스터가 스폰 가까이서 쫓기 시작하면 lost가 먼저 났음)
static constexpr float FLEE_DIR_SCALE = 0.8f;

static void UpdateChaseTest(PacketSessionRef& session, uint64 serverTick, const Protocol::MoveEntry& entry)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	const Protocol::MoveInfo& move = entry.move_info();
	me->chaseTargetX = move.pos_info().x();
	me->chaseTargetZ = move.pos_info().z();

	const int32 prevState = me->lastTargetState;
	me->lastTargetState = move.state();

	// --ranged-test: 멈추고 쏘는 판단은 내 S_MOVE에서 (UpdateRangedTest). 여기서는 위치만 갱신하고, 걸어가는 중이면 다시 조준
	if (GRangedTest)
	{
		if (me->chaseStopSent == false)
			AimAtChaseTarget(session);
		return;
	}

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

			// --flee-test: 몬스터 반대 방향으로 4m/s로 계속 달림 -> 몬스터가 쫓다가 스폰에서 20m를 넘으면 포기하고 돌아감
			if (GFleeTest && dist > 0.01f)
				SendMoveInput(session, -dx / dist * FLEE_DIR_SCALE, -dz / dist * FLEE_DIR_SCALE, ++me->chaseSeq);

			// --attack-test: 몬스터가 바로 앞(1.5m)에 멈췄으니 검증용 공격 묶음
			if (GAttackTest)
				SendAttackTestBurst(session);
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

// 원거리 + 어그로 확인용 (Test/Scenarios/MonsterRanged.ps1, --ranged-test)
//   가장 가까운 몬스터 쪽으로 걷다가 10m에서 멈춤 -> 멈춤이 서버에 적용되면 원거리 스킬을 두 번 연달아 보냄
//     첫 번째: 통과 (몬스터는 감지 거리 8m 밖이라 아직 나를 모름 -> 맞고 쫓아와야 함)
//     두 번째: 쿨타임(5초)으로 거절 -> MP가 한 번만 깎여야 함
//   멈춤 판단을 내 S_MOVE에서 하는 이유: 서 있는(Idle) 몬스터는 S_MOVE가 안 와서, 몬스터 항목만 기다리면 계속 걸어감
static constexpr float	RANGED_TEST_STOP_DIST = 10.f;	// 같은 줄의 range_m (감지 거리 8m 밖)

static void UpdateRangedTest(PacketSessionRef& session, const Protocol::MoveEntry& entry)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	const float dx = me->chaseTargetX - me->myX;
	const float dz = me->chaseTargetZ - me->myZ;
	const float dist = std::sqrt(dx * dx + dz * dz);

	if (me->chaseStopSent == false)
	{
		if (dist <= RANGED_TEST_STOP_DIST)
		{
			SendMoveInput(session, 0.f, 0.f, ++me->chaseSeq);
			me->chaseStopSent = true;
		}
		return;
	}

	// 내 항목이 IDLE이고 ack가 멈춤 입력 번호 = 서버에서 멈춘 위치가 확정됨
	const bool stopped = (entry.move_info().state() == Protocol::MOVE_STATE_IDLE && entry.ack_seq() == me->chaseSeq);
	if (me->rangedShotSent == false && stopped)
	{
		SendSkill(session, RANGED_SKILL_ID, me->chaseTargetId);	// 통과
		SendSkill(session, RANGED_SKILL_ID, me->chaseTargetId);	// reject: cooldown
		me->rangedShotSent = true;

		ostringstream line;
		line << "RANGED_TEST shot target=" << me->chaseTargetId << " dist=" << dist;
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
			line << "(monster hp=" << object.stat_info().hp() << " lv=" << object.stat_info().level() << ")";	// stat_info가 실려 오는지 확인용
			me->monsterIds.insert(object.object_id());
		}
		else if (object.object_type() == Protocol::OBJECT_TYPE_DROP)	// 바닥 드랍: item_id 0 = 골드
		{
			if (object.drop_info().item_id() == 0)
				line << "(drop gold=" << object.drop_info().amount() << ")";
			else
				line << "(drop item=" << object.drop_info().item_id() << " x" << object.drop_info().amount() << ")";
		}
		if (object.has_cast_info())	// 시전 도중에 보이게 된 객체 (예고 게이지가 이미 일부 찬 상태)
			line << "(casting skill=" << object.cast_info().skill_id()
				 << " " << object.cast_info().remaining_ms() << "/" << object.cast_info().total_ms() << "ms)";
	}
	if (GBot == false)
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

	// --bot: 패킷마다 찍는 로그를 끔. 봇 수백 명 x 초당 20번이면 Log의 락 + flush가 서버보다 먼저 막힘
	if (GBot == false)
	{
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
	}

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

		if (GBot)
			continue;	// 내 위치만 갱신. 목표 확인은 아래 UpdateBot (내 항목이 안 와도 10초 재선택은 돌아야 해서 루프 밖)

		ostringstream myLine;
		myLine << "MY_MOVE tick=" << pkt.server_tick()
			<< " x=" << move.pos_info().x() << " z=" << move.pos_info().z() << " yaw=" << move.pos_info().yaw()
			<< " state=" << move.state() << " ack=" << entry.ack_seq();
		Log(me->GetClientId(), myLine.str());

		if (GMoveTest)
			UpdateMoveTest(session, pkt.server_tick(), entry);
		if (GRangedTest)
			UpdateRangedTest(session, entry);
	}

	if (GBot && me->myId != 0)
		UpdateBot(session);
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	ostringstream line;
	line << "S_DESPAWN ids:";
	for (uint64 objectId : pkt.object_ids())
		line << " " << objectId;
	if (GBot == false)
		Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_SKILL(PacketSessionRef& session, Protocol::S_SKILL& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	ostringstream line;
	line << "S_SKILL caster=" << pkt.object_id() << " skill=" << pkt.skill_id() << " target=" << pkt.target_id()
		<< " yaw=" << pkt.yaw() << " cast=" << pkt.cast_ms() << " range=" << pkt.range() << " angle=" << pkt.hit_angle();
	if (GBot == false)
		Log(me->GetClientId(), line.str());

	// 내 스킬이 확정됨 -> 클라 쪽 쿨타임 시작 (진짜 클라도 S_SKILL을 받은 시점부터 잼)
	if (pkt.object_id() == me->myId)
		me->lastSkillUs = NowUs();

	// 피하기 확인용 (Test/Scenarios/MonsterDodge.ps1, --dodge-test)
	//   내가 타겟인 예고 공격(cast > 0)을 받으면 바로 몬스터 반대 방향으로 달림 -> 판정 순간 부채꼴 밖이라 안 맞아야 함
	if (GDodgeTest && me->dodgeSent == false && pkt.cast_ms() > 0 && pkt.target_id() == me->myId)
	{
		SendMoveInput(session, me->myX - me->chaseTargetX, me->myZ - me->chaseTargetZ, ++me->chaseSeq);
		me->dodgeSent = true;

		ostringstream dodgeLine;
		dodgeLine << "DODGE caster=" << pkt.object_id() << " cast=" << pkt.cast_ms();
		Log(me->GetClientId(), dodgeLine.str());
	}
	return true;
}

bool Handle_S_CHANGE_HP(PacketSessionRef& session, Protocol::S_CHANGE_HP& pkt)
{
	ostringstream line;
	line << "S_CHANGE_HP id=" << pkt.object_id() << " hp=" << pkt.hp()
		<< " damage=" << pkt.damage() << " attacker=" << pkt.attacker_id();
	if (GBot == false)
		Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_DIE(PacketSessionRef& session, Protocol::S_DIE& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	ostringstream line;
	line << "S_DIE id=" << pkt.object_id() << " killer=" << pkt.killer_id() << " revive=" << pkt.revive_ms();
	if (GBot == false)
		Log(me->GetClientId(), line.str());

	// 몬스터: 서버에서 이미 사라진 id (진짜 클라는 죽는 모션 뒤에 액터를 지움)
	//   chaseTargetId는 0으로 되돌리지 않음: 0이면 리스폰 몬스터의 S_SPAWN을 받을 때 추적 테스트가 처음부터 다시 시작됨
	me->monsterIds.erase(pkt.object_id());

	// 플레이어 죽음 확인용 (Test/Scenarios/PlayerDeath.ps1, --death-test)
	//   내가 죽었으면 죽은 상태에서 이동 1개 + 기본 공격 1개를 보냄 -> 서버가 dead로 거절해야 함
	if (GDeathTest && pkt.object_id() == me->myId)
	{
		SendMoveInput(session, 1.f, 0.f, ++me->chaseSeq);
		SendSkill(session, BASIC_ATTACK_SKILL_ID, me->chaseTargetId);
		Log(me->GetClientId(), "DEAD_INPUT sent");
	}
	return true;
}

bool Handle_S_REVIVE(PacketSessionRef& session, Protocol::S_REVIVE& pkt)
{
	const Protocol::ObjectInfo& object = pkt.object();
	const Protocol::PosInfo& pos = object.move_info().pos_info();

	ostringstream line;
	line << "S_REVIVE id=" << object.object_id() << " x=" << pos.x() << " z=" << pos.z() << " hp=" << object.stat_info().hp();
	if (GBot == false)
		Log(ClientIdOf(session), line.str());

	// --bot: 내가 부활함 (시작 위치로 돌아옴) -> 거기서 새 목표를 골라 다시 걷기 시작
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	if (GBot && object.object_id() == me->myId)
	{
		me->myX = pos.x();
		me->myZ = pos.z();
		PickBotTarget(session);
	}
	return true;
}

bool Handle_S_CHANGE_MP(PacketSessionRef& session, Protocol::S_CHANGE_MP& pkt)
{
	ostringstream line;
	line << "S_CHANGE_MP mp=" << pkt.mp();
	Log(ClientIdOf(session), line.str());
	return true;
}

// 채팅: 보낸 사람 이름(영문은 from, 한글 비교용 from_hex)과 내용 (테스트 문구는 영문)
//   --chat-reply: 다른 사람의 주변 채팅을 처음 받으면 방 전체로 답장 (내 쿨타임은 비어 있어서 통과해야 함)
bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
{
	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);

	ostringstream line;
	line << "S_CHAT channel=" << pkt.channel() << " from=" << pkt.sender_name() << " from_hex=" << HexOf(pkt.sender_name())
		<< " id=" << pkt.object_id() << " text=" << pkt.message();
	Log(me->GetClientId(), line.str());

	if (GChatReply && me->chatReplied == false && pkt.channel() == Protocol::CHAT_CHANNEL_NEARBY && pkt.object_id() != me->myId)
	{
		me->chatReplied = true;
		SendChat(session, Protocol::CHAT_CHANNEL_ROOM, "reply");
	}
	return true;
}

// 경험치 (처치자 본인만): 테스트는 처치자 한 명에게만 오는지와 레벨업 후 넘친 양을 봄
bool Handle_S_CHANGE_EXP(PacketSessionRef& session, Protocol::S_CHANGE_EXP& pkt)
{
	ostringstream line;
	line << "S_CHANGE_EXP level=" << pkt.level() << " exp=" << pkt.exp() << " next=" << pkt.exp_to_next() << " gained=" << pkt.gained();
	Log(ClientIdOf(session), line.str());
	return true;
}

// 레벨업 (그 플레이어가 보이는 사람 모두, 본인 포함)
bool Handle_S_LEVEL_UP(PacketSessionRef& session, Protocol::S_LEVEL_UP& pkt)
{
	ostringstream line;
	line << "S_LEVEL_UP id=" << pkt.object_id() << " level=" << pkt.level();
	Log(ClientIdOf(session), line.str());
	return true;
}

// 줍기 (그 드랍을 보던 사람 모두): 테스트는 드랍마다 정확히 한 명만 주웠는지 봄
bool Handle_S_PICKUP(PacketSessionRef& session, Protocol::S_PICKUP& pkt)
{
	ostringstream line;
	line << "S_PICKUP drop=" << pkt.drop_id() << " picker=" << pkt.picker_id();
	Log(ClientIdOf(session), line.str());
	return true;
}

// 골드 / 아이템 (주운 사람 본인만)
bool Handle_S_CHANGE_GOLD(PacketSessionRef& session, Protocol::S_CHANGE_GOLD& pkt)
{
	ostringstream line;
	line << "S_CHANGE_GOLD gold=" << pkt.gold() << " gained=" << pkt.gained();
	Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_CHANGE_ITEM(PacketSessionRef& session, Protocol::S_CHANGE_ITEM& pkt)
{
	ostringstream line;
	line << "S_CHANGE_ITEM item=" << pkt.item_id() << " gained=" << pkt.gained() << " slots=";	// 바뀐 칸들 (칸:아이템x개수)
	for (int32 i = 0; i < pkt.slots_size(); i++)
		line << (i > 0 ? "," : "") << pkt.slots(i).slot() << ":" << pkt.slots(i).item_id() << "x" << pkt.slots(i).count();
	Log(ClientIdOf(session), line.str());

	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	for (const Protocol::InventorySlot& slot : pkt.slots())
		ApplySlot(me, slot);
	TryMoveItemTest(session);
	return true;
}

// 칸 옮기기 결과 (또는 거절했을 때 서버 값으로 되돌리기): 받은 칸을 그대로 덮어씀
bool Handle_S_INVENTORY_SLOTS(PacketSessionRef& session, Protocol::S_INVENTORY_SLOTS& pkt)
{
	ostringstream line;
	line << "S_INVENTORY_SLOTS slots=";
	for (int32 i = 0; i < pkt.slots_size(); i++)
		line << (i > 0 ? "," : "") << pkt.slots(i).slot() << ":" << pkt.slots(i).item_id() << "x" << pkt.slots(i).count();
	Log(ClientIdOf(session), line.str());

	shared_ptr<ServerSession> me = static_pointer_cast<ServerSession>(session);
	for (const Protocol::InventorySlot& slot : pkt.slots())
		ApplySlot(me, slot);
	return true;
}
