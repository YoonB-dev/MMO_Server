# 게임 서버 입장 토큰(JWT) 검증
#   정상: DummyClient가 아웃게임 서버에서 로그인한 토큰으로 입장 -> 캐릭터를 DB에서 불러오거나 새로 만듦
#   공격: 위조/다른 키/서명 없음/만료/빈 토큰은 S_ENTER_GAME success=0 + error (연결은 유지, 서버는 계속 동작)
#   error 값은 Enum.proto EnterGameError: 1 = INVALID_TOKEN, 2 = TOKEN_EXPIRED
Start-GameServer

# 1. 정상 입장 2명 (가입 -> 로그인 -> 토큰으로 입장)
Start-DummyClients -Name valid -Count 2 -Arguments '--no-ping'
Wait-LogCount -Name valid -Pattern 'S_ENTER_GAME success=1' -Expected 2 -TimeoutSec 20
Assert-LogCount -Name server -Pattern '\[DB\] loaded character: account' -Expected 2

# 공격용 진짜 토큰 하나 (새 계정)
$loginId = 'auth' + $(Get-RunStamp)
$null = Invoke-AuthApi '/auth/register' @{ loginId = $loginId; password = 'test-password-1' }
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = 'test-password-1' }
$realToken = $r.Body.accessToken
$parts = $realToken.Split('.')
$payloadText = $parts[1].Replace('-', '+').Replace('_', '/')
$payloadText += '=' * ((4 - $payloadText.Length % 4) % 4)
$accountId = ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payloadText)) | ConvertFrom-Json).sub

# 2. 대조군: 테스트가 직접 만든 토큰도 진짜 키로 서명하면 통과 (아래 실패들이 New-TestJwt 탓이 아님을 확인)
Start-DummyClients -Name control -Count 1 -Arguments "--no-ping --token $(New-TestJwt -Sub $accountId)"
Wait-LogCount -Name control -Pattern 'S_ENTER_GAME success=1' -Expected 1

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
)
# 변수 이름 주의: 시나리오는 Run-Test 안에서 실행돼서 $name 같은 이름을 쓰면 Run-Test의 변수를 덮어씀
foreach ($attack in $attacks)
{
    Start-DummyClients -Name $attack.Group -Count 1 -Arguments "--no-ping --token $($attack.Token)"
    Wait-LogCount -Name $attack.Group -Pattern "S_ENTER_GAME success=0 .*error=$($attack.Error)" -Expected 1
}

Assert-LogCount -Name server -Pattern '\[Enter\] rejected: error 1' -Expected 4
Assert-LogCount -Name server -Pattern '\[Enter\] rejected: error 2' -Expected 1
Assert-LogCount -Name server -Pattern '\[DB\] loaded character: account' -Expected 3	# valid 2 + control 1 (실패한 토큰은 DB까지 안 감)

# 실패해도 연결은 유지 (토큰을 갱신해서 다시 보낼 수 있게)
Wait-Seconds 1
foreach ($attack in $attacks)
{
    Assert-LogNotContains -Name $attack.Group -Pattern 'Disconnected'
}
Assert-ServerAlive
