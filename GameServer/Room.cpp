#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "GameSession.h"
#include "ClientPacketHandler.h"
#include "MoveSendPolicy.h"
#include "GameData.h"
#include "DBQueue.h"
#include "Drop.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <random>

// Room: 게임 월드 하나 (전역 GRoom). JobQueue라서 이 객체의 함수는 한 번에 한 스레드만 실행함
//   -> _players, _monsters와 플레이어/몬스터 상태는 Room 안에서만 만지고 락이 없음
//   패킷 핸들러(IOCP 워커)는 직접 만지지 않고 DoAsync로 일을 넘김
//
// 틱 (50ms마다 DoTimer로 다음 틱 예약)
//   1초마다   ResetInputCounts, UpdatePlayerStats (자연 회복)
//   매 틱     UpdatePlayers (입력 적용 -> 이동) -> UpdateMonsters (AI) -> UpdateViews (플레이어마다 시야 안의 것만 S_SPAWN / S_DESPAWN / S_MOVE)
//   10초마다  LogStats (틱 간격, 처리 시간, 몬스터 전송 수, 스킬 거절/빗나감 수)
//   60초마다  SaveAllPlayers (전원 값을 복사해서 DB 줄로. 틱은 DB를 기다리지 않음)
//
// 입장 / 퇴장
//   DB 줄(LoadCharacter)이 캐릭터를 불러온 뒤 Enter -> S_ENTER_GAME, 시야 안의 객체를 S_SPAWN (나에게 주변, 주변에게 나)
//   끊기면 Leave -> 나를 보던 사람에게 S_DESPAWN + 값을 복사해서 DB 줄(LeaveCharacter)에 저장 요청
//
// 시야 (AOI)
//   플레이어마다 "지금 보이는 객체 목록"(Player::visible)을 들고, 매 틱 Grid로 주변 후보만 찾아서 거리로 판정
//   30m 안에 들어오면 S_SPAWN, 35m 밖으로 나가면 S_DESPAWN, 계속 보이는 것 중 움직인 것만 S_MOVE
//   이벤트(S_SKILL, S_CHANGE_HP, S_DIE, S_REVIVE)는 그 객체가 보이는 사람에게만 (BroadcastToViewers)
//
// 채팅 (C_CHAT -> HandleChat)
//   검사(범위 값, 빈 문자열, 100글자, 제어 문자) -> 0.5초 쿨타임 -> S_CHAT(보낸 사람 이름 포함)
//   주변은 BroadcastToViewers (나를 보는 사람 + 나), 방 전체는 _players 전원
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

	SpawnLoadMonsters();
}

// 부하 테스트용 몬스터 (--load-monsters N --load-area M): ±M m 안의 무작위 위치에 N마리
//   시드 고정 -> 실행할 때마다 배치가 똑같음 (AOI 적용 전/후를 같은 조건으로 비교하려고)
//   스폰 줄은 기획 데이터와 안 겹치게 번호를 크게 잡고, 리스폰도 일반 몬스터처럼 이 줄로 함 (Monster가 복사해 둠)
void Room::SpawnLoadMonsters()
{
	if (_loadMonsterCount == 0)
		return;

	if (FindMonsterData(LOAD_MONSTER_TYPE) == nullptr)
	{
		LogLine() << "[Room] load monsters skipped: monster type " << LOAD_MONSTER_TYPE << " not in data_monster";
		return;
	}

	mt19937 rng(LOAD_MONSTER_SEED);
	uniform_real_distribution<float> pos(-_loadMonsterArea, _loadMonsterArea);

	for (int32 i = 0; i < _loadMonsterCount; i++)
	{
		MonsterSpawnData spawn;
		spawn.spawnId = LOAD_SPAWN_ID_BASE + i;
		spawn.monsterId = LOAD_MONSTER_TYPE;
		spawn.x = pos(rng);
		spawn.z = pos(rng);
		spawn.respawnMs = LOAD_RESPAWN_MS;
		SpawnMonster(spawn);
	}

	LogLine() << "[Room] load monsters: " << _loadMonsterCount << " in +-" << _loadMonsterArea << "m (seed " << LOAD_MONSTER_SEED << ")";
}

// 스폰 줄 하나로 몬스터 한 마리 생성. 시작할 때와 리스폰(KillMonster의 DoTimer) 때 같이 씀
//   S_SPAWN은 여기서 보내지 않음: 다음 틱 UpdateView가 주변 사람에게만 보냄 (최대 50ms 뒤)
//   spawn을 값으로 받음: DoTimer로 넘길 때 넘기는 값과 매개변수 타입이 정확히 같아야 해서
void Room::SpawnMonster(MonsterSpawnData spawn)
{
	const MonsterData* data = FindMonsterData(spawn.monsterId);	// LoadGameData가 검사해 둬서 항상 있음
	MonsterRef monster = MakeShared<Monster>(*data, spawn);
	_monsters[monster->GetObjectId()] = monster;
	_grid.Add(monster);

	LogLine() << "[Room] spawn monster " << monster->GetObjectId() << " (type " << spawn.monsterId << ")"
		<< " at (" << spawn.x << ", " << spawn.z << ")";
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
		ExpireDrops();
	}

	if (_tick % _saveIntervalTicks == 0)
		SaveAllPlayers();

	// 이번 틱에 바뀐 이동을 전체 목록(pkt)에 모은 뒤, 플레이어마다 시야 안의 것만 골라서 보냄
	//   플레이어 먼저 -> 몬스터는 이번 틱에 움직인 플레이어 위치를 보고 판단
	//   pkt는 그대로 보내지 않음 (UpdateView가 항목을 골라 복사하는 원본)
	Protocol::S_MOVE pkt;
	pkt.set_server_tick(_tick);
	UpdatePlayers(pkt);
	UpdateMonsters(pkt);
	UpdateViews(pkt);

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
//   players / monsters: 지금 방 인원, sent: 10초 동안 보낸 패킷 수와 바이트 (부하 테스트 Test/Load/Run-Load.ps1이 이 줄을 읽음)
void Room::LogStats(uint64 now)
{
	const double avgMs = static_cast<double>(now - _logStartTime) / TICK_LOG_INTERVAL;
	const double workAvgMs = _workTimeSumUs / 1000.0 / TICK_LOG_INTERVAL;
	const double workMaxMs = _workTimeMaxUs / 1000.0;

	LogLine() << "[Room] tick " << _tick << ", avg interval " << avgMs << "ms"
		<< ", work avg " << workAvgMs << "ms max " << workMaxMs << "ms"
		<< ", monster moves " << _monsterMovesSent << " (" << _monsterSendPolicy->Name() << ")"
		<< ", players " << _players.size() << ", monsters " << _monsters.size()
		<< ", sent " << _sentPackets << " packets " << (_sentBytes / 1024) << " KB";

	// 시야: 한 명이 지금 보고 있는 객체 수 (AOI 효과를 숫자로 보려고. 지금 순간의 값)
	if (_players.empty() == false)
	{
		uint64 visibleSum = 0;
		for (auto& [id, player] : _players)
			visibleSum += player->visible.size();
		LogLine() << "[Room] view: avg " << (static_cast<double>(visibleSum) / _players.size()) << " objects per player"
			<< " (enter " << VIEW_ENTER_RADIUS << "m, leave " << VIEW_LEAVE_RADIUS << "m), drops on ground " << _drops.size();
	}

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
	_sentPackets = 0;
	_sentBytes = 0;
	_skillRejects = {};
	_skillMisses = {};
}

// (x, z)까지의 거리 제곱. 반경도 제곱해서 비교 -> sqrt를 안 씀
static float DistSq(GameObject& object, float x, float z)
{
	const Protocol::PosInfo* pos = object.GetPosInfo();
	const float dx = pos->x() - x;
	const float dz = pos->z() - z;
	return dx * dx + dz * dz;
}

// object가 보이는 플레이어마다 func(const PlayerRef&) 호출. object가 플레이어면 본인도 포함
//   "보인다" = 그 플레이어의 visible에 object가 있음 -> S_MOVE를 받는 사람과 이벤트를 받는 사람이 정확히 같음
//   보이는 사람은 object에서 나가는 반경 안에 있으므로 Grid로 그 주변 칸만 찾아봄
//   (+ 여유: 틱 도중 몬스터 공격 같은 이벤트는 위치가 마지막 시야 계산 뒤로 한 틱만큼 움직인 상태일 수 있음)
template<typename Func>
void Room::ForEachViewer(GameObject& object, Func&& func)
{
	const uint64 objectId = object.GetObjectId();
	const Protocol::PosInfo* pos = object.GetPosInfo();
	_grid.ForEachPlayer(pos->x(), pos->z(), VIEW_LEAVE_RADIUS + VIEW_EVENT_SLACK, [&](const PlayerRef& player)
		{
			if (player->GetObjectId() == objectId || player->visible.count(objectId) > 0)
				func(player);
		});
}

void Room::BroadcastToViewers(GameObject& object, SendBufferRef sendBuffer)
{
	ForEachViewer(object, [&](const PlayerRef& player)
		{
			SendTo(player, sendBuffer);
		});
}

// 입장 즉시 시야 채우기 (다음 틱까지 기다리지 않음)
//   나에게: 들어오는 반경 안의 플레이어 + 몬스터를 S_SPAWN 한 번에
//   주변 플레이어에게: 나를 S_SPAWN (같은 거리, 같은 반경이라 "내가 보는 사람" = "나를 보는 사람")
void Room::EnterView(const PlayerRef& player)
{
	const uint64 myId = player->GetObjectId();
	const float x = player->GetPosInfo()->x();
	const float z = player->GetPosInfo()->z();
	const float enterSq = VIEW_ENTER_RADIUS * VIEW_ENTER_RADIUS;

	Protocol::S_SPAWN aroundPkt;	// 나에게 보낼 주변 객체들
	SendBufferRef mePkt;			// 주변에게 보낼 "나" (처음 필요할 때 한 번만 만들고 같은 버퍼를 여러 명에게)

	_grid.ForEachPlayer(x, z, VIEW_ENTER_RADIUS, [&](const PlayerRef& other)
		{
			if (other == player || DistSq(*other, x, z) > enterSq)
				return;

			player->visible.emplace(other->GetObjectId(), _tick);
			*aroundPkt.add_objects() = other->GetObjectInfo();

			if (mePkt == nullptr)
			{
				Protocol::S_SPAWN pkt;
				*pkt.add_objects() = player->GetObjectInfo();
				mePkt = ClientPacketHandler::MakeSendBuffer(pkt);
			}
			other->visible.emplace(myId, _tick);
			SendTo(other, mePkt);
		});

	_grid.ForEachMonster(x, z, VIEW_ENTER_RADIUS, [&](const MonsterRef& monster)
		{
			if (DistSq(*monster, x, z) > enterSq)
				return;

			player->visible.emplace(monster->GetObjectId(), _tick);
			*aroundPkt.add_objects() = monster->GetObjectInfo();
		});

	_grid.ForEachDrop(x, z, VIEW_ENTER_RADIUS, [&](const DropRef& drop)
		{
			if (DistSq(*drop, x, z) > enterSq)
				return;

			player->visible.emplace(drop->GetObjectId(), _tick);
			*aroundPkt.add_objects() = drop->GetObjectInfo();
		});

	if (aroundPkt.objects_size() > 0)
		SendSpawns(player, aroundPkt);
}

void Room::UpdateViews(const Protocol::S_MOVE& tickMoves)
{
	for (auto& [id, player] : _players)
		UpdateView(player, tickMoves);
}

// 한 플레이어의 시야를 다시 계산해서 바뀐 것만 보냄
//   1. Grid에서 나가는 반경(35m)에 걸친 칸의 객체만 후보로
//   2. 후보마다 거리 판정
//        이미 보이는 것: 35m 안이면 "이번 틱 확인" 표시 (+ 이번 틱에 움직였으면 S_MOVE에 담음)
//        안 보이던 것:   30m 안이면 새로 보임 -> S_SPAWN에 담음
//        (30 ~ 35m에서는 지금 상태 그대로 -> 경계에서 왔다갔다해도 깜빡이지 않음)
//   3. visible에서 이번 틱 확인 표시가 없는 것 = 안 보이게 됨 -> S_DESPAWN에 담고 지움
//   보내는 순서: S_DESPAWN -> S_SPAWN -> S_MOVE (새로 보인 것은 S_SPAWN에 지금 위치가 있어서 S_MOVE에 안 넣음)
void Room::UpdateView(const PlayerRef& player, const Protocol::S_MOVE& tickMoves)
{
	const uint64 myId = player->GetObjectId();
	const float x = player->GetPosInfo()->x();
	const float z = player->GetPosInfo()->z();
	const float enterSq = VIEW_ENTER_RADIUS * VIEW_ENTER_RADIUS;
	const float leaveSq = VIEW_LEAVE_RADIUS * VIEW_LEAVE_RADIUS;

	Protocol::S_SPAWN spawnPkt;
	Protocol::S_MOVE movePkt;
	movePkt.set_server_tick(_tick);

	// 내 이동은 항상 (ack_seq로 내 입력이 적용된 걸 확인해야 함)
	if (player->moveTick == _tick)
		*movePkt.add_moves() = tickMoves.moves(player->moveIndex);

	// 후보 하나 판정 (플레이어, 몬스터 공통이라 GameObject로 받음)
	auto check = [&](GameObject& other)
		{
			const uint64 otherId = other.GetObjectId();
			if (otherId == myId)
				return;

			const float distSq = DistSq(other, x, z);
			auto it = player->visible.find(otherId);
			if (it != player->visible.end())
			{
				if (distSq > leaveSq)
					return;		// 표시 안 함 -> 아래 3번에서 S_DESPAWN

				it->second = _tick;
				if (other.moveTick == _tick)
					*movePkt.add_moves() = tickMoves.moves(other.moveIndex);
			}
			else if (distSq <= enterSq)
			{
				player->visible.emplace(otherId, _tick);
				*spawnPkt.add_objects() = other.GetObjectInfo();
			}
		};

	_grid.ForEachPlayer(x, z, VIEW_LEAVE_RADIUS, [&](const PlayerRef& other) { check(*other); });
	_grid.ForEachMonster(x, z, VIEW_LEAVE_RADIUS, [&](const MonsterRef& monster) { check(*monster); });
	_grid.ForEachDrop(x, z, VIEW_LEAVE_RADIUS, [&](const DropRef& drop) { check(*drop); });	// 안 움직여서 S_MOVE에는 안 들어감

	// 3. 이번 틱에 확인 못 한 것 = 멀어졌거나, 다른 칸으로 멀리 갔거나(부활 순간이동)
	//    erase(it)는 지운 다음 원소를 돌려줌 -> 지우면서 도는 동안 it를 그걸로 바꿔야 함 (지운 반복자는 못 씀)
	Protocol::S_DESPAWN despawnPkt;
	for (auto it = player->visible.begin(); it != player->visible.end();)
	{
		if (it->second != _tick)
		{
			despawnPkt.add_object_ids(it->first);
			it = player->visible.erase(it);
		}
		else
		{
			++it;
		}
	}

	if (despawnPkt.object_ids_size() > 0)
		SendTo(player, ClientPacketHandler::MakeSendBuffer(despawnPkt));
	if (spawnPkt.objects_size() > 0)
		SendSpawns(player, spawnPkt);
	if (movePkt.moves_size() > 0)
		SendMoves(player, movePkt);
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
	_grid.Add(player);

	// 1. 나에게: 내 캐릭터 정보
	{
		Protocol::S_ENTER_GAME pkt;
		pkt.set_success(true);
		*pkt.mutable_player() = player->GetObjectInfo();
		pkt.set_mp(player->mp);				// MP는 본인만 보는 정보라 ObjectInfo 밖에 따로
		pkt.set_max_mp(Player::MAX_MP);
		pkt.set_exp(player->exp);			// 경험치도 본인만 (레벨은 player.stat_info에 이미 있음)
		pkt.set_exp_to_next(GetExpToNext(player->GetLevel()));
		SendTo(player, ClientPacketHandler::MakeSendBuffer(pkt));
	}

	// 2. 시야: 주변을 나에게, 나를 주변에게 (방 전체가 아니라 들어오는 반경 안만)
	EnterView(player);
}

void Room::Leave(PlayerRef player)
{
	const uint64 id = player->GetObjectId();
	if (_players.erase(id) > 0)
	{
		// 나를 보던 사람에게만 S_DESPAWN + 그 사람의 시야 목록에서 나를 지움
		Protocol::S_DESPAWN pkt;
		pkt.add_object_ids(id);
		SendBufferRef sendBuffer = ClientPacketHandler::MakeSendBuffer(pkt);
		ForEachViewer(*player, [&](const PlayerRef& viewer)
			{
				if (viewer == player)
					return;		// 나가는 본인은 제외
				viewer->visible.erase(id);
				SendTo(viewer, sendBuffer);
			});

		_grid.Remove(player);	// ForEachViewer가 끝난 뒤에 (도는 중에 칸 목록을 바꾸면 안 됨)
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

void Room::SetDropLifetime(uint64 seconds)
{
	_dropLifetimeTicks = (seconds > 0 ? seconds : 1) * TICKS_PER_SECOND;
}

void Room::SetLoadMonsters(int32 count, float area)
{
	_loadMonsterCount = (std::max)(count, 0);
	_loadMonsterArea = (std::min)((std::max)(area, 0.f), WORLD_HALF_SIZE);	// 월드 밖에 놓으면 움직이자마자 벽에 걸림
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
//   방 전체가 아니라 Grid에서 range에 걸친 칸의 플레이어만 봄 (몬스터 감지 8m -> 보통 1~4칸)
PlayerRef Room::FindNearestPlayer(float x, float z, float range)
{
	PlayerRef nearest = nullptr;
	float nearestDistSq = range * range;	// 이보다 가까워야 후보 -> 범위 밖은 자연히 빠짐

	_grid.ForEachPlayer(x, z, range, [&](const PlayerRef& player)
		{
			if (player->IsDead())
				return;		// 시체는 감지하지 않음

			const float distSq = DistSq(*player, x, z);
			if (distSq <= nearestDistSq)
			{
				nearest = player;
				nearestDistSq = distSq;
			}
		});
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
		_grid.Update(player);	// 칸이 바뀌었으면 옮김 -> 이어서 도는 몬스터 감지가 새 위치로 찾음

		// 방향이 바뀌었으면 바로, 움직이는 중이면 4틱마다 보냄 (멈춰 있으면 안 보냄)
		const bool moving = (player->GetMoveInfo()->state() == Protocol::MOVE_STATE_MOVING);
		if (changed || (moving && resendTick))
		{
			player->moveTick = _tick;				// UpdateView가 "이번 틱에 움직였나"를 이걸로 봄
			player->moveIndex = pkt.moves_size();	// 지금 추가하는 항목의 번호
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
		_grid.Update(monster);

		// 보낼지는 전송 정책이 결정 (마지막으로 보낸 이동 vs 지금 이동)
		const Protocol::MoveInfo& now = *monster->GetMoveInfo();
		if (_monsterSendPolicy->ShouldSend(monster->lastSentMove, now, resendTick) == false)
			continue;

		monster->moveTick = _tick;
		monster->moveIndex = pkt.moves_size();
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

// 채팅 내용 검사. 문제가 있으면 거절 이유, 없으면 nullptr
//   UTF-8 형식 자체는 protobuf가 파싱할 때 검사함 (proto3 string은 잘못된 UTF-8이면 파싱 실패)
static const char* CheckChatText(const string& text, int32 maxChars)
{
	int32 chars = 0;
	bool onlySpaces = true;
	for (const unsigned char c : text)
	{
		if (c < 0x20 || c == 0x7F)
			return "control_char";		// 줄바꿈 등: 다른 줄인 척하는 가짜 시스템 메시지 방지
		if (c != ' ')
			onlySpaces = false;
		if ((c & 0xC0) != 0x80)			// UTF-8에서 10xxxxxx는 앞 글자의 이어지는 바이트 -> 글자 시작만 셈
			chars++;
	}
	if (onlySpaces)
		return "empty";					// 빈 문자열, 공백만
	if (chars > maxChars)
		return "too_long";
	return nullptr;
}

// C_CHAT: 검사 -> 쿨타임 -> 범위 안 전원에게 S_CHAT (보낸 사람 포함)
//   내용 검사에서 거절되면 쿨타임을 안 씀 (오타 때문에 0.5초 묶이지 않게)
void Room::HandleChat(PlayerRef player, Protocol::ChatChannel channel, string message)
{
	if (_players.count(player->GetObjectId()) == 0)
		return;		// 줄에 들어오기 전에 나감

	const char* reason = nullptr;
	if (Protocol::ChatChannel_IsValid(channel) == false)
		reason = "bad_channel";			// proto3 enum은 모르는 숫자도 그대로 들어옴
	else
		reason = CheckChatText(message, CHAT_MAX_CHARS);
	if (reason == nullptr && _tick < player->chatReadyTick)
		reason = "cooldown";

	if (reason != nullptr)
	{
		LogLine() << "[Room] reject chat: player " << player->GetObjectId() << ", reason " << reason;
		return;
	}
	player->chatReadyTick = _tick + CHAT_COOLDOWN_TICKS;

	// 패킷은 한 번만 만들고 같은 버퍼를 여러 명에게 (내용, 이름은 로그에 안 남김: 개인정보 + 콘솔 한글 깨짐)
	Protocol::S_CHAT pkt;
	pkt.set_object_id(player->GetObjectId());
	pkt.set_channel(channel);
	pkt.set_message(std::move(message));
	pkt.set_sender_name(player->GetObjectInfo().name());
	SendBufferRef sendBuffer = ClientPacketHandler::MakeSendBuffer(pkt);

	if (channel == Protocol::CHAT_CHANNEL_NEARBY)
	{
		BroadcastToViewers(*player, sendBuffer);	// 나를 visible에 가진 사람 + 나 (AOI 그대로)
	}
	else
	{
		for (auto& [id, other] : _players)
			SendTo(other, sendBuffer);
	}
}

// S_SKILL을 시전자가 보이는 사람에게. 플레이어 스킬(HandleSkill)과 몬스터 예고 공격(Monster::StartAttack) 공통
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
	BroadcastToViewers(caster, ClientPacketHandler::MakeSendBuffer(pkt));
}

// target의 지금 HP를 target이 보이는 사람에게. 데미지(ApplyDamage)와 회복(Monster가 스폰에 도착) 공통
void Room::BroadcastHp(GameObject& target, int32 damage, uint64 attackerId)
{
	Protocol::S_CHANGE_HP pkt;
	pkt.set_object_id(target.GetObjectId());
	pkt.set_hp(target.GetStatInfo()->hp());
	pkt.set_damage(damage);
	pkt.set_attacker_id(attackerId);
	BroadcastToViewers(target, ClientPacketHandler::MakeSendBuffer(pkt));
}

// 몬스터 죽음: 보던 사람에게 알리고, 방에서 빼고, 같은 스폰 줄로 리스폰 예약
//   HandleSkill(틱 밖의 job)에서만 부름 -> UpdateMonsters가 _monsters를 도는 도중에 지우는 일이 없음 (반복자 무효화 방지)
//   monster를 값(shared_ptr 복사)으로 받음: erase로 맵의 shared_ptr이 사라진 뒤에도 GetSpawn()을 써야 해서
void Room::KillMonster(MonsterRef monster, uint64 killerId)
{
	const uint64 monsterId = monster->GetObjectId();

	// 보던 사람에게 S_DIE + 시야 목록에서 지움 (클라는 S_DIE로 몬스터를 치움 -> S_DESPAWN은 따로 안 보냄)
	Protocol::S_DIE pkt;
	pkt.set_object_id(monsterId);
	pkt.set_killer_id(killerId);
	SendBufferRef sendBuffer = ClientPacketHandler::MakeSendBuffer(pkt);
	ForEachViewer(*monster, [&](const PlayerRef& viewer)
		{
			viewer->visible.erase(monsterId);
			SendTo(viewer, sendBuffer);
		});

	_grid.Remove(monster);
	_monsters.erase(monsterId);	// 이제 공격 대상, 틱, 시야 어디에도 안 나옴

	// 처치 보상: 경험치는 마지막 공격자에게, 골드 / 아이템은 바닥에 (먼저 줍는 사람이 가져감)
	GiveExp(killerId, monster->GetData().expReward);
	SpawnDrops(*monster);

	const MonsterSpawnData& spawn = monster->GetSpawn();
	LogLine() << "[Room] monster " << monsterId << " killed by " << killerId
		<< ", respawn in " << spawn.respawnMs << "ms (spawn " << spawn.spawnId << ")";

	// 같은 객체를 되살리지 않고 respawn_ms 뒤에 새 id로 만듦. spawn은 예약된 job 안에 복사돼서 monster가 지워져도 안전
	DoTimer(static_cast<uint64>(spawn.respawnMs), &Room::SpawnMonster, spawn);
}

// 처치 경험치: 마지막 공격자에게. 경험치는 처치자에게만, 레벨업은 처치자가 보이는 사람에게
void Room::GiveExp(uint64 killerId, int32 amount)
{
	PlayerRef killer = FindPlayer(killerId);
	if (killer == nullptr || amount <= 0)
		return;

	const int32 levelsUp = killer->GainExp(amount);

	Protocol::S_CHANGE_EXP expPkt;
	expPkt.set_level(killer->GetLevel());
	expPkt.set_exp(killer->exp);
	expPkt.set_exp_to_next(GetExpToNext(killer->GetLevel()));
	expPkt.set_gained(amount);
	SendTo(killer, ClientPacketHandler::MakeSendBuffer(expPkt));

	if (levelsUp > 0)
	{
		LogLine() << "[Room] level up: player " << killerId << " -> " << killer->GetLevel();
		Protocol::S_LEVEL_UP upPkt;
		upPkt.set_object_id(killerId);
		upPkt.set_level(killer->GetLevel());
		BroadcastToViewers(*killer, ClientPacketHandler::MakeSendBuffer(upPkt));	// 처치자 본인 포함
	}
}

// 몬스터가 죽은 자리에 골드 한 더미 + 드랍표 줄마다 확률을 굴린 아이템
//   보이는 건 다음 틱 UpdateView가 처리 (SpawnMonster와 같은 방식)
void Room::SpawnDrops(Monster& monster)
{
	const MonsterData& data = monster.GetData();
	const float x = monster.GetPosInfo()->x();
	const float z = monster.GetPosInfo()->z();
	int32 count = 0;
	int32 gold = 0;

	if (data.goldMax > 0)
	{
		gold = std::uniform_int_distribution<int32>(data.goldMin, data.goldMax)(_rng);
		if (gold > 0)
		{
			AddDrop(0, gold, x, z);
			count++;
		}
	}
	for (const MonsterDropData& drop : data.drops)
	{
		if (std::uniform_int_distribution<int32>(1, 1000)(_rng) > drop.chancePermille)
			continue;	// 1~1000 중 chance 이하가 나오면 떨어짐 (500 = 50%)
		const int32 amount = std::uniform_int_distribution<int32>(drop.minCount, drop.maxCount)(_rng);
		AddDrop(drop.itemId, amount, x, z);
		count++;
	}

	if (count > 0)
		LogLine() << "[Room] monster " << monster.GetObjectId() << " dropped " << count << " (gold " << gold << ")";
}

// 하나 만들기: 상한이면 가장 오래된 것부터 지움 -> 자리를 흩뿌림 -> 목록과 Grid에 추가
void Room::AddDrop(int32 itemId, int32 amount, float x, float z)
{
	if (static_cast<int32>(_drops.size()) >= MAX_DROPS)
		RemoveDrop(_drops.begin()->second);

	// 원 안에 고르게: 각도는 그대로, 반지름은 sqrt를 씌움 (안 씌우면 가운데로 몰림)
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	const float angle = unit(_rng) * 6.2831853f;
	const float radius = std::sqrt(unit(_rng)) * DROP_SCATTER_RADIUS;

	DropRef drop = MakeShared<Drop>(itemId, amount, x + std::sin(angle) * radius, z + std::cos(angle) * radius, _tick + _dropLifetimeTicks);
	_drops[drop->GetObjectId()] = drop;
	_grid.Add(drop);
}

// 바닥에서 없애기: 보던 사람에게 S_DESPAWN + 각자 visible에서 지움 (KillMonster의 S_DIE와 같은 순서)
//   drop을 값(shared_ptr 복사)으로 받음: _drops.begin()->second를 참조로 받으면 아래 erase가 그 원본을 지워서
//   함수 나머지에서 사라진 객체를 쓰게 됨. 복사본이 함수 끝까지 객체를 붙잡고 있음
void Room::RemoveDrop(DropRef drop)
{
	const uint64 id = drop->GetObjectId();
	Protocol::S_DESPAWN pkt;
	pkt.add_object_ids(id);
	SendBufferRef sendBuffer = ClientPacketHandler::MakeSendBuffer(pkt);
	ForEachViewer(*drop, [&](const PlayerRef& viewer)
		{
			viewer->visible.erase(id);
			SendTo(viewer, sendBuffer);
		});
	_grid.Remove(drop);	// ForEachViewer가 끝난 뒤에 (도는 중에 칸 목록을 바꾸면 안 됨)
	_drops.erase(id);
}

// 1초마다 (Tick): 수명이 모두 같아서 만료된 드랍은 _drops 앞쪽(오래된 쪽)에 모여 있음
//   -> 앞에서부터 지우다가 아직 안 된 게 나오면 멈춤 (전체를 돌지 않음)
void Room::ExpireDrops()
{
	int32 expired = 0;
	while (_drops.empty() == false && _drops.begin()->second->GetExpireTick() <= _tick)
	{
		RemoveDrop(_drops.begin()->second);
		expired++;
	}
	if (expired > 0)
		LogLine() << "[Room] drops expired: " << expired;
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
	BroadcastToViewers(*player, ClientPacketHandler::MakeSendBuffer(pkt));

	LogLine() << "[Room] player " << player->GetObjectId() << " killed by " << killerId
		<< ", revive in " << Player::REVIVE_DELAY_MS << "ms";

	// PlayerRef가 아니라 id로 예약: 5초 사이에 나가면 RevivePlayer가 못 찾고 끝남
	//   (PlayerRef를 넘기면 job이 shared_ptr 복사본을 들고 있어서, 나간 플레이어 객체가 5초 동안 안 지워짐)
	DoTimer(Player::REVIVE_DELAY_MS, &Room::RevivePlayer, player->GetObjectId());
}

// 부활: 시작 위치에서 HP 가득, MP 절반 -> 새 위치에서 나를 보는 사람에게 S_REVIVE, 본인에게 MP
//   순간이동이라 옛 위치 주변 사람은 다음 틱 UpdateView에서 S_DESPAWN, 새 위치 주변 사람은 S_SPAWN을 받음
void Room::RevivePlayer(uint64 playerId)
{
	PlayerRef player = FindPlayer(playerId);
	if (player == nullptr || player->IsDead() == false)
		return;		// 그사이 나감

	player->Revive();
	_grid.Update(player);	// 시작 위치로 순간이동 -> 칸이 바뀌었을 수 있음

	Protocol::S_REVIVE pkt;
	*pkt.mutable_object() = player->GetObjectInfo();
	BroadcastToViewers(*player, ClientPacketHandler::MakeSendBuffer(pkt));
	SendMp(player);

	LogLine() << "[Room] player " << playerId << " revived at (" << Player::SPAWN_X << ", " << Player::SPAWN_Z << ")";
}

// 큰 패킷 나누기: 항목이 많은 S_MOVE(틱마다)와 S_SPAWN(입장할 때)은 MAX_PACKET_BODY_BYTES씩 끊어서 여러 개로 보냄
//   ServerCore의 SendBuffer 청크가 6000바이트라 그보다 큰 패킷을 만들면 ASSERT_CRASH로 서버가 죽음
//   (이동 항목 약 45바이트 -> 한 틱에 약 130개, 객체 항목 약 55바이트 -> 방에 약 110개가 한계였음)
//   ByteSizeLong: protobuf가 계산한 직렬화 크기. 반복 항목은 앞에 태그 1바이트 + 길이 1~2바이트가 더 붙어서 +3
//   나뉜 S_MOVE는 server_tick이 같음 -> 클라는 항목만 적용하면 되고, 같은 틱 번호라고 버리면 안 됨
void Room::SendMoves(const PlayerRef& player, Protocol::S_MOVE& all)
{
	// AOI 뒤로는 시야 안의 항목만이라 대부분 한 개로 충분 -> 나누지 않고 그대로
	if (all.ByteSizeLong() <= MAX_PACKET_BODY_BYTES)
	{
		SendTo(player, ClientPacketHandler::MakeSendBuffer(all));
		return;
	}

	Protocol::S_MOVE part;
	part.set_server_tick(all.server_tick());
	size_t partBytes = 0;

	for (const Protocol::MoveEntry& entry : all.moves())
	{
		const size_t entryBytes = entry.ByteSizeLong() + 3;
		if (partBytes + entryBytes > MAX_PACKET_BODY_BYTES)
		{
			SendTo(player, ClientPacketHandler::MakeSendBuffer(part));
			part.clear_moves();
			partBytes = 0;
		}
		*part.add_moves() = entry;
		partBytes += entryBytes;
	}

	if (part.moves_size() > 0)
		SendTo(player, ClientPacketHandler::MakeSendBuffer(part));
}

void Room::SendSpawns(const PlayerRef& player, const Protocol::S_SPAWN& all)
{
	Protocol::S_SPAWN part;
	size_t partBytes = 0;

	for (const Protocol::ObjectInfo& object : all.objects())
	{
		const size_t objectBytes = object.ByteSizeLong() + 3;
		if (partBytes + objectBytes > MAX_PACKET_BODY_BYTES)
		{
			SendTo(player, ClientPacketHandler::MakeSendBuffer(part));
			part.clear_objects();
			partBytes = 0;
		}
		*part.add_objects() = object;
		partBytes += objectBytes;
	}

	if (part.objects_size() > 0)
		SendTo(player, ClientPacketHandler::MakeSendBuffer(part));
}

// 방에서 나가는 패킷은 전부 여기를 지남 -> 송신량 통계도 여기서 셈
//   Room 잡큐 안에서만 불리므로(한 번에 한 스레드) 카운터는 atomic이 아니어도 됨
void Room::SendTo(PlayerRef player, SendBufferRef sendBuffer)
{
	if (GameSessionRef session = player->GetSession())
	{
		_sentPackets++;
		_sentBytes += sendBuffer->WriteSize();
		session->Send(sendBuffer);
	}
}
