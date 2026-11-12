using System.Security.Cryptography;

namespace OutGameServer.Auth;

// 비밀번호 해시: PBKDF2-SHA256(비밀번호, salt, 반복 수)
//   일부러 느린 해시 (반복 수만큼 SHA-256을 다시 돌림) -> DB가 털려도 비밀번호를 하나씩 맞춰 보는 비용이 큼
//   salt: 계정마다 랜덤 16바이트 -> 같은 비밀번호라도 결과가 다르고, 미리 계산한 표로 역추적할 수 없음 (비밀은 아님)
public sealed class PasswordHasher
{
	public const int SaltSize = 16;
	public const int HashSize = 32;

	private readonly int _iterations;
	private readonly byte[] _dummySalt = RandomNumberGenerator.GetBytes(SaltSize);

	public PasswordHasher(AuthOptions options)
	{
		_iterations = options.Pbkdf2Iterations;
	}

	// 가입할 때: 새 salt를 만들고 해시. 반복 수도 같이 돌려줌 (계정마다 저장)
	public (byte[] Hash, byte[] Salt, int Iterations) Hash(string password)
	{
		byte[] salt = RandomNumberGenerator.GetBytes(SaltSize);
		return (Derive(password, salt, _iterations), salt, _iterations);
	}

	// 로그인할 때: 저장된 salt와 반복 수로 다시 해시해서 비교
	//   FixedTimeEquals: 앞에서부터 다른 바이트를 만나도 끝까지 비교 -> 걸린 시간으로 몇 바이트가 맞았는지 알 수 없음
	public bool Verify(string password, byte[] hash, byte[] salt, int iterations)
		=> CryptographicOperations.FixedTimeEquals(Derive(password, salt, iterations), hash);

	// 없는 아이디로 로그인할 때도 해시 한 번만큼 시간을 씀 -> 응답 시간으로 "있는 아이디인지" 알아낼 수 없게
	public void SpendSameTime(string password)
		=> Derive(password, _dummySalt, _iterations);

	private static byte[] Derive(string password, byte[] salt, int iterations)
		=> Rfc2898DeriveBytes.Pbkdf2(password, salt, iterations, HashAlgorithmName.SHA256, HashSize);
}
