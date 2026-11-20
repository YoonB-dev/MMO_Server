# 채팅: 주변(시야) / 방 전체, 서버 검사(빈 문자열, 100글자 초과, 제어 문자, 0.5초 쿨타임), 보낸 사람 닉네임
#   B는 한글 닉네임 캐릭터 -> 이름이 DB(UTF-16 읽기) -> 서버(UTF-8 변환) -> 클라까지 바이트 그대로 오는지도 확인
#   A, B 둘 다 새 캐릭터라 시작 위치 (0, 0) -> 서로 시야 안
#   (35m 밖 사람이 주변 채팅을 못 받는지는 확인 안 함: 새 캐릭터는 모두 같은 자리에서 시작)
Start-GameServer

# B: 한글 닉네임 캐릭터를 로비 API로 직접 만들고 --token / --character-id로 입장
$korLogin = 'kor' + (Get-RunStamp)
$null = Invoke-AuthApi '/auth/register' @{ loginId = $korLogin; password = 'test-password-1' }
$korToken = (Invoke-AuthApi '/auth/login' @{ loginId = $korLogin; password = 'test-password-1' }).Body.accessToken
$korName = '용사' + (Get-RunStamp)		# 한글 2글자(4) + 숫자 10 = 14
$r = Invoke-AuthApi '/characters' @{ nickname = $korName } -Token $korToken
Assert-That ($r.Status -eq 201) "create hangul character -> 201 (got $($r.Status))"
$korHex = -join ([Text.Encoding]::UTF8.GetBytes($korName) | ForEach-Object { $_.ToString('x2') })

Start-DummyClients -Name B -Count 1 -Arguments "--no-ping --chat-reply --token $korToken --character-id $($r.Body.characterId)"
Wait-LogCount -Name B -Pattern "S_ENTER_GAME success=1 .*name_hex=$korHex" -Expected 1	# 한글 이름이 깨지지 않고 옴

# A: 거절 4종 + 주변 채팅
Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --chat-test'
Wait-LogCount -Name B -Pattern 'S_CHAT channel=0 from=Bot[0-9a-f]{16} .*text=hello' -Expected 1		# 주변 채팅이 B에게, 보낸 사람 이름 포함
Wait-LogCount -Name A -Pattern 'S_CHAT channel=0 from=Bot[0-9a-f]{16} .*text=hello' -Expected 1		# 보낸 사람도 받음
Wait-LogCount -Name A -Pattern "S_CHAT channel=1 .*from_hex=$korHex .*text=reply" -Expected 1		# 방 전체 채팅 + 한글 보낸 사람 이름
Wait-LogCount -Name server -Pattern 'reject chat: .* reason (empty|too_long|control_char|cooldown)' -Expected 4
Assert-LogCount -Name server -Pattern 'reject chat: .* reason cooldown' -Expected 1
Assert-LogNotContains -Name B -Pattern 'text=again'		# 쿨타임으로 거절된 건 아무에게도 안 감
Assert-ServerAlive
