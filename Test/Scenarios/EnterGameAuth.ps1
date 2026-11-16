# 게임 서버 입장 토큰(JWT) + 캐릭터 검증
#   정상: DummyClient가 아웃게임 서버에서 로그인 -> 로비에서 캐릭터를 고름(없으면 만듦) -> 토큰 + 캐릭터 번호로 입장
#   공격: 위조/다른 키/서명 없음/만료/빈 토큰은 S_ENTER_GAME success=0 + error (연결은 유지, 서버는 계속 동작)
#         없는 캐릭터 번호, 다른 계정의 캐릭터 번호도 거절 (둘은 같은 응답)
#   error 값은 Enum.proto EnterGameError: 1 = INVALID_TOKEN, 2 = TOKEN_EXPIRED, 5 = INVALID_CHARACTER
Start-GameServer

# 1. 정상 입장 2명 (가입 -> 로그인 -> 로비 -> 토큰으로 입장)
Start-DummyClients -Name valid -Count 2 -Arguments '--no-ping'
Wait-LogCount -Name valid -Pattern 'S_ENTER_GAME success=1 .* name=Bot[0-9a-f]{16}' -Expected 2 -TimeoutSec 20
Assert-LogCount -Name server -Pattern '\[DB\] loaded character: account' -Expected 2

# 공격용 진짜 토큰 하나 (새 계정) + 그 계정의 캐릭터 (로비 API로 만듦. 게임 서버는 캐릭터를 만들지 않음)
$loginId = 'auth' + $(Get-RunStamp)
$null = Invoke-AuthApi '/auth/register' @{ loginId = $loginId; password = 'test-password-1' }
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = 'test-password-1' }
$realToken = $r.Body.accessToken
$parts = $realToken.Split('.')
$payloadText = $parts[1].Replace('-', '+').Replace('_', '/')
$payloadText += '=' * ((4 - $payloadText.Length % 4) % 4)
$accountId = ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payloadText)) | ConvertFrom-Json).sub
$r = Invoke-AuthApi '/characters' @{ nickname = 'Auth' + $(Get-RunStamp) } -Token $realToken
Assert-That ($r.Status -eq 201) "create character for auth account -> 201 (got $($r.Status))"
$characterId = $r.Body.characterId

# 다른 계정 하나 (캐릭터 없음). 이 계정의 토큰으로 위 캐릭터에 들어가려는 공격용
#   위 계정은 control이 접속해 있어서, 같은 계정으로 시도하면 중복 로그인 처리(control을 끊음)가 끼어듦 -> 다른 계정을 씀
$otherLogin = 'other' + $(Get-RunStamp)
$null = Invoke-AuthApi '/auth/register' @{ loginId = $otherLogin; password = 'test-password-1' }
$otherToken = (Invoke-AuthApi '/auth/login' @{ loginId = $otherLogin; password = 'test-password-1' }).Body.accessToken

# 2. 대조군: 테스트가 직접 만든 토큰도 진짜 키로 서명하면 통과 (아래 실패들이 New-TestJwt 탓이 아님을 확인)
Start-DummyClients -Name control -Count 1 -Arguments "--no-ping --token $(New-TestJwt -Sub $accountId) --character-id $characterId"
Wait-LogCount -Name control -Pattern 'S_ENTER_GAME success=1 .* name=Auth' -Expected 1

# 공격 토큰들: 하나씩 보내고 결과를 기다린 뒤 다음 것 (서버 로그는 여러 IOCP 스레드가 동시에 찍으면 한 줄에 섞일 수 있어서)
$otherKey = [Convert]::ToBase64String((1..32 | ForEach-Object { [byte](Get-Random -Maximum 256) }))
$forgedPayload = (New-TestJwt -Sub '999999').Split('.')[1]
$attacks = @(
    # 위조: payload의 sub만 다른 계정으로 바꾸고 서명은 그대로 -> 서명 불일치
    @{ Group = 'forged';   Token = "$($parts[0]).$forgedPayload.$($parts[2])";                 Error = 1 }
    # 다른 키로 서명 (키를 모르는 사람이 만든 토큰)
    @{ Group = 'otherkey'; Token = (New-TestJwt -Sub $accountId -SecretBase64 $otherKey);     Error = 1 }
    # alg none (서명 없는 토큰을 받아 주는 구현을 노리는 공격)
    @{ Group = 'algnone';  Token = (New-TestJwt -Sub $accountId -Alg none);                   Error = 1 }
    # 만료: 진짜 키로 서명했지만 1분 전에 만료 (허용 오차 30초 밖)
    @{ Group = 'expired';  Token = (New-TestJwt -Sub $accountId -ExpOffsetSec -60);           Error = 2 }
    # 빈 토큰 (토큰을 안 보내는 옛 클라이언트)
    @{ Group = 'empty';    Token = '""';                                                      Error = 1 }
    # 진짜 토큰 + 없는 캐릭터 번호
    @{ Group = 'nochar';    Token = $otherToken; Character = 999999999;                       Error = 5 }
    # 진짜 토큰 + 다른 계정의 캐릭터 번호 (위와 같은 응답: 그 번호의 캐릭터가 있는지 알려 주지 않음)
    @{ Group = 'otherchar'; Token = $otherToken; Character = $characterId;                    Error = 5 }
)
# 변수 이름 주의: 시나리오는 Run-Test 안에서 실행돼서 $name 같은 이름을 쓰면 Run-Test의 변수를 덮어씀
foreach ($attack in $attacks)
{
    $attackCharacter = if ($attack.Character) { $attack.Character } else { $characterId }
    Start-DummyClients -Name $attack.Group -Count 1 -Arguments "--no-ping --token $($attack.Token) --character-id $attackCharacter"
    Wait-LogCount -Name $attack.Group -Pattern "S_ENTER_GAME success=0 .*error=$($attack.Error)" -Expected 1
}

Assert-LogCount -Name server -Pattern '\[Enter\] rejected: error 1' -Expected 4
Assert-LogCount -Name server -Pattern '\[Enter\] rejected: error 2' -Expected 1
Assert-LogCount -Name server -Pattern '\[Enter\] rejected: error 5' -Expected 2
Assert-LogCount -Name server -Pattern '\[DB\] loaded character: account' -Expected 3	# valid 2 + control 1 (실패한 토큰은 DB까지 안 감, 캐릭터 실패는 줄이 없음)
Assert-LogCount -Name server -Pattern '\[DB\] no such character for account' -Expected 2

# 실패해도 연결은 유지 (토큰을 갱신해서 다시 보낼 수 있게)
Wait-Seconds 1
foreach ($attack in $attacks)
{
    Assert-LogNotContains -Name $attack.Group -Pattern 'Disconnected'
}
Assert-ServerAlive
