# 몬스터가 죽으면 골드 + 아이템이 바닥에 떨어지고(주변에 S_SPAWN), 시간이 지나면 사라짐(S_DESPAWN)
#   드랍 확률을 100%로 덮어씀 -> 골드 1 + 젤리 1 + 검 1 = 3개가 항상 떨어짐
#   수명을 3초로 줄여서 만료까지 확인 (기본 60초)
#   (상한 500개를 넘으면 오래된 것부터 지우는 건 확인 안 함: 몬스터 수백 마리를 잡아야 함)
Set-GameData @('UPDATE data_monster_drop SET chance_permille = 1000')
Start-GameServer -Arguments '--drop-lifetime-sec 3'

Start-DummyClients -Name A -Count 4 -Arguments '--attack-test'
Wait-LogCount -Name server -Pattern 'killed by' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1
Wait-LogCount -Name A -Pattern '\(drop gold=([5-9]|1[0-5])\)' -Expected 4		# 골드 5~15, 주변 4명 모두 봄
Wait-LogCount -Name A -Pattern '\(drop item=1001 x[12]\)' -Expected 4
Wait-LogCount -Name A -Pattern '\(drop item=2001 x1\)' -Expected 4
Wait-LogCount -Name server -Pattern 'drops expired: 3' -Expected 1 -TimeoutSec 10	# 3초 뒤 만료 -> 보던 사람에게 S_DESPAWN
Assert-ServerAlive
