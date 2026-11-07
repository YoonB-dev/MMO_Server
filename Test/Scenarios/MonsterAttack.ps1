# 몬스터 공격: 기본 공격 + 서버 검증
#   A(--attack-test)가 추적 테스트로 몬스터 앞에 선 뒤 공격 묶음을 한 번에 보냄
#     거절: 없는 스킬 1, 원거리(대상 필수) 타겟 0 1, 원거리 플레이어 id 1, 쿨타임 1 / 통과: 기본 공격 1 (HP 200 -> 190)
#   -> 이후 1초마다(S_PING) 기본 공격 (빗나가도 쿨타임을 써서 하나씩)
#     1번째 먼 몬스터 (빗나감 out-of-range), 2번째 허공 (빗나감 no-target) -> 둘 다 S_SKILL target=0
#     그다음부터 타겟: HP 180 -> 170
#   server: 10초 통계에 거절 / 빗나감 이유별 횟수
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--attack-test'	# ping을 켜 둠 (1초마다 공격)
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=170 ' -Expected 1 -TimeoutSec 10
Wait-LogCount -Name server -Pattern 'skill misses' -Expected 1 -TimeoutSec 15

Assert-LogContains -Name A -Pattern '\(monster hp=200\)'					# S_SPAWN에 HP가 실림
Assert-LogCount -Name A -Pattern 'S_CHANGE_HP .* hp=190 ' -Expected 1		# 먼 몬스터가 잘못 맞았으면 2줄
Assert-LogCount -Name A -Pattern 'S_SKILL caster=\d+ skill=1 target=0 ' -Expected 2	# 빗나감 2번 (먼 몬스터, 허공)
Assert-LogContains -Name server -Pattern 'unknown-skill 1, cooldown 1, no-mp 0, no-target 1, bad-target 1, invulnerable 0, out-of-range 0'
# 빗나감 2개는 1초 차이라 10초 통계 경계에 걸리면 두 줄로 나뉠 수 있음 -> 따로 확인
Assert-LogContains -Name server -Pattern 'skill misses: no-target 1, bad-target 0'
Assert-LogContains -Name server -Pattern 'skill misses: .*out-of-range 1\s*$'
Assert-ServerAlive
