#include "pch.h"
#include "ServerPacketHandler.h"
#include "ServerSession.h"

// ServerPacketHandler.h는 빌드할 때마다 Protocol.proto에서 생성된다
//   -> S_로 시작하는 패킷을 proto에 추가하면 여기에 Handle_S_XXX를 만들어야 링크된다
PacketHandleFunc GPacketHandler[UINT16_MAX];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	cout << "[Client] Invalid packet - ID: " << header->id << endl;
	return false;
}

bool Handle_S_PING(PacketSessionRef& session, Protocol::S_PING& pkt)
{
	double rttMs = (NowUs() - pkt.client_time_us()) / 1000.0;
	cout << "[Client] S_PING RTT " << rttMs << "ms" << endl;
	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	cout << "[Client] S_ENTER_GAME success=" << pkt.success() << " myId=" << pkt.player().object_id() << endl;
	return true;
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	cout << "[Client] S_SPAWN ids:";
	for (const Protocol::ObjectInfo& object : pkt.objects())
		cout << " " << object.object_id();
	cout << endl;
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	cout << "[Client] S_DESPAWN ids:";
	for (uint64 objectId : pkt.object_ids())
		cout << " " << objectId;
	cout << endl;
	return true;
}
