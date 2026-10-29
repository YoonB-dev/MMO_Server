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

using GameSessionRef = std::shared_ptr<class GameSession>;
using GameObjectRef = std::shared_ptr<class GameObject>;
using PlayerRef = std::shared_ptr<class Player>;
using RoomRef = std::shared_ptr<class Room>;

// 패킷 입/출력 수 (생성되는 ClientPacketHandler.h에서 증가시킴, 정의는 GameServer.cpp)
extern std::atomic<int64> g_inboundPacketCount;
extern std::atomic<int64> g_outboundPacketCount;
