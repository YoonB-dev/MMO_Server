#pragma once
#include "GameObject.h"
#include "GameData.h"	// GetExpToNext, GetMaxLevel (레벨표)

// 클라가 보낸 이동 입력 하나 (C_MOVE_INPUT의 필드와 1:1)
struct MoveInput
{
	float	dirX = 0.f;		// 좌우 (-1 ~ 1)
	float	dirZ = 0.f;		// 앞뒤 (-1 ~ 1)
	uint32	buttons = 0;	// bit0 = 점프 (저장만 하고 미사용)
	uint32	seq = 0;		// 클라가 붙인 입력 번호 1, 2, 3 ...
};

// 시야 목록(Player::visible)의 값 하나: 이 플레이어가 그 객체를 언제 확인했고, 그 객체의 이동을 언제 보냈나
struct ViewEntry
{
	uint64	seenTick = 0;	// 마지막으로 "시야 안"이라고 확인한 틱 (매 틱 UpdateView가 지금 틱으로 바꿈. 아니면 S_DESPAWN)
	uint64	sentTick = 0;	// 이 플레이어에게 그 객체의 이동(또는 S_SPAWN)을 마지막으로 보낸 틱 -> 거리별 갱신 빈도
};

// 인벤토리 한 칸 (DB player_inventory 한 줄). itemId 0 = 빈 칸
struct ItemSlot
{
	int32	slot = 0;		// 칸 번호 0 ~ inventorySize - 1
	int32	itemId = 0;
	int32	count = 0;		// 1 ~ max_stack (빈 칸이면 0)
};

// DB player_character 한 줄 + 그 캐릭터의 player_inventory 줄들 (계정당 여러 캐릭터, 아웃게임 서버가 만듦)
//   DB 큐가 불러와서 Player를 만들 때, Room이 저장할 값을 복사해서 DB 큐에 넘길 때 씀
struct CharacterData
{
	int64	characterId = 0;	// player_character PK (저장할 때 어느 줄인지)
	int64	accountId = 0;		// 계정 단위 동시 접속 관리(DB 줄의 _online)용
	float	x = 0.f;
	float	z = 0.f;
	int32	hp = 0;
	int32	mp = 0;
	int32	level = 1;
	int64	exp = 0;			// 이번 레벨에서 쌓인 경험치
	int64	gold = 0;
	vector<ItemSlot>	items;	// 아이템이 있는 칸들 (불러올 때 채우고, 저장할 때 전부 UPSERT)
	WCHAR	nickname[21] = {};	// 불러올 때만 채움 (게임 중에 안 바뀌어서 저장하지 않음). 최대 20글자 + 끝 0
};

// 플레이어 = 월드 객체 + 접속 중인 세션 + 계정
class Player : public GameObject
{
	// 플레이어 기본 수치 (몬스터, 스킬과 달리 DB가 아니라 코드 상수)
	//   새 캐릭터 기본값(Database/Migrations/009의 DEFAULT)과 같은 값이어야 함: MAX_HP, MAX_MP, SPAWN_X, SPAWN_Z
	static constexpr int32	MAX_HP = 100;

	// 1초당 자연 회복량. 회복 규칙은 Regen() 한 곳에만 있음
	static constexpr int32	HP_REGEN_PER_SECOND = 1;
	static constexpr int32	MP_REGEN_PER_SECOND = 1;	// 원거리 스킬 최대 소모(초당 4)보다 작아야 MP가 의미 있음

public:
	// MP는 본인만 보는 자원 -> ObjectInfo가 아니라 Player 멤버 (S_ENTER_GAME / S_CHANGE_MP로 본인에게만)
	static constexpr int32	MAX_MP = 100;

	// 죽음 / 부활
	static constexpr uint64	REVIVE_DELAY_MS = 5000;	// 죽고 나서 부활까지 (DoTimer 시간이라 uint64)
	static constexpr float	SPAWN_X = 0.f;			// 입장, 부활 위치
	static constexpr float	SPAWN_Z = 0.f;

	// 인벤토리 칸 수. 클래스 안의 전역 값 하나 (inline static: 헤더에 쓴 것으로 정의까지 끝남, C++17)
	//   main에서 서버가 뜨기 전에 한 번 정하고(--inventory-size) 이후 읽기만 함 -> 여러 스레드가 읽어도 안전
	static constexpr int32	DEFAULT_INVENTORY_SIZE = 20;
	inline static int32		inventorySize = DEFAULT_INVENTORY_SIZE;

	// Regen()의 결과: 실제로 바뀐 것만 true (꽉 차 있으면 false -> Room이 패킷을 안 보냄)
	struct RegenResult
	{
		bool	hpChanged = false;
		bool	mpChanged = false;
	};

public:
	// DB에서 불러온 값으로 만듦. 값이 범위를 벗어나 있어도(DB를 손으로 고친 경우 등) 서버가 망가지지 않게 잘라서 씀
	//   HP 0(죽은 채로 저장됨)이면 시작 위치에서 HP 가득으로 (부활과 같은 처리)
	Player(GameSessionRef session, const CharacterData& data)
		: GameObject(Protocol::OBJECT_TYPE_PLAYER), _session(session), _characterId(data.characterId), _accountId(data.accountId)
	{
		const bool dead = (data.hp <= 0);
		GetPosInfo()->set_x(dead ? SPAWN_X : data.x);
		GetPosInfo()->set_z(dead ? SPAWN_Z : data.z);
		GetStatInfo()->set_hp(dead ? MAX_HP : (data.hp > MAX_HP ? MAX_HP : data.hp));
		GetStatInfo()->set_max_hp(MAX_HP);
		mp = (data.mp < 0) ? 0 : (data.mp > MAX_MP ? MAX_MP : data.mp);

		// 레벨표가 바뀌어 최대 레벨이 낮아졌거나, DB를 손으로 고친 경우에도 서버가 망가지지 않게
		const int32 level = (data.level < 1) ? 1 : (data.level > GetMaxLevel() ? GetMaxLevel() : data.level);
		GetStatInfo()->set_level(level);
		const int64 need = GetExpToNext(level);
		exp = (data.exp < 0 || need == 0) ? 0 : (data.exp >= need ? need - 1 : data.exp);

		// 골드 / 인벤토리: 칸 inventorySize개를 빈 칸으로 만든 뒤, DB 줄을 그 칸 번호에 넣음
		//   건너뜀: 칸 번호가 범위 밖(칸 수를 줄인 경우), 기획에서 지운 아이템(data_item에 없음), 개수 0 이하
		//   (건너뛴 DB 줄은 그대로 남음. 저장은 서버가 아는 칸만 UPSERT)
		//   개수는 max_stack까지 자름 (max_stack을 줄인 경우)
		gold = (data.gold < 0) ? 0 : data.gold;
		inventory.resize(inventorySize);
		for (int32 i = 0; i < inventorySize; i++)
			inventory[i].slot = i;
		for (const ItemSlot& item : data.items)
		{
			const ItemData* itemData = FindItemData(item.itemId);
			if (item.slot < 0 || item.slot >= inventorySize || itemData == nullptr || item.count <= 0)
				continue;
			inventory[item.slot].itemId = item.itemId;
			inventory[item.slot].count = (item.count > itemData->maxStack) ? itemData->maxStack : item.count;
		}

		// 닉네임 WCHAR(UTF-16, DB에서 옴) -> UTF-8(protobuf string). 한글이 있어서 한 글자씩 자르면 깨짐
		char utf8[64] = {};	// 최대 20글자 x 한 글자 최대 3바이트 + 끝 0
		const int32 len = ::WideCharToMultiByte(CP_UTF8, 0, data.nickname, -1, utf8, sizeof(utf8), nullptr, nullptr);
		GetObjectInfo().set_name(len > 0 ? utf8 : "");
	}

	int64	GetAccountId() const { return _accountId; }

	// 지금 상태를 DB 한 줄 모양으로 복사 (Room 줄 안에서만 호출 -> 저장은 DB 줄이 이 복사본으로)
	//   닉네임은 저장하지 않아서 비워 둠
	CharacterData ToCharacterData()
	{
		CharacterData data{ _characterId, _accountId, GetPosInfo()->x(), GetPosInfo()->z(), GetStatInfo()->hp(), mp, GetLevel(), exp, gold };
		for (const ItemSlot& s : inventory)
		{
			if (s.itemId != 0)
				data.items.push_back(s);	// 아이템이 있는 칸만
		}
		return data;
	}

	// 다 들어갈 자리가 있나 (바꾸지 않고 세기만): 같은 아이템 칸의 남은 자리 + 빈 칸 x maxStack
	//   드랍은 통째로만 주움 -> 이게 true일 때만 AddItem
	bool CanAddItem(int32 itemId, int32 amount, int32 maxStack) const
	{
		int32 room = 0;
		for (const ItemSlot& s : inventory)
		{
			if (s.itemId == itemId)
				room += maxStack - s.count;
			else if (s.itemId == 0)
				room += maxStack;
			if (room >= amount)
				return true;
		}
		return false;
	}

	// 넣기 (CanAddItem이 true일 때만 부름): 1) 같은 아이템 칸을 maxStack까지 채움 2) 남으면 빈 칸에 번호 순서대로 새로 쌓음
	//   바뀐 칸 번호를 changed에 담음 -> Room이 S_CHANGE_ITEM으로 그 칸들만 보냄
	//   (std::min은 Windows.h의 min 매크로와 부딪혀서 삼항 연산자로)
	void AddItem(int32 itemId, int32 amount, int32 maxStack, xvector<int32>& changed)
	{
		for (ItemSlot& s : inventory)
		{
			if (amount == 0)
				return;
			if (s.itemId != itemId || s.count >= maxStack)
				continue;
			const int32 space = maxStack - s.count;
			const int32 put = (amount < space) ? amount : space;
			s.count += put;
			amount -= put;
			changed.push_back(s.slot);
		}
		for (ItemSlot& s : inventory)
		{
			if (amount == 0)
				return;
			if (s.itemId != 0)
				continue;
			const int32 put = (amount < maxStack) ? amount : maxStack;
			s.itemId = itemId;
			s.count = put;
			amount -= put;
			changed.push_back(s.slot);
		}
	}

	// 레벨은 StatInfo 한 곳에만 둠 (HP처럼). 따로 변수를 두면 두 값이 어긋날 수 있음
	int32	GetLevel() { return GetStatInfo()->level(); }

	// 경험치 얻기 (Room::GiveExp가 호출). 값만 바꾸고 오른 레벨 수를 돌려줌 -> 보내는 건 Room
	//   레벨업: 필요한 양을 넘기면 넘친 만큼 다음 레벨로 (한 번에 여러 레벨도). 최대 레벨이면 더 안 쌓음
	int32 GainExp(int64 amount)
	{
		int32 levelsUp = 0;
		exp += amount;
		while (true)
		{
			const int64 need = GetExpToNext(GetLevel());
			if (need == 0)
			{
				exp = 0;		// 최대 레벨
				break;
			}
			if (exp < need)
				break;
			exp -= need;
			GetStatInfo()->set_level(GetLevel() + 1);
			levelsUp++;
		}
		return levelsUp;
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

	// --- 성장 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	int64	exp = 0;	// 이번 레벨에서 쌓인 경험치 (본인만 보는 값이라 ObjectInfo 밖에. 레벨은 StatInfo)

	// --- 재화 / 인벤토리 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	int64				gold = 0;
	xvector<ItemSlot>	inventory;				// [칸 번호] = 그 칸 (크기 inventorySize, 빈 칸은 itemId 0)
	uint64				pickupReadyTick = 0;	// 이 틱부터 다시 줍기 가능 (스패밍 방지)

	// --- 시야 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	//   지금 이 클라에 보이는 객체 id -> ViewEntry (언제 확인했나 / 그 객체의 이동을 언제 보냈나)
	//   매 틱 Room::UpdateView가 확인한 것만 seenTick을 지금 틱으로 바꾸고, 아닌 것(= 이번에 확인 안 됨)은 S_DESPAWN 후 지움
	//   나 자신은 넣지 않음 (내 이동은 항상 받음). 다른 플레이어는 상한(우선순위 순)까지만 들어감
	xunordered_map<uint64, ViewEntry>	visible;

	// --- 채팅 (Room의 JobQueue 안에서만 접근 -> 락 없음) ---
	uint64	chatReadyTick = 0;	// 이 틱부터 다시 채팅 가능 (도배 방지). 0 = 바로 가능

private:
	weak_ptr<GameSession>	_session;	// 세션이 Player를 shared_ptr로 들고 있으므로 이쪽은 weak (순환 참조 방지)
	int64					_characterId = 0;	// player_character의 character_id (저장할 때 어느 줄인지)
	int64					_accountId = 0;		// 토큰의 sub (같은 계정 동시 접속 관리용)
};
