#pragma once
#include "GameObject.h"

// 바닥에 떨어진 골드 / 아이템. 움직이지 않고, 누가 줍거나 시간이 지나면 사라짐
//   무엇인지(item_id, amount)는 ObjectInfo.drop_info에 둠 -> S_SPAWN에 그대로 실림
//   Room 줄 안에서만 만들고 지움 (Room::SpawnDrops / RemoveDrop) -> 락 없음
class Drop : public GameObject
{
public:
	Drop(int32 itemId, int32 amount, float x, float z, uint64 pickableTick, uint64 expireTick)
		: GameObject(Protocol::OBJECT_TYPE_DROP), _pickableTick(pickableTick), _expireTick(expireTick)
	{
		GetPosInfo()->set_x(x);
		GetPosInfo()->set_z(z);
		Protocol::DropInfo* info = GetObjectInfo().mutable_drop_info();
		info->set_item_id(itemId);	// 0 = 골드
		info->set_amount(amount);
	}

	uint64	GetPickableTick() const { return _pickableTick; }
	uint64	GetExpireTick() const { return _expireTick; }

private:
	uint64	_pickableTick = 0;	// 이 틱부터 주울 수 있음 (떨어지는 연출 동안은 못 주움)
	uint64	_expireTick = 0;	// 이 틱이 되면 사라짐
};
