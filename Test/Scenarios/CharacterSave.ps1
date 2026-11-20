# 캐릭터 저장: 주기 저장 -> 중복 로그인(옛 세션 끊고 저장 뒤 불러옴) -> 재접속해도 위치 유지
#   서버를 --save-interval-sec 2로 띄워서 60초를 기다리지 않음
#   B, C는 A와 같은 계정 (-AccountOf A)
Start-GameServer -Arguments '--save-interval-sec 2'

# 1. A 입장 -> 오른쪽으로 약 1초 이동 후 멈춤 -> 그 뒤 주기 저장이 한 번 더 돌아야 함
Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --move-test'
Wait-LogCount -Name A -Pattern 'MOVE_TEST ok' -Expected 1
$savesBefore = Get-LogMatchCount -Name server -Pattern '\[DB\] saved 1 character\(s\)'
Wait-LogCount -Name server -Pattern '\[DB\] saved 1 character\(s\)' -Expected ($savesBefore + 1) -TimeoutSec 5
$movedX = Get-LogNumber -Name A -Pattern 'MOVE_TEST ok x=([-\d.]+)'

# 2. 중복 로그인: B가 같은 계정으로 입장 -> 서버가 A를 끊고, A의 저장이 끝난 뒤 B가 불러옴 -> B는 A가 멈춘 자리에서 시작
Start-DummyClients -Name B -Count 1 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 1 -TimeoutSec 15
Assert-LogContains -Name A -Pattern '\] Disconnected'
Assert-LogCount -Name server -Pattern '\[Login\] duplicate login: account' -Expected 1
Assert-LogCount -Name server -Pattern '\[DB\] saved character on leave' -Expected 1
$bX = Get-LogNumber -Name B -Pattern 'S_ENTER_GAME success=1 .*pos=\(([-\d.]+),'
Assert-That ($null -ne $bX -and [Math]::Abs($bX - $movedX) -lt 0.01) "duplicate login: B starts where A stopped (A x=$movedX, B x=$bX)"

# 3. 재접속: B 종료 -> 나갈 때 저장 -> C가 같은 계정으로 입장해도 같은 자리
Stop-DummyClients -Name B
Wait-LogCount -Name server -Pattern '\[DB\] saved character on leave' -Expected 2
Start-DummyClients -Name C -Count 1 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name C -Pattern 'S_ENTER_GAME success=1' -Expected 1 -TimeoutSec 15
$cX = Get-LogNumber -Name C -Pattern 'S_ENTER_GAME success=1 .*pos=\(([-\d.]+),'
Assert-That ($null -ne $cX -and [Math]::Abs($cX - $movedX) -lt 0.01) "reconnect: C starts where A stopped (A x=$movedX, C x=$cX)"
Assert-LogCount -Name server -Pattern '\[Login\] duplicate login' -Expected 1	# B는 끊긴 뒤라 중복 아님

Assert-LogNotContains -Name server -Pattern 'failed to (save|load) character'
Assert-ServerAlive
