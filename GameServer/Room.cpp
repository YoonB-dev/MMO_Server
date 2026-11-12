#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"
#include "MoveSendPolicy.h"
#include "GameData.h"
#include "DBQueue.h"
#include <cmath>
#include <algorithm>
#include <chrono>

// Room: 게임 월드 하나 (전역 GRoom). JobQueue라서 이 객체의 함수는 한 번에 한 스레드만 실행함
//   -> _players, _monsters와 플레이어/몬스터 상태는 Room 안에서만 만지고 락이 없음
//   패킷 핸들러(IOCP 워커)는 직접 만지지 않고 DoAsync로 일을 넘김
//
// 틱 (50ms마다 DoTimer로 다음 틱 예약)
//   1초마다   ResetInputCounts, UpdatePlayerStats (자연 회복)
//   매 틱     UpdatePlayers (입력 적용 -> 이동) -> UpdateMonsters (AI) -> 바뀐 이동을 S_MOVE 하나로 전송
//   10초마다  LogStats (틱 간격, 처리 시간, 몬스터 전송 수, 스킬 거절/빗나감 수)
//   60초마다  SaveAllPlayers (전원 값을 복사해서 DB 줄로. 틱은 DB를 기다리지 않음)
//
// 입장 / 퇴장
//   DB 줄(LoadCharacter)이 캐릭터를 불러온 뒤 Enter -> S_ENTER_GAME, S_SPAWN
//   끊기면 Leave -> S_DESPAWN + 값을 복사해서 DB 줄(LeaveCharacter)에 저장 요청
//
// 전투 (C_SKILL -> HandleSkill)
//   검사 6개 (타겟 문제는 대상 필수 스킬이면 거절, 아니면 빗나감) -> 확정(쿨타임, MP) -> S_SKILL
//   -> 빗나감이면 끝, 맞았으면 ApplyDamage(S_CHANGE_HP)
//   몬스터 공격은 AI가 정함 (Monster::UpdateChase -> StartAttack: BroadcastSkill로 예고 -> UpdateAttack: 판정 -> HitPlayer)
//   플레이어 HP 0 -> KillPlayer (시체로 남음, S_DIE) -> 5초 뒤 RevivePlayer (시작 위치, S_REVIVE)
//   -> HP 0이면 KillMonster (S_DIE, 리스폰 예약), 아니면 Monster::OnDamaged (어그로)

// main에서 생성 (전역 변수 초기화 시점에는 ServerCore의 GMemory가 아직 없을 수 있음)
RoomRef GRoom;

void Room::SetMonsterSendPolicy(const MoveSendPolicy* policy)
{
	_monsterSendPolicy = policy;
	LogLine() << "[Room] monster send policy: " << policy->Name();
}

void Room::Start()
{
	SpawnMonsters();	// 서버가 열리기 전(main 스레드만 실행 중)이라 바로 호출해도 안전

	const uint64 now = ::GetTickCount64();
	_nextTickTime = now + TICK_INTERVAL_MS;
	_logStartTime = now;
	DoTimer(TICK_INTERVAL_MS, &Room::Tick);
}

// 스폰 위치와 종류는 DB(data_monster_spawn)에서 읽어 둔 것 (main의 LoadGameData)
void Room::SpawnMonsters()
{
	for (const MonsterSpawnData& spawn : GetMonsterSpawns())
		SpawnMonster(spawn);
}

// 스폰 줄 하나로 몬스터 한 마리 생성 + 이미 있는 사람들에게 S_SPAWN. 시작할 때와 리스폰(KillMonster의 DoTimer) 때 같이 씀
//   spawn을 값으로 받음: DoTimer로 넘길 때 넘기는 값과 매개변수 타입이 정확히 같아야 해서
void Room::SpawnMonster(MonsterSpawnData spawn)
{
	const MonsterData* data = FindMonsterData(spawn.monsterId);	// LoadGameData가 검사해 둬서 항상 있음
	MonsterRef monster = MakeShared<Monster>(*data, spawn);
	_monsters[monster->GetObjectId()] = monster;

	LogLine() << "[Room] spawn monster " << monster->GetObjectId() << " (type " << spawn.monsterId << ")"
		<< " at (" << spawn.x << ", " << spawn.z << ")";

	// 이미 방에 있는 사람들에게 (서버 시작 때는 아무도 없어서 패킷을 만들지 않음)
	if (_players.empty() == false)
	{
		Protocol::S_SPAWN pkt;
		*pkt.add_objects() = monster->GetObjectInfo();
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));
	}
}

void Room::Tick()
{
	_tick++;

	// 게임 처리에 걸린 시간 측정 (GetTickCount64는 약 15ms 단위라 짧은 시간은 steady_clock으로)
	const auto workStart = chrono::steady_clock::now();

	if (_tick % TICKS_PER_SECOND == 0)
	{
		ResetInputCounts();
		UpdatePlayerStats();
	}

	if (_tick % _saveIntervalTicks == 0)
		SaveAllPlayers();

	// 이번 틱에 바뀐 이동을 한 패킷에 모아서 전체(본인 포함)에 전송
	//   플레이어 먼저 -> 몬스터는 이번 틱에 움직인 플레이어 위치를 보고 판단
	Protocol::S_MOVE pkt;
	pkt.set_server_tick(_tick);
	UpdatePlayers(pkt);
	UpdateMonsters(pkt);

	if (pkt.moves_size() > 0)
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));

	const uint64 workUs = chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - workStart).count();
	_workTimeSumUs += workUs;
	if (workUs > _workTimeMaxUs)
		_workTimeMaxUs = workUs;

	const uint64 now = ::GetTickCount64();

	if (_tick % TICK_LOG_INTERVAL == 0)
		LogStats(now);

	// 목표 시각을 누적해서 다음 틱 예약 -> 한 번 늦게 실행돼도 다음 간격이 밀리지 않음
	_nextTickTime += TICK_INTERVAL_MS;
	if (now > _nextTickTime + MAX_TICK_DELAY_MS)
		_nextTickTime = now;	// 너무 밀렸으면(서버 멈춤 등) 몰아서 따라잡지 않고 기준을 새로 잡음

	const uint64 delay = (_nextTickTime > now) ? (_nextTickTime - now) : 0;
	DoTimer(delay, &Room::Tick);
}

// 200틱(10초) 동안의 통계 출력 후 다음 구간을 위해 0으로
//   avg interval: 틱 사이 간격 (50ms면 정상), work: 틱 안에서 게임 처리에 쓴 시간, monster moves: 보낸 몬스터 이동 수
void Room::LogStats(uint64 now)
{
	const double avgMs = static_cast<double>(now - _logStartTime) / TICK_LOG_INTERVAL;
	const double workAvgMs = _workTimeSumUs / 1000.0 / TICK_LOG_INTERVAL;
	const double workMaxMs = _workTimeMaxUs / 1000.0;

	LogLine() << "[Room] tick " << _tick << ", avg interval " << avgMs << "ms"
		<< ", work avg " << workAvgMs << "ms max " << workMaxMs << "ms"
		<< ", monster moves " << _monsterMovesSent << " (" << _monsterSendPolicy->Name() << ")";

	// 스킬 거절 / 빗나감: 이 구간에 하나라도 있었을 때만 출력
	//   거절은 정상 플레이에서 거의 0 (핵 의심용), 빗나감은 허공 공격이라 정상 플레이에서도 생김
	const SkillRejectStats& r = _skillRejects;
	const TargetFailStats& rt = r.target;
	if (r.dead + r.unknownSkill + r.cooldown + r.noMp + rt.noTarget + rt.badTarget + rt.invulnerable + rt.outOfRange > 0)
	{
		LogLine() << "[Room] skill rejects: dead " << r.dead << ", unknown-skill " << r.unknownSkill << ", cooldown " << r.cooldown
			<< ", no-mp " << r.noMp << ", no-target " << rt.noTarget << ", bad-target " << rt.badTarget
			<< ", invulnerable " << rt.invulnerable << ", out-of-range " << rt.outOfRange;
	}

	const TargetFailStats& m = _skillMisses;
	if (m.noTarget + m.badTarget + m.invulnerable + m.outOfRange > 0)
	{
		LogLine() << "[Room] skill misses: no-target " << m.noTarget << ", bad-target " << m.badTarget
			<< ", invulnerable " << m.invulnerable << ", out-of-range " << m.outOfRange;
	}

	_logStartTime = now;
	_workTimeSumUs = 0;
	_workTimeMaxUs = 0;
	_monsterMovesSent = 0;
	_skillRejects = {};
	_skillMisses = {};
}

void Room::Enter(PlayerRef player)
{
	// DB 큐가 SetPlayer한 뒤 바로 끊기면, Leave가 이 Enter보다 먼저 줄에 들어갈 수 있음
	//   그 경우 세션은 이미 연결 끊김 표시라 여기서 막음 (안 막으면 아무도 내보내지 않는 유령 플레이어가 남음)
	GameSessionRef session = player->GetSession();
	if (session == nullptr || session->IsConnected() == false)
		return;

	const uint64 myId = player->GetObjectId();
	_players[myId] = player;

	// 1. 나에게: 내 캐릭터 정보
	{
		Protocol::S_ENTER_GAME pkt;
		pkt.set_success(true);
		*pkt.mutable_player() = player->GetObjectInfo();
		pkt.set_mp(player->mp);				// MP는 본인만 보는 정보라 ObjectInfo 밖에 따로
		pkt.set_max_mp(Player::MAX_MP);
		SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
	}

	// 2. 나에게: 이미 방에 있던 객체들 (다른 플레이어 + 몬스터)
	{
		Protocol::S_SPAWN pkt;
		for (auto& [id, other] : _players)
		{
			if (id != myId)
				*pkt.add_objects() = other->GetObjectInfo();
		}

		for (auto& [id, monster] : _monsters)
			*pkt.add_objects() = monster->GetObjectInfo();

		if (pkt.objects_size() > 0)
			SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
	}

	// 3. 다른 사람들에게: 내가 들어왔다
	{
		Protocol::S_SPAWN pkt;
		*pkt.add_objects() = player->GetObjectInfo();
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt), myId);
	}
}

void Room::Leave(PlayerRef player)
{
	const uint64 id = player->GetObjectId();
	if (_players.erase(id) > 0)
	{
		Protocol::S_DESPAWN pkt;
		pkt.add_object_ids(id);
		Broadcast(ClientPacketHandler::MakeSendBuffer(pkt), id);
	}

	// 방에 없었어도(Enter보다 Leave가 먼저 온 경우) 저장 + 접속 목록 정리는 반드시
	//   안 보내면 DB 줄의 접속 목록에 계속 남아서 그 계정이 다시 못 들어옴
	//   값은 지금 복사해서 넘김 (DB 줄은 이 복사본만 봄 -> Player를 같이 만지지 않음)
	GDBQueue->DoAsync(&DBQueue::LeaveCharacter, player->ToCharacterData());
}

void Room::SetSaveInterval(uint64 seconds)
{
	_saveIntervalTicks = (seconds > 0 ? seconds : 1) * TICKS_PER_SECOND;	// 0이면 % 0이 돼서 최소 1초
}

// 주기 저장: 전원의 지금 값을 복사해서 한 번에 넘김 (Room 줄은 DB를 기다리지 않음)
//   Room은 한 줄이라 "주기 저장 복사 -> Leave 복사" 순서로 DB 줄에 들어감 -> 옛 값이 나중 값을 덮어쓰지 않음
void Room::SaveAllPlayers()
{
	if (_players.empty())
		return;

	xvector<CharacterData> rows;
	rows.reserve(_players.size());
	for (auto& [id, player] : _players)
		rows.push_back(player->ToCharacterData());

	GDBQueue->DoAsync(&DBQueue::SaveCharacters, rows);
}

void Room::HandleMoveInput(PlayerRef player, float dirX, float dirZ, uint32 buttons, uint32 seq)
{
	// 입력이 줄에서 기다리는 사이 이미 Leave가 처리된 플레이어
	if (_players.find(player->GetObjectId()) == _players.end())
		return;

	// 개수 제한: 검사보다 먼저 센다 (이상한 입력을 무한히 보내는 것도 막기 위해)
	if (++player->inputCount > MAX_INPUTS_PER_SECOND)
		return RejectInput(player, seq, "rate");

	// 죽은 동안은 못 움직임 (부활할 때 Revive가 정지 상태로 세움)
	if (player->IsDead())
		return RejectInput(player, seq, "dead");

	// 순서: 이미 받은 번호 이하면 옛날 입력이거나 중복
	if (seq <= player->lastInput.seq)
		return RejectInput(player, seq, "old seq");

	// 값: NaN이나 무한대면 이후 위치 계산이 전부 망가짐
	if (std::isfinite(dirX) == false || std::isfinite(dirZ) == false)
		return RejectInput(player, seq, "invalid dir");

	// 길이 정리: 1보다 길면 방향은 유지하고 길이만 1로 (속도 핵 방지), 거의 0이면 멈춤
	const float length = std::sqrt(dirX * dirX + dirZ * dirZ);
	if (length < MIN_DIR_LENGTH)
	{
		dirX = 0.f;
		dirZ = 0.f;
	}
	else if (length > 1.f)
	{
		dirX /= length;
		dirZ /= length;
	}

	// 같은 틱 안에 또 오면 덮어씀 -> 틱은 마지막 입력 하나만 적용
	player->lastInput = MoveInput{ dirX, dirZ, buttons, seq };
	player->hasNewInput = true;
}

// C_SKILL 처리: 클라가 고른 타겟을 믿지 않고 서버 데이터로 다시 검사
//   나 자신:  1 스킬 존재   2 쿨타임   3 MP
//   타겟:     4 방에 있는 몬스터   5 무적(복귀 중) 아님   6 서버 위치 거리 <= 사거리 + 여유  (CheckTarget)
//   1~3에 걸리면 거절: 이유별 횟수만 세고 끝 (클라에는 아무것도 안 보냄, 쿨타임/MP 안 씀)
//   4~6에 걸리면 대상 필수 스킬은 거절, 아니면 빗나감 (S_SKILL target 0, 쿨타임은 씀)
//   통과하면 쿨타임과 MP를 쓰고 S_SKILL -> 맞았으면 데미지
void Room::HandleSkill(PlayerRef player, int32 skillId, uint64 targetId)
{
	// 요청이 줄에서 기다리는 사이 이미 Leave가 처리된 플레이어
	if (_players.find(player->GetObjectId()) == _players.end())
		return;

	// 죽은 동안은 스킬을 못 씀
	if (player->IsDead())
	{
		_skillRejects.dead++;
		return;
	}

	// 1. 스킬 존재 (data_skill). 몬스터 스킬(101번부터)은 플레이어가 못 씀
	const SkillData* skill = FindSkillData(skillId);
	if (skill == nullptr || skillId >= MONSTER_SKILL_ID_BEGIN)
	{
		_skillRejects.unknownSkill++;
		return;
	}

	// 2. 쿨타임 (1틱 여유)
	//    readyTick - 1 대신 반대쪽에 더함: uint64라 0 - 1이 아주 큰 수가 됨
	//    []는 키가 없으면 0으로 칸을 만듦 -> 스킬 검사 뒤에 둬야 아무 id로나 칸이 생기지 않음
	uint64& readyTick = player->skillReadyTick[skillId];
	if (_tick + COOLDOWN_SLACK_TICKS < readyTick)
	{
		_skillRejects.cooldown++;
		return;
	}

	// 3. MP (비용 0인 스킬은 항상 통과)
	if (player->mp < skill->mpCost)
	{
		_skillRejects.noMp++;
		return;
	}

	// 4~6. 타겟. 문제가 있으면 target은 nullptr 그대로
	MonsterRef target;
	const TargetCheck check = CheckTarget(player, *skill, targetId, target);
	if (check != TargetCheck::Ok)
	{
		if (skill->requiresTarget)
		{
			CountTargetFail(_skillRejects.target, check);	// 대상 필수 -> 거절
			return;
		}
		CountTargetFail(_skillMisses, check);				// 대상 없어도 됨 -> 빗나감으로 계속
	}

	// ---- 여기부터 확정 ----
	// ms -> 틱은 올림: 설계값보다 짧아지지 않게 (500ms -> 10틱, 520ms -> 11틱)
	const uint64 cooldownTicks = (static_cast<uint64>(skill->cooldownMs) + TICK_INTERVAL_MS - 1) / TICK_INTERVAL_MS;
	readyTick = _tick + cooldownTicks;

	// MP는 모든 검사를 통과한 뒤에 깎음 -> 거절된 요청은 MP를 안 씀
	if (skill->mpCost > 0)
	{
		player->mp -= skill->mpCost;
		SendMp(player);
	}

	// 맞았고 멈춰 있을 때만 타겟 쪽으로 돌아봄 (이동 중엔 이동 방향 yaw를 유지하고, 클라가 상체만 돌림)
	//   빗나감이면 지금 yaw 그대로 -> 그 방향으로 허공을 휘두름
	Protocol::PosInfo* myPos = player->GetPosInfo();
	if (target != nullptr && player->GetMoveInfo()->state() == Protocol::MOVE_STATE_IDLE)
	{
		const Protocol::PosInfo* targetPos = target->GetPosInfo();
		myPos->set_yaw(std::atan2(targetPos->x() - myPos->x(), targetPos->z() - myPos->z()) * RAD_TO_DEG);
	}

	// 모두에게: 누가 무슨 스킬을 어느 방향으로 썼고, 누가 맞았나 (빗나감이면 target 0)
	BroadcastSkill(*player, *skill, target != nullptr ? target->GetObjectId() : 0);

	if (target == nullptr)
		return;		// 빗나감: 데미지 없음

	ApplyDamage(*target, player->GetObjectId(), skill->damage);

	// 몬스터만의 처리는 여기서 (ApplyDamage는 GameObject 공통)
	if (target->GetStatInfo()->hp() == 0)
		KillMonster(target, player->GetObjectId());
	else
		target->OnDamaged(player->GetObjectId());	// 어그로
}

// HandleSkill의 4~6번 타겟 검사. Ok면 outTarget에 몬스터를 채움 (아니면 건드리지 않음)
Room::TargetCheck Room::CheckTarget(const PlayerRef& player, const SkillData& skill, uint64 targetId, MonsterRef& outTarget)
{
	// 4. 방에 있는 몬스터 (0은 허공, 없는 id / 이미 죽은 몬스터 / 플레이어 id는 BadTarget)
	if (targetId == 0)
		return TargetCheck::NoTarget;

	auto it = _monsters.find(targetId);
	if (it == _monsters.end())
		return TargetCheck::BadTarget;

	const MonsterRef& monster = it->second;

	// 5. 복귀(Return) 중이면 무적: 끌고 가서 반격 없이 때리는 것 방지
	if (monster->IsInvulnerable())
		return TargetCheck::Invulnerable;

	// 6. 거리: 클라가 본 위치가 아니라 서버 위치로 잼 (+ 지연 여유). 제곱끼리 비교 -> sqrt 안 씀
	const Protocol::PosInfo* myPos = player->GetPosInfo();
	const Protocol::PosInfo* targetPos = monster->GetPosInfo();
	const float dx = targetPos->x() - myPos->x();
	const float dz = targetPos->z() - myPos->z();
	const float maxRange = skill.range + SKILL_RANGE_SLACK;
	if (dx * dx + dz * dz > maxRange * maxRange)
		return TargetCheck::OutOfRange;

	outTarget = monster;	// shared_ptr 복사: 죽어서 맵에서 빠져도 HandleSkill 끝까지 살아 있음
	return TargetCheck::Ok;
}

// 타겟 문제를 이유별로 셈 (거절, 빗나감 공통)
void Room::CountTargetFail(TargetFailStats& stats, TargetCheck result)
{
	switch (result)
	{
	case TargetCheck::NoTarget:		stats.noTarget++;		break;
	case TargetCheck::BadTarget:	stats.badTarget++;		break;
	case TargetCheck::Invulnerable:	stats.invulnerable++;	break;
	case TargetCheck::OutOfRange:	stats.outOfRange++;		break;
	case TargetCheck::Ok:			break;
	}
}

PlayerRef Room::FindPlayer(uint64 objectId)
{
	auto it = _players.find(objectId);
	return (it != _players.end()) ? it->second : nullptr;
}

// (x, z)에서 range 안에 있는 가장 가까운 살아 있는 플레이어. 없으면 nullptr
PlayerRef Room::FindNearestPlayer(float x, float z, float range)
{
	PlayerRef nearest = nullptr;
	float nearestDistSq = range * range;	// 이보다 가까워야 후보 -> 범위 밖은 자연히 빠짐

	for (auto& [id, player] : _players)
	{
		if (player->IsDead())
			continue;	// 시체는 감지하지 않음

		const Protocol::PosInfo* pos = player->GetPosInfo();
		const float dx = pos->x() - x;
		const float dz = pos->z() - z;
		const float distSq = dx * dx + dz * dz;
		if (distSq <= nearestDistSq)
		{
			nearest = player;
			nearestDistSq = distSq;
		}
	}
	return nearest;
}

void Room::ResetInputCounts()
{
	for (auto& [id, player] : _players)
		player->inputCount = 0;
}

// 1초마다 플레이어 스탯 갱신 (자연 회복). 계산은 Player::Regen, 누구에게 보낼지는 여기서
//   _players를 도는 중에 BroadcastHp도 _players를 돌지만 둘 다 읽기만 해서 안전 (넣거나 빼면 안 됨)
void Room::UpdatePlayerStats()
{
	for (auto& [id, player] : _players)
	{
		const Player::RegenResult result = player->Regen();
		if (result.hpChanged)
			BroadcastHp(*player, 0, 0);	// HP는 모두가 보는 정보 -> 전체
		if (result.mpChanged)
			SendMp(player);				// MP는 본인만
	}
}

// MP는 본인만 보는 정보 -> Broadcast가 아니라 SendTo
void Room::SendMp(const PlayerRef& player)
{
	Protocol::S_CHANGE_MP pkt;
	pkt.set_mp(player->mp);
	SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
}

void Room::UpdatePlayers(Protocol::S_MOVE& pkt)
{
	const bool resendTick = (_tick % MOVE_RESEND_TICKS == 0);

	for (auto& [id, player] : _players)
	{
		const bool changed = ApplyInput(player);
		MovePlayer(player);

		// 방향이 바뀌었으면 바로, 움직이는 중이면 4틱마다 보냄 (멈춰 있으면 안 보냄)
		const bool moving = (player->GetMoveInfo()->state() == Protocol::MOVE_STATE_MOVING);
		if (changed || (moving && resendTick))
		{
			Protocol::MoveEntry* entry = pkt.add_moves();
			entry->set_object_id(id);
			*entry->mutable_move_info() = *player->GetMoveInfo();
			entry->set_ack_seq(player->lastInput.seq);	// 받아들인 입력은 위 ApplyInput에서 전부 적용됐으므로 = 적용한 마지막 번호
		}
	}
}

void Room::UpdateMonsters(Protocol::S_MOVE& pkt)
{
	const bool resendTick = (_tick % MOVE_RESEND_TICKS == 0);

	for (auto& [id, monster] : _monsters)
	{
		monster->Update(*this, TICK_DT);

		// 보낼지는 전송 정책이 결정 (마지막으로 보낸 이동 vs 지금 이동)
		const Protocol::MoveInfo& now = *monster->GetMoveInfo();
		if (_monsterSendPolicy->ShouldSend(monster->lastSentMove, now, resendTick) == false)
			continue;

		Protocol::MoveEntry* entry = pkt.add_moves();
		entry->set_object_id(id);
		*entry->mutable_move_info() = now;
		// ack_seq는 0 그대로 (몬스터는 클라 입력이 없음)

		monster->lastSentMove = now;
		_monsterMovesSent++;
	}
}

// 우편함에 새 입력이 있으면 이동 방향/상태에 반영. 반영했으면 true
bool Room::ApplyInput(const PlayerRef& player)
{
	if (player->hasNewInput == false)
		return false;
	player->hasNewInput = false;	// 꺼냈으니 다음 틱엔 "새 입력 없음"

	const MoveInput& input = player->lastInput;
	Protocol::MoveInfo* moveInfo = player->GetMoveInfo();
	moveInfo->set_dir_x(input.dirX);
	moveInfo->set_dir_z(input.dirZ);

	// 거의 0인 방향은 HandleMoveInput에서 정확히 0으로 바꿔 두었으므로 == 비교로 충분
	const bool stop = (input.dirX == 0.f && input.dirZ == 0.f);
	moveInfo->set_state(stop ? Protocol::MOVE_STATE_IDLE : Protocol::MOVE_STATE_MOVING);
	moveInfo->set_speed(stop ? 0.f : MOVE_SPEED);

	// 바라보는 방향 (서버 좌표: +z = 0도, +x = 90도). 멈출 때는 마지막으로 보던 방향 유지
	if (stop == false)
		moveInfo->mutable_pos_info()->set_yaw(std::atan2(input.dirX, input.dirZ) * RAD_TO_DEG);

	return true;
}

void Room::MovePlayer(const PlayerRef& player)
{
	Protocol::MoveInfo* moveInfo = player->GetMoveInfo();
	if (moveInfo->state() != Protocol::MOVE_STATE_MOVING)
		return;

	const float step = MOVE_SPEED * TICK_DT;	// 한 틱에 0.25m
	Protocol::PosInfo* pos = moveInfo->mutable_pos_info();
	pos->set_x(std::clamp(pos->x() + moveInfo->dir_x() * step, -WORLD_HALF_SIZE, WORLD_HALF_SIZE));
	pos->set_z(std::clamp(pos->z() + moveInfo->dir_z() * step, -WORLD_HALF_SIZE, WORLD_HALF_SIZE));
	// y(높이)는 쓰지 않음 (점프 없음)
}

void Room::RejectInput(PlayerRef player, uint32 seq, const char* reason)
{
	LogLine() << "[Room] reject input: player " << player->GetObjectId()
		<< ", seq " << seq << ", reason " << reason;
}

// target의 HP를 깎고(0 아래로는 안 내려감) 모두에게 알림. 죽음 처리는 호출한 쪽에서 (HandleSkill / HitPlayer)
//   HP는 GameObject(ObjectInfo)에 있으므로 Monster / Player 구분 없이 GameObject로 받음
void Room::ApplyDamage(GameObject& target, uint64 attackerId, int32 damage)
{
	Protocol::StatInfo* stat = target.GetStatInfo();
	int32 hp = stat->hp() - damage;
	if (hp < 0)
		hp = 0;		// std::max는 Windows.h의 max 매크로와 부딪혀서 안 씀
	stat->set_hp(hp);

	BroadcastHp(target, damage, attackerId);
}

// S_SKILL을 모두에게. 플레이어 스킬(HandleSkill)과 몬스터 예고 공격(Monster::StartAttack) 공통
//   yaw는 시전자의 지금 방향. cast_ms, range, hit_angle은 클라가 예고 범위를 그리는 데 씀 (즉시 스킬은 cast 0)
void Room::BroadcastSkill(GameObject& caster, const SkillData& skill, uint64 targetId)
{
	Protocol::S_SKILL pkt;
	pkt.set_object_id(caster.GetObjectId());
	pkt.set_skill_id(skill.skillId);
	pkt.set_target_id(targetId);
	pkt.set_yaw(caster.GetPosInfo()->yaw());
	pkt.set_cast_ms(skill.castMs);
	pkt.set_range(skill.range);
	pkt.set_hit_angle(skill.hitAngleDeg);
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));
}

// target의 지금 HP를 모두에게. 데미지(ApplyDamage)와 회복(Monster가 스폰에 도착) 공통
void Room::BroadcastHp(GameObject& target, int32 damage, uint64 attackerId)
{
	Protocol::S_CHANGE_HP pkt;
	pkt.set_object_id(target.GetObjectId());
	pkt.set_hp(target.GetStatInfo()->hp());
	pkt.set_damage(damage);
	pkt.set_attacker_id(attackerId);
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));
}

// 몬스터 죽음: 방에서 빼고, 모두에게 알리고, 같은 스폰 줄로 리스폰 예약
//   HandleSkill(틱 밖의 job)에서만 부름 -> UpdateMonsters가 _monsters를 도는 도중에 지우는 일이 없음 (반복자 무효화 방지)
//   monster를 값(shared_ptr 복사)으로 받음: erase로 맵의 shared_ptr이 사라진 뒤에도 GetSpawn()을 써야 해서
void Room::KillMonster(MonsterRef monster, uint64 killerId)
{
	const uint64 monsterId = monster->GetObjectId();
	_monsters.erase(monsterId);	// 이제 공격 대상, 틱, 새로 들어온 사람의 S_SPAWN 어디에도 안 나옴

	Protocol::S_DIE pkt;
	pkt.set_object_id(monsterId);
	pkt.set_killer_id(killerId);
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));

	const MonsterSpawnData& spawn = monster->GetSpawn();
	LogLine() << "[Room] monster " << monsterId << " killed by " << killerId
		<< ", respawn in " << spawn.respawnMs << "ms (spawn " << spawn.spawnId << ")";

	// 같은 객체를 되살리지 않고 respawn_ms 뒤에 새 id로 만듦. spawn은 예약된 job 안에 복사돼서 monster가 지워져도 안전
	DoTimer(static_cast<uint64>(spawn.respawnMs), &Room::SpawnMonster, spawn);
}

// 몬스터가 플레이어를 맞힘 (Monster::UpdateAttack). 몬스터 쪽 HandleSkill처럼 "데미지 + 죽음 처리"를 한 곳에서
void Room::HitPlayer(const PlayerRef& target, uint64 attackerId, int32 damage)
{
	ApplyDamage(*target, attackerId, damage);
	if (target->IsDead())
		KillPlayer(target, attackerId);
}

// 플레이어 죽음: 방에는 남김 (연결 유지, 시체로 보임). 멈추고, 알리고, 부활 예약
//   _players를 지우지 않으므로 UpdateMonsters 도중(몬스터 공격 판정)에 불려도 반복자 문제가 없음
void Room::KillPlayer(const PlayerRef& player, uint64 killerId)
{
	// 이동 우편함에 정지를 넣음 -> 다음 틱 ApplyInput이 멈추고 S_MOVE로 알림 (이후 입력은 HandleMoveInput이 거절)
	player->lastInput.dirX = 0.f;
	player->lastInput.dirZ = 0.f;
	player->hasNewInput = true;

	Protocol::S_DIE pkt;
	pkt.set_object_id(player->GetObjectId());
	pkt.set_killer_id(killerId);
	pkt.set_revive_ms(static_cast<int32>(Player::REVIVE_DELAY_MS));
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));

	LogLine() << "[Room] player " << player->GetObjectId() << " killed by " << killerId
		<< ", revive in " << Player::REVIVE_DELAY_MS << "ms";

	// PlayerRef가 아니라 id로 예약: 5초 사이에 나가면 RevivePlayer가 못 찾고 끝남
	//   (PlayerRef를 넘기면 job이 shared_ptr 복사본을 들고 있어서, 나간 플레이어 객체가 5초 동안 안 지워짐)
	DoTimer(Player::REVIVE_DELAY_MS, &Room::RevivePlayer, player->GetObjectId());
}

// 부활: 시작 위치에서 HP 가득, MP 절반 -> 모두에게 S_REVIVE, 본인에게 MP
void Room::RevivePlayer(uint64 playerId)
{
	PlayerRef player = FindPlayer(playerId);
	if (player == nullptr || player->IsDead() == false)
		return;		// 그사이 나감

	player->Revive();

	Protocol::S_REVIVE pkt;
	*pkt.mutable_object() = player->GetObjectInfo();
	Broadcast(ClientPacketHandler::MakeSendBuffer(pkt));
	SendMp(player);

	LogLine() << "[Room] player " << playerId << " revived at (" << Player::SPAWN_X << ", " << Player::SPAWN_Z << ")";
}

void Room::Broadcast(SendBufferRef sendBuffer, uint64 exceptId)
{
	for (auto& [id, player] : _players)
	{
		if (id != exceptId)
			SendTo(player, sendBuffer);
	}
}

void Room::SendTo(PlayerRef player, SendBufferRef sendBuffer)
{
	if (GameSessionRef session = player->GetSession())
		session->Send(sendBuffer);
}
