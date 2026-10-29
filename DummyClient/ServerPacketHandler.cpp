#include "pch.h"
#include "ServerPacketHandler.h"
#include "ServerSession.h"
#include <sstream>

// ServerPacketHandler.h는 빌드할 때마다 Protocol.proto에서 생성된다
//   -> S_로 시작하는 패킷을 proto에 추가하면 여기에 Handle_S_XXX를 만들어야 링크된다
PacketHandleFunc GPacketHandler[UINT16_MAX];

static int32 ClientIdOf(PacketSessionRef& session)
{
	return static_pointer_cast<ServerSession>(session)->GetClientId();
}

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	Log(ClientIdOf(session), "Invalid packet - ID: " + to_string(header->id));
	return false;
}

bool Handle_S_PING(PacketSessionRef& session, Protocol::S_PING& pkt)
{
	double rttMs = (NowUs() - pkt.client_time_us()) / 1000.0;

	ostringstream line;
	line << "S_PING RTT " << rttMs << "ms";
	Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	ostringstream line;
	line << "S_ENTER_GAME success=" << pkt.success() << " myId=" << pkt.player().object_id();
	Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	ostringstream line;
	line << "S_SPAWN ids:";
	for (const Protocol::ObjectInfo& object : pkt.objects())
		line << " " << object.object_id();
	Log(ClientIdOf(session), line.str());
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	ostringstream line;
	line << "S_DESPAWN ids:";
	for (uint64 objectId : pkt.object_ids())
		line << " " << objectId;
	Log(ClientIdOf(session), line.str());
	return true;
}
