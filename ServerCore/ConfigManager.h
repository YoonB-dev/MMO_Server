#pragma once
#include <string>
#include <map>

class ConfigManager
{
public:
	ConfigManager() = default;
	~ConfigManager() = default;

	bool LoadConfig(const std::wstring& configPath);
	std::wstring GetDBConnectionString();

	// Server settings
	std::wstring GetServerIP();
	uint16 GetServerPort();
	int32 GetWorkerThreadCount();

	// [Database], [Server] 말고 다른 섹션의 값 (예: [Auth] JwtSecret). 없으면 빈 문자열
	std::string GetString(const std::string& section, const std::string& key);

private:
	std::wstring _configPath;
	std::wstring _dbConnectionString;
	std::wstring _serverIP;
	uint16 _serverPort = 7777;
	int32 _workerThreads = 5;

	std::map<std::string, std::map<std::string, std::string>> _sections;	// 섹션 이름 -> (키 -> 값)
};