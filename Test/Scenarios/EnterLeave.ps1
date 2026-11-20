# 입장/퇴장: A 3명 입장 -> B 3명 입장 -> B 종료
#
# 보이는 플레이어 수로 셈 (S_SPAWN 줄 수가 아니라 그 안의 플레이어 id 수)
#   새로 들어온 사람은 입장 즉시 주변 목록을 받고, 이미 있던 사람들은 다음 틱 시야 계산에서 새로 온 사람을 받음
#   -> 같은 틱에 여러 명이 들어오면 한 줄(S_SPAWN ids: 7 8 9)로 묶여서 옴. 그래서 줄 수는 그때그때 다름
#   플레이어 id = S_SPAWN 줄에서 공백 뒤에 오고 공백이나 줄 끝으로 끝나는 숫자 (몬스터는 '3(monster hp=200 lv=3)'처럼 괄호가 붙어서 빠짐)
#   A 3명은 각자 다른 5명(A 2 + B 3)을 봄 = 15, B 3명도 각자 다른 5명(B 2 + A 3) = 15
#   B 종료: A 3세션 x B 3명 = S_DESPAWN 9줄 (나가는 사람마다 따로 보냄)
$playerIdPattern = '(?<=S_SPAWN ids:.*\s)\d+(?=\s|$)'

Start-GameServer

Start-DummyClients -Name A -Count 3 -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 3

Start-DummyClients -Name B -Count 3 -Arguments '--no-ping'
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 3
Wait-LogTotal -Name A -Pattern $playerIdPattern -Expected 15
Wait-LogTotal -Name B -Pattern $playerIdPattern -Expected 15

Stop-DummyClients -Name B
Wait-LogCount -Name A -Pattern 'S_DESPAWN' -Expected 9

Assert-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 3
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 3
Assert-That ((Get-LogMatchTotal -Name A -Pattern $playerIdPattern) -eq 15) "A saw 15 player spawns (got $(Get-LogMatchTotal -Name A -Pattern $playerIdPattern))"
Assert-That ((Get-LogMatchTotal -Name B -Pattern $playerIdPattern) -eq 15) "B saw 15 player spawns (got $(Get-LogMatchTotal -Name B -Pattern $playerIdPattern))"
Assert-LogCount -Name A -Pattern 'S_DESPAWN' -Expected 9
Assert-ServerAlive
