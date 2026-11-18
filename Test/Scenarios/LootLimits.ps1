# 줍기 거절: 인벤토리가 넘칠 때, 너무 멀 때
#   젤리 max_stack = 1, 젤리 드랍 개수는 항상 2 -> 아무도 못 주움 (지금 0개 + 2 > 1) -> 수명이 지나 사라짐
#   C(--pickup-test, 공격 안 함): 시작 위치 (0, 0)에 서 있음, 슬라임과 약 21m -> 드랍은 보이지만 줍기 범위(2m + 여유 1m) 밖
#   A 4명(--attack-test --pickup-test)이 슬라임을 잡고 줍기 키 -> 골드, 검만 주워짐
Set-GameData @(
    'UPDATE data_monster_drop SET chance_permille = 1000',
    'UPDATE data_monster_drop SET min_count = 2, max_count = 2 WHERE item_id = 1001',
    'UPDATE data_item SET max_stack = 1 WHERE item_id = 1001')
Start-GameServer -Arguments '--drop-lifetime-sec 5'

Start-DummyClients -Name C -Count 1 -Arguments '--pickup-test'
Wait-LogCount -Name C -Pattern 'S_ENTER_GAME success=1' -Expected 1
Start-DummyClients -Name A -Count 4 -Arguments '--attack-test --pickup-test'
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 2 -TimeoutSec 10		# 골드, 검
Wait-LogCount -Name server -Pattern 'drops expired: 1' -Expected 1 -TimeoutSec 10	# 젤리 (2개 > max_stack 1)
Assert-LogCount -Name server -Pattern 'picked up drop .* \(item 1001 ' -Expected 0
Assert-LogCount -Name C -Pattern 'drop item=1001 x2' -Expected 1		# C에게도 보임 (시야 30m 안)
Assert-LogCount -Name C -Pattern 'S_PICKUP ' -Expected 2					# 남이 줍는 건 봄
Assert-LogCount -Name C -Pattern 'S_CHANGE_(GOLD|ITEM) ' -Expected 0		# 하지만 C는 못 주움
Assert-ServerAlive
