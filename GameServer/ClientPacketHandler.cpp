#include "pch.h"
#include "ClientPacketHandler.h"
#include "GameSession.h"
#include "Player.h"
#include "Room.h"
#include "JwtVerifier.h"
#include "DBQueue.h"

// ClientPacketHandler.h는 빌드할 때마다 Protocol.proto에서 생성된다 (Common/protoc-21.12-win64/bin/GenPackets.bat)
//   -> C_로 시작하는 패킷을 proto에 추가하면 여기에 Handle_C_XXX를 만들어야 링크된다
// 핸들러는 IOCP 워커 스레드에서 동시에 실행됨 -> 게임 상태는 직접 만지지 않고 GRoom->DoAsync로 넘김
PacketHandleFunc GPacketHandler[UINT16_MAX];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	LogLine() << "Invalid packet received - ID: " << header->id << ", Size: " << header->size;
	return false;
}

bool Handle_C_PING(PacketSessionRef& session, Protocol::C_PING& pkt)
{
	Protocol::S_PING pongPkt;
	pongPkt.set_client_time_us(pkt.client_time_us());
	session->Send(ClientPacketHandler::MakeSendBuffer(pongPkt));
	return true;
}

// 입장: 토큰 검증(여기, 계산만) -> 캐릭터 불러오기(DB 큐) -> 입장(Room 줄)
//   실패해도 연결은 끊지 않음 (클라가 토큰을 갱신해서 다시 보낼 수 있게)
bool Handle_C_ENTER_GAME(PacketSessionRef& session, Protocol::C_ENTER_GAME& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);

	// 이미 입장했거나 처리 중이면 거절 (같은 세션에서 두 번 보내기)
	if (gameSession->TryBeginEnter() == false)
	{
		Protocol::S_ENTER_GAME fail;
		fail.set_success(false);
		fail.set_error(Protocol::ENTER_GAME_ERROR_ALREADY_ENTERED);
		session->Send(ClientPacketHandler::MakeSendBuffer(fail));
		return true;
	}

	// 아웃게임 서버가 발급한 Access Token(JWT) 검증. HMAC 계산 한 번이라 이 IOCP 스레드에서 바로 함 (DB 안 씀)
	const JwtVerifier::Result auth = GJwtVerifier.Verify(pkt.access_token());
	if (auth.error != Protocol::ENTER_GAME_ERROR_NONE)
	{
		gameSession->FailEnter(auth.error);
		return true;
	}

	// 캐릭터 불러오기는 DB 큐에서 (IOCP 스레드도 Room 줄도 DB를 기다리지 않게)
	//   내 계정 캐릭터인지는 DB가 account_id까지 같이 보고 판단
	GDBQueue->DoAsync(&DBQueue::LoadCharacter, gameSession, auth.accountId, pkt.character_id());
	return true;
}

bool Handle_C_MOVE_INPUT(PacketSessionRef& session, Protocol::C_MOVE_INPUT& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	PlayerRef player = gameSession->GetPlayer();
	if (player == nullptr)
		return false;	// 아직 입장 안 한 세션의 입력

	// 검증과 저장은 Room의 줄에서 (Player의 입력 상태는 Room 안에서만 만짐)
	GRoom->DoAsync(&Room::HandleMoveInput, player, pkt.dir_x(), pkt.dir_z(), pkt.buttons(), pkt.seq());
	return true;
}

bool Handle_C_SKILL(PacketSessionRef& session, Protocol::C_SKILL& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	PlayerRef player = gameSession->GetPlayer();
	if (player == nullptr)
		return false;	// 아직 입장 안 한 세션의 요청

	// 검사와 처리는 전부 Room의 줄에서 (쿨타임, 몬스터 위치/HP는 Room 안에서만 만짐)
	GRoom->DoAsync(&Room::HandleSkill, player, pkt.skill_id(), pkt.target_id());
	return true;
}

bool Handle_C_CHAT(PacketSessionRef& session, Protocol::C_CHAT& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	PlayerRef player = gameSession->GetPlayer();
	if (player == nullptr)
		return false;	// 아직 입장 안 한 세션의 채팅

	// 검사와 전송은 Room의 줄에서 (쿨타임, 시야 목록은 Room 안에서만 만짐)
	//   문자열은 복사 대신 move: pkt는 이 함수가 끝나면 사라지므로 내용물을 Job으로 옮김
	GRoom->DoAsync(&Room::HandleChat, player, pkt.channel(), std::move(*pkt.mutable_message()));
	return true;
}

bool Handle_C_PICKUP(PacketSessionRef& session, Protocol::C_PICKUP& pkt)
{
	GameSessionRef gameSession = static_pointer_cast<GameSession>(session);
	PlayerRef player = gameSession->GetPlayer();
	if (player == nullptr)
		return false;	// 아직 입장 안 한 세션의 줍기

	// 무엇을 주울지는 Room 줄에서 고름 (드랍 목록, 인벤토리는 Room 안에서만 만짐)
	GRoom->DoAsync(&Room::HandlePickup, player);
	return true;
}
