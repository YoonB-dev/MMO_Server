# 입장/퇴장: A 3명 입장 -> B 3명 입장 -> B 종료
#
# S_SPAWN 줄 수 계산 (한 번에 한 명씩 입장한다고 볼 때)
#   - 새로 들어온 사람: 이미 있는 사람이 있으면 목록을 S_SPAWN 1줄로 받음
#   - 이미 있던 사람들: 새로 들어온 사람마다 S_SPAWN 1줄씩 받음
#   A끼리: 첫 입장자는 0줄, 2번째 입장 -> 본인 1 + 기존 1, 3번째 -> 본인 1 + 기존 2  = 5줄
#   B 입장: B 한 명당 A 3세션이 1줄씩                                               = 9줄  -> A 합계 14줄
#   B끼리: B 각자 입장 목록 1줄(3줄) + 나중 입장자 알림(1 + 2 = 3줄)                  = 6줄
#   B 종료: A 3세션 x B 3명                                                         = S_DESPAWN 9줄
Start-GameServer

Start-DummyClients -Name A -Count 3 -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 3

Start-DummyClients -Name B -Count 3 -Arguments '--no-ping'
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 3
Wait-LogCount -Name A -Pattern 'S_SPAWN' -Expected 14

Stop-DummyClients -Name B
Wait-LogCount -Name A -Pattern 'S_DESPAWN' -Expected 9

Assert-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 3
Assert-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 3
Assert-LogCount -Name A -Pattern 'S_SPAWN' -Expected 14
Assert-LogCount -Name B -Pattern 'S_SPAWN' -Expected 6
Assert-LogCount -Name A -Pattern 'S_DESPAWN' -Expected 9
Assert-ServerAlive
