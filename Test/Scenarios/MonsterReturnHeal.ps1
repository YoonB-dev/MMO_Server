# 복귀 중 무적 + 도착하면 HP 회복
#   A(--attack-test --flee-test): 몬스터 앞에서 공격 묶음(정상 1개 -> hp 190) 후 반대로 도망
#     계속 1초마다 공격: 쫓아오는 동안은 사거리 밖, 몬스터가 포기하고 돌아가는 동안(약 3초)은 무적으로 거절
#   server: 포기(leash 또는 lost) -> Return -> 스폰 도착 시 200으로 회복, 10초 통계의 빗나감에 invulnerable (기본 공격은 빗나감으로 나감)
#     포기 이유는 몬스터 위치에 따라 달라짐 (스폰에서 20m가 먼저인지, 플레이어와 12m가 먼저인지). 이 시나리오는 복귀 이후만 봄
#   A: S_CHANGE_HP hp=200 damage=0 attacker=0 (회복)
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --flee-test'
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=190 ' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'give up .*\((leash|lost)\)' -Expected 1
Wait-LogCount -Name server -Pattern 'Return -> Idle' -Expected 1
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=200 damage=0 attacker=0' -Expected 1
Wait-LogCount -Name server -Pattern 'skill misses: .*invulnerable [1-9]' -Expected 1 -TimeoutSec 15
Assert-ServerAlive
