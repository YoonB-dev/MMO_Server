# 드랍 상한: 방 전체 드랍이 상한을 넘으면 가장 오래된 것부터 지움
#   상한 2, 드랍 확률 100% -> 슬라임 한 마리가 3개(골드 -> 젤리 -> 검 순서로 만듦)를 떨어뜨리면
#   세 번째(검)를 만들 때 가장 오래된 골드가 지워짐 -> 주변에는 젤리, 검만 보임 (골드는 S_SPAWN 전에 지워짐)
Set-GameData @('UPDATE data_monster_drop SET chance_permille = 1000')
Start-GameServer -Arguments '--max-drops 2'

Start-DummyClients -Name A -Count 4 -Arguments '--attack-test'
Wait-LogCount -Name server -Pattern 'dropped 3 \(gold \d+\)' -Expected 1 -TimeoutSec 30
Assert-LogCount -Name server -Pattern 'drop limit 2: removed oldest drop' -Expected 1
Wait-LogCount -Name A -Pattern '\(drop item=1001 x[12]\)' -Expected 4
Wait-LogCount -Name A -Pattern '\(drop item=2001 x1\)' -Expected 4
Assert-LogCount -Name A -Pattern '\(drop gold=' -Expected 0		# 가장 오래된 골드가 지워짐
Assert-ServerAlive
