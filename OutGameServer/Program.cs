using OutGameServer;
using OutGameServer.Auth;
using OutGameServer.Data;

// 아웃게임 서버: 게임 밖에서 하는 일. 지금은 가입 / 로그인 / 토큰 갱신 / 로그아웃 (/auth/*)
//   게임 서버(IOCP)는 여기서 발급한 Access Token(JWT)만 검증하고, 비밀번호는 전혀 모름
var builder = WebApplication.CreateBuilder(args);

// 설정 = appsettings.json (비밀 아닌 값) + user-secrets (DB 연결 문자열, JWT 비밀키 -> Setup-Secrets.ps1이 넣음)
//   user-secrets는 저장소 밖(%APPDATA%)에 있어서 git에 안 올라감. 개발 환경이 아니어도 읽도록 직접 추가
builder.Configuration.AddUserSecrets<Program>(optional: true);
AuthOptions options = AuthOptions.Load(builder.Configuration);	// 빠진 값이 있으면 이유와 함께 예외 -> 서버가 안 뜸

// 의존성 주입: 객체를 프레임워크에 등록해 두면, 엔드포인트가 매개변수로 필요할 때 넣어 줌 (전역 변수 대신)
//   전부 상태가 없어서(설정값만 들고 있음) 하나만 만들어 같이 씀 (Singleton)
builder.Services.AddSingleton(options);
builder.Services.AddSingleton<PasswordHasher>();
builder.Services.AddSingleton<TokenService>();
builder.Services.AddSingleton<AccountRepository>();

WebApplication app = builder.Build();
app.MapAuthEndpoints();
app.Run();
