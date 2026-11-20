# 아웃게임 서버 인증 흐름 (가입 -> 로그인 -> 토큰 갱신(회전) -> 재사용 감지 -> 로그아웃)
#   매번 새 아이디로 가입 (계정은 Migrations 테이블이라 지워지지 않음)
#   OutGameServer/Setup-Secrets.ps1을 한 번 실행해 둬야 함 (DB 연결, JWT 키)
Start-OutGameServer

$loginId = 't' + (Get-Date -Format 'MMddHHmmssfff')
$password = 'test-password-1'

# 1. 가입
$r = Invoke-AuthApi '/auth/register' @{ loginId = $loginId; password = $password }
Assert-That ($r.Status -eq 201) "register -> 201 (got $($r.Status))"
$r = Invoke-AuthApi '/auth/register' @{ loginId = $loginId; password = $password }
Assert-That ($r.Status -eq 409 -and $r.Body.error -eq 'login_id_taken') "register same id -> 409 login_id_taken (got $($r.Status))"
$r = Invoke-AuthApi '/auth/register' @{ loginId = 'ab'; password = $password }
Assert-That ($r.Status -eq 400 -and $r.Body.error -eq 'invalid_login_id') "register short id -> 400 invalid_login_id (got $($r.Status))"

# 2. 로그인 실패: 틀린 비밀번호와 없는 아이디가 같은 응답 (어느 아이디가 있는지 안 알려 줌)
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = 'wrong-password' }
Assert-That ($r.Status -eq 401 -and $r.Body.error -eq 'invalid_credentials') "login wrong password -> 401 (got $($r.Status))"
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId + 'x'; password = $password }
Assert-That ($r.Status -eq 401 -and $r.Body.error -eq 'invalid_credentials') "login unknown id -> 401 (got $($r.Status))"

# 3. 로그인 성공: Access JWT의 payload(claims) 확인
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = $password }
Assert-That ($r.Status -eq 200) "login -> 200 (got $($r.Status))"
$access1 = $r.Body.accessToken
$refresh1 = $r.Body.refreshToken
Assert-That ($r.Body.gameServer.port -eq 7777) "login response has game server address ($($r.Body.gameServer.host):$($r.Body.gameServer.port))"

$parts = $access1.Split('.')
Assert-That ($parts.Count -eq 3) "access token is a JWT (header.payload.signature)"
$payloadText = $parts[1].Replace('-', '+').Replace('_', '/')
$payloadText += '=' * ((4 - $payloadText.Length % 4) % 4)
$claims = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payloadText)) | ConvertFrom-Json
Assert-That ($claims.iss -eq 'MMO-Auth' -and $claims.aud -eq 'MMO-Game') "claims iss/aud = MMO-Auth/MMO-Game (got $($claims.iss)/$($claims.aud))"
Assert-That ($claims.sub -match '^\d+$') "claims sub is account id (got $($claims.sub))"
Assert-That (($claims.exp - $claims.iat) -eq 900) "access token lives 15 min (exp - iat = $($claims.exp - $claims.iat))"

# 4. 갱신: 새 토큰 쌍, Refresh가 바뀜 (회전)
$r = Invoke-AuthApi '/auth/refresh' @{ refreshToken = $refresh1 }
Assert-That ($r.Status -eq 200) "refresh -> 200 (got $($r.Status))"
$refresh2 = $r.Body.refreshToken
Assert-That ($refresh2 -and $refresh2 -ne $refresh1) "refresh rotates the refresh token"

# 5. 재사용 감지: 이미 쓴 refresh1을 다시 쓰면 401 + 그 계정의 Refresh 전부 폐기 -> refresh2도 401
$r = Invoke-AuthApi '/auth/refresh' @{ refreshToken = $refresh1 }
Assert-That ($r.Status -eq 401 -and $r.Body.error -eq 'token_reused') "reuse old refresh -> 401 token_reused (got $($r.Status) $($r.Body.error))"
$r = Invoke-AuthApi '/auth/refresh' @{ refreshToken = $refresh2 }
Assert-That ($r.Status -eq 401) "after reuse, the newer refresh is also revoked -> 401 (got $($r.Status))"

# 6. 다시 로그인 -> 로그아웃 -> 그 Refresh는 못 씀
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = $password }
$refresh3 = $r.Body.refreshToken
$r = Invoke-AuthApi '/auth/logout' @{ refreshToken = $refresh3 }
Assert-That ($r.Status -eq 204) "logout -> 204 (got $($r.Status))"
$r = Invoke-AuthApi '/auth/refresh' @{ refreshToken = $refresh3 }
Assert-That ($r.Status -eq 401) "refresh after logout -> 401 (got $($r.Status))"

Assert-LogContains -Name outgame -Pattern 'refresh token reused: revoked all tokens of account'
Assert-LogNotContains -Name outgame -Pattern ([regex]::Escape($password))	# 비밀번호는 로그에 안 남음
Assert-ProcessAlive -Name outgame
