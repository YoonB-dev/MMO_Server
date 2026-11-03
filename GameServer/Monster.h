#pragma once
#include "GameObject.h"

// 몬스터 = 월드 객체. 세션 없이 서버가 직접 움직인다 (AI는 다음 단계부터)
class Monster : public GameObject
{
public:
	Monster(float x, float z)
		: GameObject(Protocol::OBJECT_TYPE_MONSTER)
	{
		GetPosInfo()->set_x(x);
		GetPosInfo()->set_z(z);
	}
};
