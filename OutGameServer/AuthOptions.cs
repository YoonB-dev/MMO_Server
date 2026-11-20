namespace OutGameServer;

// 설정값 묶음. 시작할 때 한 번 읽어서 검사하고, 이후 읽기만 함
//   비밀 아닌 값: appsettings.json의 Auth, GameServer
//   비밀 값: user-secrets의 ConnectionStrings:Mmo, Auth:JwtSecret (Setup-Secrets.ps1이 넣음, 저장소에 없음)
public sealed class AuthOptions
{
	public required string ConnectionString { get; init; }
	public required byte[] JwtSecret { get; init; }		// HS256 비밀키. 게임 서버도 같은 키로 검증 (2단계에서 config.ini [Auth])
	public required string Issuer { get; init; }		// JWT iss: 누가 발급했나
	public required string Audience { get; init; }		// JWT aud: 누구에게 쓰라고 발급했나 (게임 서버)
	public required int AccessTokenMinutes { get; init; }
	public required int RefreshTokenDays { get; init; }
	public required int Pbkdf2Iterations { get; init; }	// 비밀번호 해시 반복 수 (계정마다 저장하므로 나중에 올려도 됨)
	public required string GameServerHost { get; init; }	// 로그인 응답에 넣어 주는 게임 서버 주소
	public required int GameServerPort { get; init; }

	public static AuthOptions Load(IConfiguration config)
	{
		string connectionString = config.GetConnectionString("Mmo")
			?? throw new InvalidOperationException("ConnectionStrings:Mmo is missing (run OutGameServer/Setup-Secrets.ps1)");

		string secretText = config["Auth:JwtSecret"]
			?? throw new InvalidOperationException("Auth:JwtSecret is missing (run OutGameServer/Setup-Secrets.ps1)");
		byte[] secret = Convert.FromBase64String(secretText);
		if (secret.Length < 32)
			throw new InvalidOperationException("Auth:JwtSecret must be at least 32 bytes (HS256)");

		return new AuthOptions
		{
			ConnectionString = connectionString,
			JwtSecret = secret,
			Issuer = Required(config, "Auth:Issuer"),
			Audience = Required(config, "Auth:Audience"),
			AccessTokenMinutes = int.Parse(Required(config, "Auth:AccessTokenMinutes")),
			RefreshTokenDays = int.Parse(Required(config, "Auth:RefreshTokenDays")),
			Pbkdf2Iterations = int.Parse(Required(config, "Auth:Pbkdf2Iterations")),
			GameServerHost = Required(config, "GameServer:Host"),
			GameServerPort = int.Parse(Required(config, "GameServer:Port")),
		};
	}

	private static string Required(IConfiguration config, string key)
		=> config[key] ?? throw new InvalidOperationException($"{key} is missing in appsettings.json");
}
