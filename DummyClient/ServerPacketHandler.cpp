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

static void SendMoveInput(PacketSessionRef& session, float dirX, float dirZ, uint32 seq)
{
	Protocol::C_MOVE_INPUT pkt;
	pkt.set_dir_x(dirX);
	pkt.set_dir_z(dirZ);
	pkt.set_seq(seq);
	session->Send(ServerPacketHandler::MakeSendBuffer(pkt));
}

// 서버 입력 검증 확인용 (Test/Scenarios/MoveInputValidation.ps1)
static void SendInputTestBurst(PacketSessionRef& session)
{
	const float nan = numeric_limits<float>::quiet_NaN();

	SendMoveInput(session, 1.f, 0.f, 1);		// 통과
	SendMoveInput(session, 100.f, 0.f, 2);		// 통과 (길이 1로 줄어듦)
	SendMoveInput(session, nan, 0.f, 3);		// reject: invalid dir
	SendMoveInput(session, 0.f, 1.f, 2);		// reject: old seq
	SendMoveInput(session, 0.f, 0.f, 4);		// 통과 (멈춤)

	// 개수 제한: 위 5개 포함 총 70개를 한꺼번에 보냄
	//   -> 중간에 1초 경계(카운트 초기화)가 껴서 둘로 나뉘어도 한쪽은 35개 이상이라 rate가 반드시 생김
	for (uint32 seq = 10; seq < 75; seq++)
		SendMoveInput(session, 1.f, 0.f, seq);
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	ostringstream line;
	line << "S_ENTER_GAME success=" << pkt.success() << " myId=" << pkt.player().object_id();
	Log(ClientIdOf(session), line.str());

	if (GInputTest)
		SendInputTestBurst(session);
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

bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
	ostringstream line;
	line << "S_MOVE tick=" << pkt.server_tick() << " count=" << pkt.moves_size();
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
