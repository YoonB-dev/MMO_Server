# 플레이어 죽음 + 5초 뒤 부활
#   몬스터 공격 데미지를 100으로 덮어써서 한 대에 죽게 함 (시나리오가 끝나면 Run-Test가 원래 값으로 복구)
#   A(--death-test): 몬스터 앞에 가만히 있다가 맞아 죽음 -> 죽은 상태에서 이동 1개 + 공격 1개 -> 서버가 dead로 거절
#   server: player killed, 몬스터는 타겟이 죽어서 포기(dead) -> 5초 뒤 시작 위치 (0, 0)에서 부활
#   A: S_CHANGE_HP hp=0 -> S_DIE revive=5000 -> S_REVIVE (0, 0) hp=100 -> S_CHANGE_MP mp=50 (절반)
Set-GameData -Sql "UPDATE data_skill SET damage = 100 WHERE skill_id = 101"
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --death-test'
Wait-LogCount -Name A -Pattern 'S_DIE .* revive=5000' -Expected 1 -TimeoutSec 20
Wait-LogCount -Name server -Pattern 'give up target \d+ \(dead\)' -Expected 1
Wait-LogCount -Name A -Pattern 'S_REVIVE ' -Expected 1 -TimeoutSec 10
Wait-LogCount -Name server -Pattern 'skill rejects: dead 1,' -Expected 1 -TimeoutSec 15

Assert-LogContains -Name server -Pattern 'player \d+ killed by \d+, revive in 5000ms'
Assert-LogContains -Name server -Pattern 'reason dead'							# 죽은 동안 이동 거절
Assert-LogContains -Name A -Pattern 'S_CHANGE_HP id=\d+ hp=0 damage=100 '
Assert-LogContains -Name A -Pattern 'S_REVIVE id=\d+ x=0 z=0 hp=100'
Assert-LogContains -Name A -Pattern 'S_CHANGE_MP mp=50'
Assert-ServerAlive
