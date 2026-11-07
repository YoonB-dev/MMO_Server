# 원거리 스킬 + MP + 어그로
#   A(--ranged-test): 가장 가까운 몬스터 쪽으로 걸어가 10m(원거리 사거리)에서 멈추고 원거리 스킬을 2번 연달아 보냄
#     감지 거리(8m) 밖이라 몬스터는 아직 A를 모름 -> 맞고(aggro) 쫓아옴
#     두 번째는 쿨타임(5초)으로 거절 -> MP는 한 번만 깎임
#   A: S_ENTER_GAME mp=100/100 -> S_SKILL skill=2 -> S_CHANGE_HP hp=170 damage=30 -> S_CHANGE_MP 90 -> 1초마다 +1
#   HP가 꽉 찬 플레이어는 자연 회복 패킷(damage=0)을 안 받아야 함
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --ranged-test'
Wait-LogCount -Name A -Pattern 'RANGED_TEST shot' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'aggro on' -Expected 1
Wait-LogCount -Name A -Pattern 'S_CHANGE_MP mp=92' -Expected 1		# 90 -> 91 -> 92 (1초에 1)
Wait-LogCount -Name server -Pattern 'skill rejects' -Expected 1 -TimeoutSec 15

Assert-LogContains -Name A -Pattern 'S_ENTER_GAME .* mp=100/100'
Assert-LogContains -Name A -Pattern 'S_SKILL caster=\d+ skill=2 '
Assert-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=170 damage=30 ' -Expected 1
Assert-LogCount -Name A -Pattern 'S_CHANGE_MP mp=80' -Expected 0		# 쿨타임으로 거절된 두 번째는 MP를 안 깎음
Assert-LogCount -Name A -Pattern 'damage=0 attacker=0' -Expected 0		# HP가 꽉 차 있으면 자연 회복 패킷 없음
Assert-LogContains -Name server -Pattern 'unknown-skill 0, cooldown 1, no-mp 0, no-target 0, bad-target 0, invulnerable 0, out-of-range 0'
Assert-ServerAlive
