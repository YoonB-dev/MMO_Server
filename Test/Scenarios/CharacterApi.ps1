# 로비 API: 캐릭터 목록 / 생성 (토큰 검사, 닉네임 규칙, 중복, 계정당 3개)
#   매번 새 아이디와 새 닉네임 (캐릭터는 Migrations 테이블이라 지워지지 않음)
#   닉네임 규칙: 영문/숫자/완성형 한글, 2글자 이상, 영문/숫자 1 + 한글 2 합계 20 이하, Player+숫자는 예약
Start-OutGameServer

$stamp = Get-Date -Format 'MMddHHmmssfff'	# 13자리 숫자
$loginId = 'c' + $stamp
$password = 'test-password-1'

# 1. 가입 -> 로그인 -> 빈 목록
$r = Invoke-AuthApi '/auth/register' @{ loginId = $loginId; password = $password }
Assert-That ($r.Status -eq 201) "register -> 201 (got $($r.Status))"
$r = Invoke-AuthApi '/auth/login' @{ loginId = $loginId; password = $password }
Assert-That ($r.Status -eq 200) "login -> 200 (got $($r.Status))"
$token = $r.Body.accessToken

$r = Invoke-AuthApi '/characters' -Method Get -Token $token
Assert-That ($r.Status -eq 200 -and @($r.Body).Count -eq 0) "list new account -> 200 empty (got $($r.Status), $(@($r.Body).Count) items)"

# 2. 토큰 검사
$r = Invoke-AuthApi '/characters' -Method Get
Assert-That ($r.Status -eq 401 -and $r.Body.error -eq 'invalid_token') "list without token -> 401 (got $($r.Status))"
$r = Invoke-AuthApi '/characters' -Method Get -Token ($token.Substring(0, $token.Length - 2) + 'xx')
Assert-That ($r.Status -eq 401 -and $r.Body.error -eq 'invalid_token') "list with tampered token -> 401 (got $($r.Status))"
$r = Invoke-AuthApi '/characters' @{ nickname = 'H' + $stamp }
Assert-That ($r.Status -eq 401) "create without token -> 401 (got $($r.Status))"

# 3. 닉네임 규칙 위반
foreach ($bad in @('a', 'ab_c', 'ab c', ('가' * 11), 'ㅋㅋㅋ'))
{
    $r = Invoke-AuthApi '/characters' @{ nickname = $bad } -Token $token
    Assert-That ($r.Status -eq 400 -and $r.Body.error -eq 'invalid_nickname') "create '$bad' -> 400 invalid_nickname (got $($r.Status) $($r.Body.error))"
}
foreach ($reserved in @('Player5', 'player12'))
{
    $r = Invoke-AuthApi '/characters' @{ nickname = $reserved } -Token $token
    Assert-That ($r.Status -eq 400 -and $r.Body.error -eq 'nickname_reserved') "create '$reserved' -> 400 nickname_reserved (got $($r.Status) $($r.Body.error))"
}

# 4. 생성: 중복(대소문자 무시), 한글 이름, 계정당 3개
$first = 'H' + $stamp
$r = Invoke-AuthApi '/characters' @{ nickname = $first } -Token $token
Assert-That ($r.Status -eq 201 -and $r.Body.characterId -gt 0 -and $r.Body.nickname -eq $first) "create '$first' -> 201 (got $($r.Status))"
$r = Invoke-AuthApi '/characters' @{ nickname = $first.ToLower() } -Token $token
Assert-That ($r.Status -eq 409 -and $r.Body.error -eq 'nickname_taken') "create same name in lower case -> 409 nickname_taken (got $($r.Status) $($r.Body.error))"

$korean = '용사' + $stamp	# 한글 2글자(4) + 숫자 13 = 17
$r = Invoke-AuthApi '/characters' @{ nickname = $korean } -Token $token
Assert-That ($r.Status -eq 201 -and $r.Body.nickname -eq $korean) "create hangul name -> 201 (got $($r.Status) $($r.Body.error))"
$r = Invoke-AuthApi '/characters' @{ nickname = 'N' + $stamp } -Token $token
Assert-That ($r.Status -eq 201) "create third -> 201 (got $($r.Status))"
$r = Invoke-AuthApi '/characters' @{ nickname = 'M' + $stamp } -Token $token
Assert-That ($r.Status -eq 409 -and $r.Body.error -eq 'character_limit') "create fourth -> 409 character_limit (got $($r.Status) $($r.Body.error))"

# 5. 목록: 만든 순서대로 3개
$r = Invoke-AuthApi '/characters' -Method Get -Token $token
$names = @($r.Body | ForEach-Object { $_.nickname })
Assert-That ($r.Status -eq 200 -and $names.Count -eq 3 -and $names[0] -eq $first -and $names[1] -eq $korean) "list -> 3 characters in order (got $($names.Count))"
