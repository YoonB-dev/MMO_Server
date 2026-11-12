using Dapper;
using MySqlConnector;

namespace OutGameServer.Data;

// account, refresh_token 테이블 읽기/쓰기 (Database/Migrations/001, 002)
//   Dapper: SQL은 직접 쓰고, 결과 줄을 C# 객체로 옮기는 일만 해 주는 가벼운 라이브러리
//   요청마다 연결을 열고 닫음 -> MySqlConnector가 내부에서 연결을 풀로 재사용해서 실제로는 빠름
//   시각은 전부 UTC (UTC_TIMESTAMP)
public sealed class AccountRepository
{
	private const int MySqlDuplicateKey = 1062;

	private readonly string _connectionString;

	public AccountRepository(AuthOptions options)
	{
		_connectionString = options.ConnectionString;
	}

	public sealed record AccountRow(long AccountId, byte[] PasswordHash, byte[] PasswordSalt, int PasswordIterations);

	public enum RotateResult { Ok, NotFound, Expired, Reused }

	// refresh_token 한 줄의 상태 (MySQL의 IS NOT NULL / 비교 결과는 0 / 1 정수로 옴)
	private sealed record RefreshRow(long AccountId, long Revoked, long Expired);

	// 가입. 같은 아이디가 이미 있으면 null (UNIQUE 위반을 그대로 이용 -> 확인하고 넣는 사이에 다른 요청이 끼어들 틈이 없음)
	public async Task<long?> CreateAccountAsync(string loginId, byte[] hash, byte[] salt, int iterations)
	{
		// await using: 블록이 끝나면 연결을 자동으로 닫음 (C++의 RAII, DBConnectionGuard와 같은 역할)
		await using var conn = new MySqlConnection(_connectionString);
		try
		{
			return await conn.ExecuteScalarAsync<long>(
				"INSERT INTO account (login_id, password_hash, password_salt, password_iterations) " +
				"VALUES (@loginId, @hash, @salt, @iterations); SELECT LAST_INSERT_ID();",
				new { loginId, hash, salt, iterations });
		}
		catch (MySqlException ex) when (ex.Number == MySqlDuplicateKey)
		{
			return null;
		}
	}

	public async Task<AccountRow?> FindAccountAsync(string loginId)
	{
		await using var conn = new MySqlConnection(_connectionString);
		return await conn.QuerySingleOrDefaultAsync<AccountRow>(
			"SELECT account_id AS AccountId, password_hash AS PasswordHash, password_salt AS PasswordSalt, " +
			"password_iterations AS PasswordIterations FROM account WHERE login_id = @loginId",
			new { loginId });
	}

	public async Task InsertRefreshTokenAsync(byte[] tokenHash, long accountId, int days)
	{
		await using var conn = new MySqlConnection(_connectionString);
		await conn.ExecuteAsync(
			"INSERT INTO refresh_token (token_hash, account_id, expires_at) " +
			"VALUES (@tokenHash, @accountId, DATE_ADD(UTC_TIMESTAMP(), INTERVAL @days DAY))",
			new { tokenHash, accountId, days });
	}

	// 회전: 옛 토큰을 폐기하고 새 토큰을 저장 (한 트랜잭션)
	//   이미 폐기된 토큰이 오면 = 누가 옛 토큰을 다시 씀 (도난 의심) -> 그 계정의 살아 있는 토큰을 전부 폐기
	//   FOR UPDATE: 같은 토큰으로 동시에 두 요청이 와도 하나만 회전되게 그 줄을 잠금
	public async Task<(RotateResult Result, long AccountId)> RotateRefreshTokenAsync(byte[] oldHash, byte[] newHash, int days)
	{
		await using var conn = new MySqlConnection(_connectionString);
		await conn.OpenAsync();
		await using var tx = await conn.BeginTransactionAsync();

		RefreshRow? row = await conn.QuerySingleOrDefaultAsync<RefreshRow>(
			"SELECT account_id AS AccountId, revoked_at IS NOT NULL AS Revoked, expires_at <= UTC_TIMESTAMP() AS Expired " +
			"FROM refresh_token WHERE token_hash = @oldHash FOR UPDATE",
			new { oldHash }, tx);

		if (row == null)
			return (RotateResult.NotFound, 0);

		if (row.Revoked != 0)
		{
			await conn.ExecuteAsync(
				"UPDATE refresh_token SET revoked_at = UTC_TIMESTAMP() WHERE account_id = @accountId AND revoked_at IS NULL",
				new { accountId = row.AccountId }, tx);
			await tx.CommitAsync();
			return (RotateResult.Reused, row.AccountId);
		}

		if (row.Expired != 0)
			return (RotateResult.Expired, row.AccountId);

		await conn.ExecuteAsync(
			"UPDATE refresh_token SET revoked_at = UTC_TIMESTAMP(), replaced_by = @newHash WHERE token_hash = @oldHash",
			new { oldHash, newHash }, tx);
		await conn.ExecuteAsync(
			"INSERT INTO refresh_token (token_hash, account_id, expires_at) " +
			"VALUES (@newHash, @accountId, DATE_ADD(UTC_TIMESTAMP(), INTERVAL @days DAY))",
			new { newHash, accountId = row.AccountId, days }, tx);
		await tx.CommitAsync();
		return (RotateResult.Ok, row.AccountId);
	}

	// 로그아웃: 그 토큰만 폐기 (없거나 이미 폐기됐어도 그냥 끝)
	public async Task RevokeRefreshTokenAsync(byte[] tokenHash)
	{
		await using var conn = new MySqlConnection(_connectionString);
		await conn.ExecuteAsync(
			"UPDATE refresh_token SET revoked_at = UTC_TIMESTAMP() WHERE token_hash = @tokenHash AND revoked_at IS NULL",
			new { tokenHash });
	}
}
