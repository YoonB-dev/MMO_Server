# 줍기: 드랍 3개(골드 + 젤리 + 검)를 4명이 1초마다 줍기 키 -> 드랍마다 정확히 한 명만 가져감 (먼저 처리된 사람)
#   보던 4명 모두 S_PICKUP을 받음, 주운 사람만 S_CHANGE_GOLD / S_CHANGE_ITEM
#   끊고 다시 들어오면 골드 더미를 주운 한 명만 그 골드를, 나머지는 0 (저장 확인). 아이템도 각각 한 명에게만
#   --attack-test인 클라는 몬스터에게서 약 1.5m에 서고 드랍은 몬스터 자리 1m 안 -> 걸어가지 않아도 줍기 범위(2m + 여유 1m) 안
Set-GameData @('UPDATE data_monster_drop SET chance_permille = 1000')
Start-GameServer

Start-DummyClients -Name A -Count 4 -Arguments '--attack-test --pickup-test'
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1 -TimeoutSec 30
$gold = Get-LogNumber -Name server -Pattern 'dropped 3 \(gold (\d+)\)'
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 3 -TimeoutSec 10
Wait-LogCount -Name A -Pattern 'S_PICKUP ' -Expected 12			# 드랍 3개 x 보던 4명
Assert-LogCount -Name server -Pattern 'picked up drop' -Expected 3	# 같은 드랍을 두 번 주운 일 없음
Assert-LogCount -Name A -Pattern 'S_CHANGE_GOLD gold=\d+ gained=' -Expected 1		# 골드 더미는 한 명만
Assert-LogCount -Name A -Pattern "S_CHANGE_GOLD gold=$gold gained=$gold" -Expected 1
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=1001 ' -Expected 1			# 젤리, 검 각각 한 명만
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=2001 count=1 gained=1' -Expected 1

# 저장: 끊으면 나갈 때 저장 -> 같은 계정(같은 캐릭터)으로 다시 들어오면 그대로
Stop-DummyClients -Name A
Wait-LogCount -Name server -Pattern 'saved character on leave' -Expected 4
Start-DummyClients -Name B -Count 4 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 4
Assert-LogCount -Name B -Pattern "S_ENTER_GAME success=1 .* gold=$gold items=" -Expected 1
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* gold=0 items=' -Expected 3
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* items=.*1001x[12]' -Expected 1
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* items=.*2001x1' -Expected 1
Assert-ServerAlive
