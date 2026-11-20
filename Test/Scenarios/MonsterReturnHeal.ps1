# 복귀 중 무적 + 도착하면 HP 회복
#   A(--attack-test --flee-test): 몬스터 앞에서 공격 묶음(정상 1개 -> hp 190) 후 반대로 도망
#     계속 1초마다 공격: 쫓아오는 동안은 사거리 밖, 몬스터가 포기하고 돌아가는 동안(약 3초)은 무적으로 거절
#   server: 포기(leash 또는 lost) -> Return -> 스폰 도착 시 200으로 회복, 10초 통계의 빗나감에 invulnerable (기본 공격은 빗나감으로 나감)
#     포기 이유는 몬스터 위치에 따라 달라짐 (스폰에서 20m가 먼저인지, 플레이어와 12m가 먼저인지). 이 시나리오는 복귀 이후만 봄
#   B(옵션 없음): 시작 위치(0, 0)에 가만히 서서 회복을 봄
#     시야(AOI) 때문에 A는 회복을 못 받음: 계속 도망쳐서 몬스터가 스폰에 도착할 때쯤 35m 밖 -> S_DESPAWN으로 이미 안 보임
#     스폰 3곳은 (0, 0)에서 21 ~ 25m라 B에게는 항상 보임
#   B: S_CHANGE_HP hp=200 damage=0 attacker=0 (회복)
Start-GameServer

Start-DummyClients -Name B -Count 1 -Arguments '--no-ping'
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 1
Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --flee-test'
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=190 ' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'give up .*\((leash|lost)\)' -Expected 1
Wait-LogCount -Name server -Pattern 'Return -> Idle' -Expected 1
Wait-LogCount -Name B -Pattern 'S_CHANGE_HP .* hp=200 damage=0 attacker=0' -Expected 1
Wait-LogCount -Name server -Pattern 'skill misses: .*invulnerable [1-9]' -Expected 1 -TimeoutSec 15
Assert-ServerAlive
