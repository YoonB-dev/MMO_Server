using System.Text;
using System.Text.RegularExpressions;
using OutGameServer.Auth;
using OutGameServer.Data;
using static OutGameServer.Auth.AuthEndpoints;	// ErrorResponse 같이 씀

namespace OutGameServer.Characters;

// /characters 엔드포인트 (로비: 로그인 후, 게임 입장 전). 전부 Access Token 필요
//   GET  /characters  -> 200 [{characterId, nickname}]
//   POST /characters  -> 201 / 400 invalid_nickname, nickname_reserved / 409 nickname_taken, character_limit
//   401 invalid_token: 토큰 없음, 서명/발급자/대상이 틀림, 만료
public static partial class CharacterEndpoints
{
	public sealed record CreateCharacterRequest(string? Nickname);

	private const int MaxCharactersPerAccount = 3;
	private const int NicknameMaxWeight = 20;	// 영문/숫자 1, 한글 2 (DB CHECK 006과 같은 규칙)

	[GeneratedRegex("^[A-Za-z0-9가-힣]{2,20}$")]	// 영문, 숫자, 완성형 한글(가~힣)
	private static partial Regex NicknamePattern();

	[GeneratedRegex("^player[0-9]+$", RegexOptions.IgnoreCase)]	// 기존 테스트 계정 이름(Player + 계정 번호)과 겹치지 않게 예약
	private static partial Regex ReservedPattern();

	public static void MapCharacterEndpoints(this WebApplication app)
	{
		RouteGroupBuilder characters = app.MapGroup("/characters");
		characters.MapGet("", ListAsync);
		characters.MapPost("", CreateAsync);
	}

	private static async Task<IResult> ListAsync(HttpRequest request, TokenService tokens, CharacterRepository repo)
	{
		long? accountId = await AuthenticateAsync(request, tokens);
		if (accountId == null)
			return InvalidToken();
		return Results.Ok(await repo.ListAsync(accountId.Value));
	}

	private static async Task<IResult> CreateAsync(CreateCharacterRequest req, HttpRequest request, TokenService tokens, CharacterRepository repo)
	{
		long? accountId = await AuthenticateAsync(request, tokens);
		if (accountId == null)
			return InvalidToken();

		// NFC: 맥 등에서 한글이 자모로 쪼개진 모양(NFD)으로 와도 완성형으로 합친 뒤 검사 / 저장
		string? nickname = req.Nickname?.Normalize(NormalizationForm.FormC);
		string? error = CheckNickname(nickname);
		if (error != null)
			return Results.BadRequest(new ErrorResponse(error));

		var (result, characterId) = await repo.CreateAsync(accountId.Value, nickname!, MaxCharactersPerAccount);
		switch (result)
		{
			case CharacterRepository.CreateResult.Ok:
				Console.WriteLine($"[Lobby] account {accountId} created character {characterId}");	// 닉네임은 안 찍음 (콘솔 한글)
				return Results.Created($"/characters/{characterId}", new CharacterRepository.CharacterRow(characterId, nickname!));
			case CharacterRepository.CreateResult.NicknameTaken:
				return Results.Conflict(new ErrorResponse("nickname_taken"));
			default:
				return Results.Conflict(new ErrorResponse("character_limit"));
		}
	}

	// 통과하면 null. 형식(문자, 2~20자) -> 길이 가중치(한글 2) -> 예약 이름
	private static string? CheckNickname(string? nickname)
	{
		if (nickname == null || NicknamePattern().IsMatch(nickname) == false)
			return "invalid_nickname";

		int weight = 0;
		foreach (char c in nickname)
			weight += (c >= '가' && c <= '힣') ? 2 : 1;
		if (weight > NicknameMaxWeight)
			return "invalid_nickname";

		if (ReservedPattern().IsMatch(nickname))
			return "nickname_reserved";
		return null;
	}

	// "Authorization: Bearer <token>" 헤더에서 토큰을 꺼내 검증
	private static async Task<long?> AuthenticateAsync(HttpRequest request, TokenService tokens)
	{
		string? header = request.Headers.Authorization;
		if (header == null || header.StartsWith("Bearer ", StringComparison.Ordinal) == false)
			return null;
		return await tokens.ValidateAccessTokenAsync(header["Bearer ".Length..]);
	}

	private static IResult InvalidToken()
		=> Results.Json(new ErrorResponse("invalid_token"), statusCode: StatusCodes.Status401Unauthorized);
}
