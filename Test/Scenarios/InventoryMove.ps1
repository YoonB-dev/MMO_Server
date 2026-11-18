# 인벤토리 칸 옮기기: 옮기기 / 바꾸기 / 합치기 / 거절 3종, 그리고 트랜잭션 저장(빈 칸이 된 줄 삭제)
#   InventorySlots처럼 젤리 60개(50 + 10)와 검 2개를 주운 뒤, DummyClient(--move-item-test)가 옮기기 묶음을 보냄
#     b(젤리 10) -> 19(빈 칸) 옮기기 / c(검) -> a(젤리 50) 바꾸기 / c(젤리 50) -> 19(젤리 10) 합치기 = 19는 50, c는 10
#     a -> a (같은 칸), b -> a (빈 칸에서), 99 -> 0 (범위 밖) 은 거절
#   다시 들어오면: 19번 칸 젤리 50, 젤리 10 한 칸, 검 두 칸 = 칸 4개 (젤리 60, 검 2개 그대로)
#     옮기기로 빈 칸이 된 b의 옛 줄이 DB에서 지워졌어야 칸이 4개 (안 지워지면 5개)
Set-GameData @(
    'UPDATE data_monster_drop SET chance_permille = 1000',
    'UPDATE data_monster_drop SET min_count = 60, max_count = 60 WHERE item_id = 1001',
    'UPDATE data_monster_drop SET min_count = 2, max_count = 2 WHERE item_id = 2001',
    'UPDATE data_skill SET damage = 100 WHERE skill_id = 1')
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --pickup-test --move-item-test'
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 3 -TimeoutSec 30
Wait-LogCount -Name A -Pattern 'MOVE_ITEM_TEST sent' -Expected 1
Wait-LogCount -Name A -Pattern 'S_INVENTORY_SLOTS ' -Expected 5		# 옮기기 3 + 되돌리기 2 (범위 밖은 되돌릴 칸이 없어서 응답 없음)
Wait-LogCount -Name server -Pattern 'reject item move: .* same slot' -Expected 1
Wait-LogCount -Name server -Pattern 'reject item move: .* empty slot' -Expected 1
Wait-LogCount -Name server -Pattern 'reject item move: .* out of range' -Expected 1
Assert-LogContains -Name A -Pattern 'S_INVENTORY_SLOTS slots=\d+:0x0,19:1001x10$'		# 1. 옮기기
Assert-LogContains -Name A -Pattern 'S_INVENTORY_SLOTS slots=\d+:1001x50,\d+:2001x1$'	# 2. 바꾸기
Assert-LogContains -Name A -Pattern 'S_INVENTORY_SLOTS slots=\d+:1001x10,19:1001x50$'	# 3. 합치기 (넘친 10은 원래 칸에)

# 저장 (한 트랜잭션: 캐릭터 + 칸 UPSERT + 빈 칸 DELETE) -> 다시 들어와서 칸 배치 확인
Stop-DummyClients -Name A
Wait-LogCount -Name server -Pattern 'saved character on leave' -Expected 1
Start-DummyClients -Name B -Count 1 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* inv=20 items=' -Expected 1
Assert-LogContains -Name B -Pattern 'items=.*19:1001x50'
Assert-LogContains -Name B -Pattern 'items=.*:1001x10'
Assert-LogContains -Name B -Pattern 'items=.*:2001x1.*:2001x1'
Assert-LogContains -Name B -Pattern 'items=\d+:\d+x\d+,\d+:\d+x\d+,\d+:\d+x\d+,\d+:\d+x\d+$'	# 정확히 4칸 (빈 칸 줄이 지워짐)
Assert-ServerAlive
