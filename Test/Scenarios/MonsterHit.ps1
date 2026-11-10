# 몬스터 예고 공격 (맞음)
#   A(--chase-test): 몬스터 쪽으로 걸어가 멈춘 뒤 가만히 있음
#   server: 몬스터가 사거리(2m) 안에서 멈추고 예고 -> 0.6초 뒤 판정 'attack hit'
#   A: S_SKILL caster=<몬스터> skill=101 cast=600 range=2 angle=45 (예고) -> S_CHANGE_HP 내 HP 100 -> 92
#      -> 1초마다 자연 회복 hp=93 damage=0 attacker=0
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --chase-test'
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'attack hit' -Expected 1
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=93 damage=0 attacker=0' -Expected 1	# 맞은 뒤 자연 회복

Assert-LogContains -Name A -Pattern 'S_SKILL caster=\d+ skill=101 target=\d+ .*cast=600 range=2 angle=45'
Assert-LogContains -Name A -Pattern 'S_CHANGE_HP id=\d+ hp=92 damage=8 '
Assert-ServerAlive
