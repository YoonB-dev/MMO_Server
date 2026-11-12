#pragma once
#include "Enum.pb.h"

// 아웃게임 서버(OutGameServer)가 발급한 Access Token(JWT, HS256) 검증
//   시작할 때 config.ini [Auth]로 한 번 Init하고, 이후 읽기만 함 -> 여러 IOCP 스레드가 락 없이 같이 씀
//   DB를 안 쓰고 계산만 함 (HMAC-SHA256 한 번) -> 패킷 핸들러에서 바로 불러도 됨
class JwtVerifier
{
public:
	// 성공이면 error = NONE, accountId = sub
	struct Result
	{
		Protocol::EnterGameError	error = Protocol::ENTER_GAME_ERROR_INVALID_TOKEN;
		int64						accountId = 0;
	};

	// secretBase64: [Auth] JwtSecret (OutGameServer user-secrets와 같은 값, Setup-Secrets.ps1이 넣음). 이상하면 false
	bool	Init(const string& secretBase64, const string& issuer, const string& audience);
	Result	Verify(const string& token) const;

private:
	std::vector<uint8>	_secret;	// HMAC 키 (32바이트 이상)
	string				_issuer;	// 기대하는 iss
	string				_audience;	// 기대하는 aud
};

extern JwtVerifier GJwtVerifier;
