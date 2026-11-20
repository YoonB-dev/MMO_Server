# 20명이 같은 몬스터를 잡고 1초마다 동시에 줍기 키 -> 드랍 3개가 각각 정확히 한 명에게만 (중복 획득 없음)
#   드랍 확률 100% (골드 + 젤리 + 검), 리스폰을 10분으로 늘려서 두 번째 처치가 끼어들지 않게
#   몬스터 근처(줍기 범위 3m 안)에 있는 클라만 주울 수 있음 -> 그 사이의 경쟁을 봄
#   보는 사람은 20명 모두(시야 30m 안) -> S_PICKUP은 드랍 3개 x 20명 = 60개
Set-GameData @(
    'UPDATE data_monster_drop SET chance_permille = 1000',
    'UPDATE data_monster_spawn SET respawn_ms = 600000')
Start-GameServer

Start-DummyClients -Name A -Count 20 -Arguments '--attack-test --pickup-test'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 20 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'picked up drop' -Expected 3 -TimeoutSec 10
Wait-LogCount -Name A -Pattern 'S_PICKUP ' -Expected 60
Assert-LogCount -Name server -Pattern 'picked up drop' -Expected 3				# 같은 드랍을 두 번 주운 일 없음
Assert-LogCount -Name A -Pattern 'S_CHANGE_GOLD ' -Expected 1					# 골드는 한 명만
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=1001 ' -Expected 1
Assert-LogCount -Name A -Pattern 'S_CHANGE_ITEM item=2001 ' -Expected 1
Assert-LogCount -Name A -Pattern 'S_PICKUP ' -Expected 60						# 늦게 온 중복 S_PICKUP도 없음
Assert-ServerAlive
