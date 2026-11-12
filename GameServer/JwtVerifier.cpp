#include "pch.h"
#include "JwtVerifier.h"
#include <bcrypt.h>
#include <chrono>
#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

JwtVerifier GJwtVerifier;

static constexpr int64 CLOCK_SKEW_SEC = 30;	// 서버끼리 시계가 조금 달라도 정상 토큰이 거절되지 않게
static constexpr size_t SIGNATURE_SIZE = 32;	// HMAC-SHA256 결과 크기
static constexpr size_t MIN_SECRET_SIZE = 32;

// base64 디코드. JWT 조각은 base64url('-', '_', 패딩 없음), config의 키는 일반 base64('+', '/', '=' 패딩)
//   6비트씩 모아서 8비트가 차면 1바이트로 꺼냄. 허용 안 되는 글자가 있으면 false
static bool Base64Decode(const string& text, bool url, std::vector<uint8>& out)
{
	out.clear();
	uint32 buffer = 0;
	int32 bits = 0;
	for (char c : text)
	{
		int32 value;
		if (c >= 'A' && c <= 'Z')			value = c - 'A';
		else if (c >= 'a' && c <= 'z')		value = c - 'a' + 26;
		else if (c >= '0' && c <= '9')		value = c - '0' + 52;
		else if (c == (url ? '-' : '+'))	value = 62;
		else if (c == (url ? '_' : '/'))	value = 63;
		else if (c == '=' && url == false)	break;	// 패딩부터는 끝
		else								return false;

		buffer = (buffer << 6) | static_cast<uint32>(value);
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<uint8>((buffer >> bits) & 0xFF));
		}
	}
	return true;
}

// BCrypt 핸들을 소멸자에서 닫는 작은 래퍼 (RAII) -> 중간에 실패로 return해도 핸들이 새지 않음
struct BCryptAlgHandle
{
	BCRYPT_ALG_HANDLE handle = nullptr;
	~BCryptAlgHandle() { if (handle) ::BCryptCloseAlgorithmProvider(handle, 0); }
};

struct BCryptHashHandle
{
	BCRYPT_HASH_HANDLE handle = nullptr;
	~BCryptHashHandle() { if (handle) ::BCryptDestroyHash(handle); }
};

// HMAC-SHA256(key, data) -> out (Windows 기본 암호 API, 라이브러리 추가 없음)
static bool HmacSha256(const std::vector<uint8>& key, const string& data, uint8 (&out)[SIGNATURE_SIZE])
{
	BCryptAlgHandle alg;
	if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) == false)
		return false;

	BCryptHashHandle hash;
	if (BCRYPT_SUCCESS(::BCryptCreateHash(alg.handle, &hash.handle, nullptr, 0,
		const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)) == false)
		return false;

	if (BCRYPT_SUCCESS(::BCryptHashData(hash.handle,
		reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0)) == false)
		return false;

	return BCRYPT_SUCCESS(::BCryptFinishHash(hash.handle, out, SIGNATURE_SIZE, 0));
}

// 끝까지 다 비교 (앞에서 다른 바이트를 만나도 멈추지 않음) -> 걸린 시간으로 몇 바이트가 맞았는지 알 수 없음
static bool ConstantTimeEquals(const uint8* a, const uint8* b, size_t size)
{
	uint8 diff = 0;
	for (size_t i = 0; i < size; i++)
		diff |= static_cast<uint8>(a[i] ^ b[i]);
	return diff == 0;
}

bool JwtVerifier::Init(const string& secretBase64, const string& issuer, const string& audience)
{
	if (secretBase64.empty() || issuer.empty() || audience.empty())
		return false;
	if (Base64Decode(secretBase64, false, _secret) == false || _secret.size() < MIN_SECRET_SIZE)
		return false;

	_issuer = issuer;
	_audience = audience;
	return true;
}

// 순서: 형식 -> alg -> 서명 -> claims. 서명을 확인하기 전에는 payload를 믿지 않음
JwtVerifier::Result JwtVerifier::Verify(const string& token) const
{
	Result result;	// 기본값 INVALID_TOKEN -> 아래에서 통과한 경우만 바꿈

	// 1. 형식: header.payload.signature (점이 정확히 2개)
	const size_t dot1 = token.find('.');
	if (dot1 == string::npos)
		return result;
	const size_t dot2 = token.find('.', dot1 + 1);
	if (dot2 == string::npos || token.find('.', dot2 + 1) != string::npos)
		return result;

	std::vector<uint8> headerJson, payloadJson, signature;
	if (Base64Decode(token.substr(0, dot1), true, headerJson) == false
		|| Base64Decode(token.substr(dot1 + 1, dot2 - dot1 - 1), true, payloadJson) == false
		|| Base64Decode(token.substr(dot2 + 1), true, signature) == false)
		return result;

	try
	{
		// 2. alg는 HS256만: "none"이나 다른 알고리즘으로 서명 검사를 건너뛰게 하는 공격을 막음
		const nlohmann::json header = nlohmann::json::parse(headerJson.begin(), headerJson.end());
		if (header.value("alg", "") != "HS256")
			return result;

		// 3. 서명: HMAC-SHA256(키, "header.payload")가 signature와 같아야 함 -> 내용이 1바이트라도 바뀌었으면 다름
		uint8 expected[SIGNATURE_SIZE];
		if (signature.size() != SIGNATURE_SIZE || HmacSha256(_secret, token.substr(0, dot2), expected) == false)
			return result;
		if (ConstantTimeEquals(expected, signature.data(), SIGNATURE_SIZE) == false)
			return result;

		// 4. claims: 우리 인증 서버가(iss) 게임 입장용으로(aud) 발급했고, 지금 유효한가(nbf, exp)
		const nlohmann::json payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end());
		if (payload.value("iss", "") != _issuer || payload.value("aud", "") != _audience)
			return result;

		const int64 now = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		if (payload.value("nbf", int64(0)) > now + CLOCK_SKEW_SEC)
			return result;
		if (payload.at("exp").get<int64>() <= now - CLOCK_SKEW_SEC)
		{
			result.error = Protocol::ENTER_GAME_ERROR_TOKEN_EXPIRED;
			return result;
		}

		// 5. sub = account_id
		const int64 accountId = std::stoll(payload.at("sub").get<string>());
		if (accountId <= 0)
			return result;

		result.error = Protocol::ENTER_GAME_ERROR_NONE;
		result.accountId = accountId;
		return result;
	}
	catch (const std::exception&)
	{
		return result;	// JSON 형식 오류, 키 없음, 타입이 다름, sub가 숫자가 아님
	}
}
