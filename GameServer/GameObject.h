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
	Protocol::PosInfo*		GetPosInfo() { return _objectInfo.mutable_pos_info(); }

private:
	Protocol::ObjectInfo	_objectInfo;	// id, 타입, 위치(PosInfo)를 합성으로 보관 -> 패킷에 그대로 복사해서 보냄
};
