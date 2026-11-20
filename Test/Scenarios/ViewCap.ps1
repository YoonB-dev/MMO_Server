# 시야 상한: 몰려 있어도 다른 플레이어는 상한까지만 보임 (몬스터는 상한 없음)
#   상한 5 (--max-visible-players 5), 20명이 모두 시작 위치 (0, 0)에 모임 -> 서로가 다 시야 안
#   한 명이 보는 객체 = 다른 플레이어 5 + 근처 슬라임(스폰 21 ~ 25m, 배회 중이라 2 ~ 3마리)
#   10초 통계의 view: avg가 5 이상 8 이하 (상한이 없으면 19 + 슬라임)
Start-GameServer -Arguments '--max-visible-players 5'

Start-DummyClients -Name A -Count 20 -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 20 -TimeoutSec 30
Wait-LogCount -Name server -Pattern '\[Room\] tick .* players 20,' -Expected 2 -TimeoutSec 30	# 20명이 다 들어온 뒤 10초 구간 하나가 끝날 때까지
$avg = Get-LogNumber -Name server -Pattern 'view: avg ([\d.]+) objects'
Assert-That ($avg -ge 5 -and $avg -le 8) "view avg with cap 5 is between 5 and 8 (got $avg)"
Assert-ServerAlive
