using Dapper;
using MySqlConnector;

namespace OutGameServer.Data;

// player_character 테이블 (Database/Migrations/003 ~ 009): 로비에서 캐릭터 목록 / 생성
//   위치, HP, MP는 넣지 않음 -> DB 기본값(009)이 채움. 게임 중 값은 게임 서버가 저장
public sealed class CharacterRepository
{
	private const int MySqlDuplicateKey = 1062;

	private readonly string _connectionString;

	public CharacterRepository(AuthOptions options)
	{
		_connectionString = options.ConnectionString;
	}

	public sealed record CharacterRow(long CharacterId, string Nickname);

	public enum CreateResult { Ok, NicknameTaken, LimitReached }

	public async Task<IEnumerable<CharacterRow>> ListAsync(long accountId)
	{
		await using var conn = new MySqlConnection(_connectionString);
		return await conn.QueryAsync<CharacterRow>(
			"SELECT character_id AS CharacterId, nickname AS Nickname FROM player_character " +
			"WHERE account_id = @accountId ORDER BY character_id",
			new { accountId });
	}

	// 생성: 개수 확인 -> 추가를 한 트랜잭션으로
	//   계정 줄을 FOR UPDATE로 잠가서, 같은 계정의 생성 요청 두 개가 동시에 와도 하나씩 차례로 개수를 봄
	//   (잠그지 않으면 둘 다 "2개"를 보고 둘 다 추가 -> 4개가 됨)
	//   닉네임 중복은 UNIQUE 위반을 그대로 이용 (CreateAccountAsync와 같은 방식)
	public async Task<(CreateResult Result, long CharacterId)> CreateAsync(long accountId, string nickname, int maxCount)
	{
		await using var conn = new MySqlConnection(_connectionString);
		await conn.OpenAsync();
		await using var tx = await conn.BeginTransactionAsync();

		await conn.ExecuteScalarAsync<long>(
			"SELECT account_id FROM account WHERE account_id = @accountId FOR UPDATE", new { accountId }, tx);
		long count = await conn.ExecuteScalarAsync<long>(
			"SELECT COUNT(*) FROM player_character WHERE account_id = @accountId", new { accountId }, tx);
		if (count >= maxCount)
			return (CreateResult.LimitReached, 0);	// Commit 없이 나가면 tx가 정리되면서 되돌림 (잠금도 풀림)

		try
		{
			long characterId = await conn.ExecuteScalarAsync<long>(
				"INSERT INTO player_character (account_id, nickname) VALUES (@accountId, @nickname); SELECT LAST_INSERT_ID();",
				new { accountId, nickname }, tx);
			await tx.CommitAsync();
			return (CreateResult.Ok, characterId);
		}
		catch (MySqlException ex) when (ex.Number == MySqlDuplicateKey)
		{
			return (CreateResult.NicknameTaken, 0);
		}
	}
}
