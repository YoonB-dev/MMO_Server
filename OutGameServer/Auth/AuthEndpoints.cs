using System.Text.RegularExpressions;
using OutGameServer.Data;

namespace OutGameServer.Auth;

// /auth/* 엔드포인트 (HTTPS)
//   register: 가입            -> 201 / 400 형식 / 409 이미 있음
//   login:    로그인          -> 200 {Access, Refresh, 게임 서버 주소} / 401
//   refresh:  토큰 갱신(회전) -> 200 새 토큰 쌍 / 401 (폐기된 토큰 재사용이면 그 계정의 Refresh 전부 폐기)
//   logout:   Refresh 폐기    -> 204
// 비밀번호와 토큰은 로그에 절대 남기지 않음
public static partial class AuthEndpoints
{
	// 요청 / 응답 모양. JSON 이름은 camelCase (loginId, accessToken ...)
	public sealed record RegisterRequest(string? LoginId, string? Password);
	public sealed record LoginRequest(string? LoginId, string? Password);
	public sealed record RefreshRequest(string? RefreshToken);
	public sealed record GameServerAddress(string Host, int Port);
	public sealed record TokenResponse(string AccessToken, int AccessTokenExpiresIn, string RefreshToken, int RefreshTokenExpiresIn, GameServerAddress GameServer);
	public sealed record ErrorResponse(string Error);

	private const int PasswordMinLength = 8;
	private const int PasswordMaxLength = 64;

	[GeneratedRegex("^[A-Za-z0-9_]{4,20}$")]
	private static partial Regex LoginIdPattern();

	public static void MapAuthEndpoints(this WebApplication app)
	{
		RouteGroupBuilder auth = app.MapGroup("/auth");
		auth.MapPost("/register", RegisterAsync);
		auth.MapPost("/login", LoginAsync);
		auth.MapPost("/refresh", RefreshAsync);
		auth.MapPost("/logout", LogoutAsync);
	}

	// 매개변수 중 요청 본문(record)은 JSON에서, 나머지(PasswordHasher 등)는 등록해 둔 서비스에서 채워짐
	private static async Task<IResult> RegisterAsync(RegisterRequest req, PasswordHasher hasher, AccountRepository repo)
	{
		if (req.LoginId == null || LoginIdPattern().IsMatch(req.LoginId) == false)
			return Results.BadRequest(new ErrorResponse("invalid_login_id"));
		if (req.Password == null || req.Password.Length < PasswordMinLength || req.Password.Length > PasswordMaxLength)
			return Results.BadRequest(new ErrorResponse("invalid_password"));

		var (hash, salt, iterations) = hasher.Hash(req.Password);
		long? accountId = await repo.CreateAccountAsync(req.LoginId, hash, salt, iterations);
		if (accountId == null)
		{
			Console.WriteLine($"[Auth] register rejected: login id taken ({req.LoginId})");
			return Results.Conflict(new ErrorResponse("login_id_taken"));
		}

		Console.WriteLine($"[Auth] register account {accountId} ({req.LoginId})");
		return Results.Created($"/accounts/{accountId}", null);
	}

	private static async Task<IResult> LoginAsync(LoginRequest req, PasswordHasher hasher, TokenService tokens, AccountRepository repo, AuthOptions options)
	{
		if (req.LoginId == null || req.Password == null)
			return Results.BadRequest(new ErrorResponse("invalid_request"));

		AccountRepository.AccountRow? account = await repo.FindAccountAsync(req.LoginId);
		if (account == null)
		{
			hasher.SpendSameTime(req.Password);	// 없는 아이디도 해시 한 번만큼 시간을 써서, 응답 시간으로 구분 못 하게
			Console.WriteLine("[Auth] login failed");
			return Results.Json(new ErrorResponse("invalid_credentials"), statusCode: StatusCodes.Status401Unauthorized);
		}

		if (hasher.Verify(req.Password, account.PasswordHash, account.PasswordSalt, account.PasswordIterations) == false)
		{
			Console.WriteLine("[Auth] login failed");	// 아이디가 없을 때와 같은 응답 -> 어느 아이디가 있는지 알려 주지 않음
			return Results.Json(new ErrorResponse("invalid_credentials"), statusCode: StatusCodes.Status401Unauthorized);
		}

		Console.WriteLine($"[Auth] login account {account.AccountId}");
		return Results.Ok(await IssueTokensAsync(account.AccountId, tokens, repo, options));
	}

	private static async Task<IResult> RefreshAsync(RefreshRequest req, TokenService tokens, AccountRepository repo, AuthOptions options)
	{
		if (string.IsNullOrEmpty(req.RefreshToken))
			return Results.BadRequest(new ErrorResponse("invalid_request"));

		string newRefresh = TokenService.CreateRefreshToken();
		var (result, accountId) = await repo.RotateRefreshTokenAsync(
			TokenService.HashRefreshToken(req.RefreshToken), TokenService.HashRefreshToken(newRefresh), options.RefreshTokenDays);

		switch (result)
		{
			case AccountRepository.RotateResult.Ok:
				Console.WriteLine($"[Auth] refresh account {accountId}");
				return Results.Ok(BuildResponse(accountId, newRefresh, tokens, options));
			case AccountRepository.RotateResult.Reused:
				Console.WriteLine($"[Auth] refresh token reused: revoked all tokens of account {accountId}");
				return Results.Json(new ErrorResponse("token_reused"), statusCode: StatusCodes.Status401Unauthorized);
			case AccountRepository.RotateResult.Expired:
				return Results.Json(new ErrorResponse("token_expired"), statusCode: StatusCodes.Status401Unauthorized);
			default:
				return Results.Json(new ErrorResponse("invalid_token"), statusCode: StatusCodes.Status401Unauthorized);
		}
	}

	// 없거나 이미 폐기된 토큰이어도 같은 응답 (토큰이 있었는지 알려 주지 않음)
	private static async Task<IResult> LogoutAsync(RefreshRequest req, AccountRepository repo)
	{
		if (string.IsNullOrEmpty(req.RefreshToken) == false)
			await repo.RevokeRefreshTokenAsync(TokenService.HashRefreshToken(req.RefreshToken));
		return Results.NoContent();
	}

	// 로그인: 새 Refresh를 DB에 저장하고 Access와 같이 돌려줌
	private static async Task<TokenResponse> IssueTokensAsync(long accountId, TokenService tokens, AccountRepository repo, AuthOptions options)
	{
		string refresh = TokenService.CreateRefreshToken();
		await repo.InsertRefreshTokenAsync(TokenService.HashRefreshToken(refresh), accountId, options.RefreshTokenDays);
		return BuildResponse(accountId, refresh, tokens, options);
	}

	private static TokenResponse BuildResponse(long accountId, string refresh, TokenService tokens, AuthOptions options)
		=> new(tokens.CreateAccessToken(accountId), tokens.AccessTokenSeconds, refresh, tokens.RefreshTokenSeconds,
			new GameServerAddress(options.GameServerHost, options.GameServerPort));
}
