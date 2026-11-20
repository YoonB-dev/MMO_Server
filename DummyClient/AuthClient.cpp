#include "pch.h"
#include "AuthClient.h"
#include "ServerSession.h"
#include <winhttp.h>
#include <future>
#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

static const string TEST_PASSWORD = "dummy-password-1";	// 테스트 계정 공통 비밀번호 (테스트 DB 전용)

// WinHTTP 핸들을 소멸자에서 닫는 래퍼 (RAII) -> 중간에 return해도 안 샘
struct WinHttpHandle
{
	HINTERNET handle = nullptr;
	~WinHttpHandle() { if (handle) ::WinHttpCloseHandle(handle); }
};

struct HttpResult
{
	int32	status = 0;		// 0 = 연결 자체가 실패 (서버 꺼짐, 인증서 문제)
	string	body;
};

// JSON 요청을 보내고 상태 코드와 응답 본문을 받음
//   method: L"GET" 또는 L"POST" (GET이면 body는 빈 문자열), token: 비어 있지 않으면 Authorization: Bearer 헤더 (로비 API)
static HttpResult SendJson(const wstring& host, uint16 port, const wchar_t* method, const wchar_t* path, const string& body, const string& token)
{
	HttpResult result;

	WinHttpHandle session;
	session.handle = ::WinHttpOpen(L"MMO-DummyClient/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session.handle == nullptr)
		return result;

	WinHttpHandle connect;
	connect.handle = ::WinHttpConnect(session.handle, host.c_str(), port, 0);
	if (connect.handle == nullptr)
		return result;

	WinHttpHandle request;
	request.handle = ::WinHttpOpenRequest(connect.handle, method, path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);	// HTTPS
	if (request.handle == nullptr)
		return result;

	wstring headers = L"Content-Type: application/json\r\n";
	if (token.empty() == false)
		headers += L"Authorization: Bearer " + wstring(token.begin(), token.end()) + L"\r\n";	// JWT는 ASCII라 한 글자씩 옮겨도 됨

	if (::WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
			static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) == FALSE
		|| ::WinHttpReceiveResponse(request.handle, nullptr) == FALSE)
		return result;

	DWORD status = 0;
	DWORD statusSize = sizeof(status);
	::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
	result.status = static_cast<int32>(status);

	// 응답 본문은 여러 조각으로 올 수 있어서 남은 양이 0이 될 때까지 읽음
	DWORD available = 0;
	while (::WinHttpQueryDataAvailable(request.handle, &available) && available > 0)
	{
		string chunk(available, '\0');
		DWORD read = 0;
		if (::WinHttpReadData(request.handle, &chunk[0], available, &read) == FALSE)
			break;
		result.body.append(chunk.data(), read);
	}
	return result;
}

// 테스트 계정의 캐릭터 닉네임: Bot + 로그인 아이디의 FNV-1a 해시(16진수 16자리) = 19자
//   로그인 아이디(prefix_N)에는 _가 있어서 그대로 못 씀 (닉네임은 영문/숫자/한글만). 같은 아이디면 항상 같은 이름
static string BotNickname(const string& loginId)
{
	uint64 hash = 14695981039346656037ull;	// FNV-1a 64비트 시작값
	for (const unsigned char c : loginId)
	{
		hash ^= c;
		hash *= 1099511628211ull;			// FNV 소수
	}
	char buf[32];
	::snprintf(buf, sizeof(buf), "Bot%016llx", hash);
	return buf;
}

LoginInfo LoginForTest(const wstring& host, uint16 port, const string& loginId, const string& password)
{
	const string body = nlohmann::json{ { "loginId", loginId }, { "password", password } }.dump();

	// 가입: 201(새로 만듦), 409(이미 있음) 둘 다 괜찮음
	const HttpResult reg = SendJson(host, port, L"POST", L"/auth/register", body, "");
	if (reg.status != 201 && reg.status != 409)
	{
		Log("[Client] register failed: " + loginId + " (status " + to_string(reg.status) + ")");
		return {};
	}

	const HttpResult login = SendJson(host, port, L"POST", L"/auth/login", body, "");
	if (login.status != 200)
	{
		Log("[Client] login failed: " + loginId + " (status " + to_string(login.status) + ")");
		return {};
	}

	LoginInfo info;
	try
	{
		info.token = nlohmann::json::parse(login.body).at("accessToken").get<string>();
	}
	catch (const std::exception&)
	{
		Log("[Client] login response has no accessToken: " + loginId);
		return {};
	}

	// 로비: 내 캐릭터 목록 -> 있으면 첫 번째, 없으면 새로 만듦 (아웃게임 서버만 캐릭터를 만들 수 있음)
	try
	{
		const HttpResult list = SendJson(host, port, L"GET", L"/characters", "", info.token);
		if (list.status != 200)
		{
			Log("[Client] character list failed: " + loginId + " (status " + to_string(list.status) + ")");
			return {};
		}

		const nlohmann::json characters = nlohmann::json::parse(list.body);
		if (characters.empty() == false)
		{
			info.characterId = characters[0].at("characterId").get<int64>();
			return info;
		}

		const string createBody = nlohmann::json{ { "nickname", BotNickname(loginId) } }.dump();
		const HttpResult created = SendJson(host, port, L"POST", L"/characters", createBody, info.token);
		if (created.status != 201)
		{
			Log("[Client] character create failed: " + loginId + " (status " + to_string(created.status) + ")");
			return {};
		}
		info.characterId = nlohmann::json::parse(created.body).at("characterId").get<int64>();
		return info;
	}
	catch (const std::exception&)
	{
		Log("[Client] unexpected character response: " + loginId);
		return {};
	}
}

std::vector<LoginInfo> LoginAll(const wstring& host, uint16 port, const string& prefix, int32 count)
{
	// 계정마다 std::async로 따로 실행 -> 서버가 비밀번호 해시를 여러 코어에서 동시에 처리
	//   async는 함수를 다른 스레드에서 시작하고, 결과를 나중에 꺼낼 수 있는 future를 바로 돌려줌
	std::vector<std::future<LoginInfo>> futures;
	for (int32 i = 1; i <= count; i++)
		futures.push_back(std::async(std::launch::async, LoginForTest, host, port, prefix + "_" + to_string(i), TEST_PASSWORD));

	// get(): 그 계정의 결과가 나올 때까지 기다렸다가 꺼냄 (앞에서 이미 전부 시작해 둬서 기다리는 동안 나머지도 진행 중)
	std::vector<LoginInfo> logins;
	for (std::future<LoginInfo>& f : futures)
		logins.push_back(f.get());
	return logins;
}
