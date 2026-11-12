using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text;
using Microsoft.IdentityModel.JsonWebTokens;
using Microsoft.IdentityModel.Tokens;

namespace OutGameServer.Auth;

// 토큰 두 종류
//   Access Token: JWT (header.payload.signature). 게임 서버가 DB 없이 서명만 보고 검증. 수명 짧게 (털려도 금방 무효)
//   Refresh Token: 랜덤 32바이트 (의미 없는 문자열). /auth/refresh에서만 씀. DB에는 SHA-256 해시만 저장
public sealed class TokenService
{
	private readonly AuthOptions _options;
	private readonly SigningCredentials _signing;
	private readonly JsonWebTokenHandler _handler = new();

	public TokenService(AuthOptions options)
	{
		_options = options;
		// HS256 = HMAC-SHA256: 같은 비밀키로 서명하고 검증 (게임 서버도 같은 키를 가짐)
		_signing = new SigningCredentials(new SymmetricSecurityKey(options.JwtSecret), SecurityAlgorithms.HmacSha256);
	}

	public int AccessTokenSeconds => _options.AccessTokenMinutes * 60;
	public int RefreshTokenSeconds => _options.RefreshTokenDays * 24 * 60 * 60;

	// claims: payload에 들어가는 키-값 = AuthServer와 GameServer 사이의 약속
	//   iss(발급자), aud(대상), sub(주인 = account_id), iat(발급), exp(만료), nbf(이전엔 무효), jti(토큰 id)
	//   payload는 암호화가 아니라 인코딩이라 누구나 읽을 수 있음 -> 비밀은 절대 넣지 않음
	public string CreateAccessToken(long accountId)
	{
		DateTime now = DateTime.UtcNow;
		var descriptor = new SecurityTokenDescriptor
		{
			Issuer = _options.Issuer,
			Audience = _options.Audience,
			IssuedAt = now,
			NotBefore = now,
			Expires = now.AddMinutes(_options.AccessTokenMinutes),
			Claims = new Dictionary<string, object>
			{
				[JwtRegisteredClaimNames.Sub] = accountId.ToString(),
				[JwtRegisteredClaimNames.Jti] = Guid.NewGuid().ToString("N"),
			},
			SigningCredentials = _signing,
		};
		return _handler.CreateToken(descriptor);
	}

	// Refresh Token 원문: 랜덤 32바이트를 URL에 써도 되는 base64로 (클라에게만 주고 서버는 안 들고 있음)
	public static string CreateRefreshToken()
		=> Base64Url.EncodeToString(RandomNumberGenerator.GetBytes(32));

	// DB에 저장하고 찾을 때 쓰는 값. 토큰이 이미 충분히 긴 랜덤이라 느린 해시(PBKDF2)는 필요 없음
	public static byte[] HashRefreshToken(string refreshToken)
		=> SHA256.HashData(Encoding.UTF8.GetBytes(refreshToken));
}
