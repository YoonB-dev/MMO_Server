# 인벤토리 칸: 한 칸이 가득 차면(젤리 50) 빈 칸에 새로 쌓고, 장비(검, max_stack 1)는 한 칸에 하나씩
#   젤리 60개가 한 번에 떨어짐 -> 한 칸 50 + 새 칸 10 / 검 2개 -> 두 칸에 1개씩 / 다시 들어와도 칸 배치 그대로
#   혼자 잡고 혼자 주움 (기본 공격 데미지를 올려서 빨리 잡음). 어느 걸 먼저 줍느냐에 따라 칸 번호는 달라짐 -> 번호는 \d+
Set-GameData @(
    'UPDATE data_monster_drop SET chance_permille = 1000',
    'UPDATE data_monster_drop SET min_count = 60, max_count = 60 WHERE item_id = 1001',
    'UPDATE data_monster_drop SET min_count = 2, max_count = 2 WHERE item_id = 2001',
    'UPDATE data_skill SET damage = 100 WHERE skill_id = 1')
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --pickup-test'
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 3 -TimeoutSec 30
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=1001 gained=60 slots=\d+:1001x50,\d+:1001x10$' -Expected 1
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=2001 gained=2 slots=\d+:2001x1,\d+:2001x1$' -Expected 1

# 저장: 끊으면 나갈 때 저장 -> 같은 계정(같은 캐릭터)으로 다시 들어오면 칸 4개 그대로
Stop-DummyClients -Name A
Wait-LogCount -Name server -Pattern 'saved character on leave' -Expected 1
Start-DummyClients -Name B -Count 1 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* inv=20 items=' -Expected 1
Assert-LogContains -Name B -Pattern 'items=.*:1001x50'
Assert-LogContains -Name B -Pattern 'items=.*:1001x10'
Assert-LogContains -Name B -Pattern 'items=.*:2001x1.*:2001x1'
Assert-ServerAlive
