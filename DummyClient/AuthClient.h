#pragma once

// 아웃게임 서버(OutGameServer) 로그인. 테스트용 계정이라 없으면 가입부터 함
//   HTTPS (WinHTTP, Windows 기본). 개발 인증서가 신뢰 등록돼 있어야 함 (dotnet dev-certs https --trust)

// 계정 하나: 가입(이미 있으면 넘어감) -> 로그인 -> Access Token. 실패하면 빈 문자열 (이유는 로그)
string LoginForTest(const wstring& host, uint16 port, const string& loginId, const string& password);

// 계정 count개 (prefix_1 ... prefix_count)를 동시에 로그인 -> 토큰 목록 (순서 = 클라 번호)
std::vector<string> LoginAll(const wstring& host, uint16 port, const string& prefix, int32 count);
