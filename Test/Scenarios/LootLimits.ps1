# 줍기 거절: 인벤토리 칸이 없을 때, 너무 멀 때
#   A 혼자(--attack-test --pickup-test), 인벤토리 1칸 (--inventory-size 1)
#     골드는 칸을 안 써서 주워짐. 젤리와 검 중 먼저 주운 하나가 유일한 칸을 차지 -> 나머지는 칸이 없어서 못 주움 -> 수명이 지나 사라짐
#   C(--pickup-test, 공격 안 함): 시작 위치 (0, 0)에 서 있음, 슬라임과 약 21m -> 드랍은 보이지만 줍기 범위(2m + 여유 1m) 밖
Set-GameData @(
    'UPDATE data_monster_drop SET chance_permille = 1000',
    'UPDATE data_skill SET damage = 100 WHERE skill_id = 1')
Start-GameServer -Arguments '--drop-lifetime-sec 5 --inventory-size 1'

Start-DummyClients -Name C -Count 1 -Arguments '--pickup-test'
Wait-LogCount -Name C -Pattern 'S_ENTER_GAME success=1 .* inv=1 ' -Expected 1
Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --pickup-test'
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 2 -TimeoutSec 10		# 골드 + (젤리 또는 검)
Wait-LogCount -Name server -Pattern 'drops expired: 1' -Expected 1 -TimeoutSec 10	# 칸이 없어서 못 주운 하나
Assert-LogCount -Name A -Pattern 'S_CHANGE_GOLD ' -Expected 1
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM .* slots=0:' -Expected 1			# 아이템은 하나만, 유일한 칸(0번)에
Assert-LogCount -Name C -Pattern '\(drop gold=' -Expected 1						# C에게도 보임 (시야 30m 안)
Assert-LogCount -Name C -Pattern 'S_PICKUP ' -Expected 2							# 남이 줍는 건 봄
Assert-LogCount -Name C -Pattern 'S_CHANGE_(GOLD|ITEM) ' -Expected 0				# 하지만 C는 못 주움
Assert-ServerAlive
