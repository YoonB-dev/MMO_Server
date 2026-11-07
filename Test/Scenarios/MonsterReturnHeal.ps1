# 복귀 중 무적 + 도착하면 HP 회복
#   A(--attack-test --flee-test): 몬스터 앞에서 공격 묶음(정상 1개 -> hp 190) 후 반대로 도망
#     계속 1초마다 공격: 쫓아오는 동안은 사거리 밖, 몬스터가 포기하고 돌아가는 동안(약 3초)은 무적으로 거절
#   server: leash -> Return -> 스폰 도착 시 200으로 회복, 10초 통계에 invulnerable
#   A: S_CHANGE_HP hp=200 damage=0 attacker=0 (회복)
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test --flee-test'
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=190 ' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'give up .*\(leash\)' -Expected 1
Wait-LogCount -Name server -Pattern 'Return -> Idle' -Expected 1
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=200 damage=0 attacker=0' -Expected 1
Wait-LogCount -Name server -Pattern 'skill rejects: .*invulnerable [1-9]' -Expected 1 -TimeoutSec 15
Assert-ServerAlive
