# 몬스터 죽음과 리스폰
#   A 4명(--attack-test)이 같은 몬스터(입장 위치에서 가장 가까운 것)에게 걸어가서 같이 때림 -> 1명보다 4배 빨리 HP 0
#     (1명이면 1초에 10씩 20초. 4명은 거의 같은 순간의 S_SPAWN을 보고 고르므로 같은 몬스터를 고름)
#   server: 'killed by' -> 10초(data_monster_spawn.respawn_ms) 뒤 같은 스폰 자리에 새 id로 스폰
#   A: S_CHANGE_HP hp=0 -> S_DIE (4명 모두) -> 리스폰 몬스터 하나만 담긴 S_SPAWN (hp=200)
Start-GameServer

Start-DummyClients -Name A -Count 4 -Arguments '--attack-test'
Wait-LogCount -Name server -Pattern 'killed by' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name A -Pattern 'S_DIE ' -Expected 4
Wait-LogCount -Name server -Pattern '\[Room\] spawn monster' -Expected 4 -TimeoutSec 15	# 처음 3 + 리스폰 1
Wait-LogCount -Name A -Pattern 'S_SPAWN ids: \d+\(monster hp=200\)\s*$' -Expected 4

Assert-LogCount -Name server -Pattern 'killed by' -Expected 1
Assert-LogContains -Name A -Pattern 'S_CHANGE_HP .* hp=0 '
Assert-ServerAlive
