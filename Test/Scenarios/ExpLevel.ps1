# 경험치 / 레벨업 / 저장
#   레벨표를 덮어써서 1, 2레벨 필요 경험치를 5로 -> 슬라임(20) 한 마리면 1 -> 2 -> 3레벨, 넘친 10이 남음 (여러 레벨 한 번에)
#   A 4명이 같이 때림 -> 처치자 한 명만 S_CHANGE_EXP, 레벨업은 주변 4명 모두 S_LEVEL_UP
#   그 뒤 끊고 같은 계정으로 다시 들어오면 처치자만 level=3 exp=10으로 시작 (나갈 때 저장)
#   몬스터 레벨(data_monster.level = 3)이 S_SPAWN에 실려 오는지도 확인
Set-GameData @('UPDATE data_level SET exp_to_next = 5 WHERE level IN (1, 2)')
Start-GameServer

Start-DummyClients -Name A -Count 4 -Arguments '--attack-test'
Wait-LogCount -Name A -Pattern 'S_SPAWN ids: .*\(monster hp=\d+ lv=3\)' -Expected 4	# 몬스터 레벨이 S_SPAWN에 실림
Wait-LogCount -Name server -Pattern 'killed by' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name A -Pattern 'S_CHANGE_EXP level=3 exp=10 next=220 gained=20' -Expected 1
Wait-LogCount -Name A -Pattern 'S_LEVEL_UP .* level=3' -Expected 4
Assert-LogCount -Name A -Pattern 'S_CHANGE_EXP' -Expected 1	# 처치자만
Assert-LogCount -Name server -Pattern '\[Room\] level up: player \d+ -> 3' -Expected 1

# 저장: 끊으면 나갈 때 저장 -> 같은 계정(같은 캐릭터)으로 다시 들어오면 처치자만 3레벨
Stop-DummyClients -Name A
Wait-LogCount -Name server -Pattern 'saved character on leave' -Expected 4
Start-DummyClients -Name B -Count 4 -Arguments '--no-ping' -AccountOf A
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* level=3 exp=10 next=220' -Expected 1
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1 .* level=1 exp=0 next=5' -Expected 3
Assert-ServerAlive
