#pragma once
#include "Struct.pb.h"

// 월드에 존재하는 모든 객체(플레이어, 몬스터 등)의 공통 부모
class GameObject
{
public:
	GameObject(Protocol::ObjectType type);
	virtual ~GameObject() = default;

	uint64					GetObjectId() const { return _objectInfo.object_id(); }
	Protocol::ObjectInfo&	GetObjectInfo() { return _objectInfo; }
	Protocol::MoveInfo*		GetMoveInfo() { return _objectInfo.mutable_move_info(); }
	Protocol::PosInfo*		GetPosInfo() { return GetMoveInfo()->mutable_pos_info(); }
	Protocol::StatInfo*		GetStatInfo() { return _objectInfo.mutable_stat_info(); }

public:
	// --- AOI (Room JobQueue 안에서만 접근 -> 락 없음) ---
	int32	gridCell = -1;	// 들어 있는 Grid 칸 번호 (-1 = 그리드에 없음). Grid만 바꿈
	uint64	moveTick = 0;	// 마지막으로 S_MOVE 항목이 생긴 틱. == 지금 틱이면 "이번 틱에 움직임"
	int32	moveIndex = 0;	// 그 틱의 전체 이동 목록에서 몇 번째 항목인지 (시야 안의 사람에게 이 항목을 복사해서 보냄)

private:
	Protocol::ObjectInfo	_objectInfo;	// id, 타입, 이동 정보(MoveInfo), 스탯(StatInfo)을 합성으로 보관 -> 패킷에 그대로 복사해서 보냄
};
