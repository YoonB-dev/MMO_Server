# 몬스터 공격: 기본 공격 + 서버 검증
#   A(--attack-test)가 추적 테스트로 몬스터 앞에 선 뒤 공격 묶음을 한 번에 보냄
#     없는 스킬 1, 없는 id 1, 플레이어 id 1, 먼 몬스터 1, 정상 1, 바로 또 1(쿨타임)
#   -> 정상 1개만 통과: 몬스터 HP 200 -> 190 (HP와 데미지는 DB: data_monster.max_hp, data_skill.damage)
#   -> 이후 1초마다(S_PING) 기본 공격: 180, 170 ... (클라 쿨타임 0.5초가 지나서 보내므로 거절 없음)
#   server: 10초 통계에 거절 이유별 횟수 한 줄
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test'	# ping을 켜 둠 (1초마다 공격)
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=170 ' -Expected 1 -TimeoutSec 10
Wait-LogCount -Name server -Pattern 'skill rejects' -Expected 1 -TimeoutSec 15

Assert-LogContains -Name A -Pattern '\(monster hp=200\)'				# S_SPAWN에 HP가 실림
Assert-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=190 ' -Expected 1	# 먼 몬스터가 잘못 맞았으면 2줄
Assert-LogContains -Name server -Pattern 'unknown-skill 1, cooldown 1, no-mp 0, bad-target 2, invulnerable 0, out-of-range 1'
Assert-ServerAlive
