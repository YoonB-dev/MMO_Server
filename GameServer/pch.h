#pragma once

#define WIN32_LEAN_AND_MEAN             // 거의 사용되지 않는 내용을 Windows 헤더에서 제외합니다.

#ifdef _DEBUG
#pragma comment(lib, "ServerCore\\Debug\\ServerCore.lib")
#pragma comment(lib, "Protobuf\\Debug\\libprotobufd.lib")
#else
#pragma comment(lib, "ServerCore\\Release\\ServerCore.lib")
#pragma comment(lib, "Protobuf\\Release\\libprotobuf.lib")
#endif

#include "CorePch.h"
#include "LogLine.h"	// 서버 로그 한 줄 (여러 스레드가 찍어도 안 섞임)

using GameSessionRef = std::shared_ptr<class GameSession>;
using GameObjectRef = std::shared_ptr<class GameObject>;
using PlayerRef = std::shared_ptr<class Player>;
using MonsterRef = std::shared_ptr<class Monster>;
using DropRef = std::shared_ptr<class Drop>;
using RoomRef = std::shared_ptr<class Room>;

// 구간 재기용 지금 시각 (마이크로초). steady_clock: 시계 맞춤으로 되돌아가지 않음
//   inline: 헤더에 정의해도, 여러 .cpp가 포함할 때 "같은 함수가 여러 개" 링크 오류가 안 남 (하나로 합쳐짐)
//   int64: DoAsync에 넘기는 requestUs와 타입이 정확히 같아야 함 (DBQueue)
inline int64 NowUs()
{
	return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

// 패킷 입/출력 수 (생성되는 ClientPacketHandler.h에서 증가시킴, 정의는 GameServer.cpp)
extern std::atomic<int64> g_inboundPacketCount;
extern std::atomic<int64> g_outboundPacketCount;
