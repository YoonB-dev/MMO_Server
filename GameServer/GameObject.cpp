#include "pch.h"
#include "GameObject.h"

// 여러 워커 스레드가 동시에 객체를 만들어도 id가 겹치지 않게 atomic으로 발급
static Atomic<uint64> s_idGenerator = 1;

GameObject::GameObject(Protocol::ObjectType type)
{
	_objectInfo.set_object_id(s_idGenerator.fetch_add(1));
	_objectInfo.set_object_type(type);
}
