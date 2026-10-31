# 이동: B(관전) 입장 -> A가 --move-test로 오른쪽 약 1초 이동 후 멈춤
#   A: (100,0) 입력이 길이 1로 줄어 5m/s로 약 5m 이동, yaw 90, 마지막 ack 2 -> 'MOVE_TEST ok'
#   B: A의 출발/멈춤이 담긴 S_MOVE를 최소 2번 받음
Start-GameServer

Start-DummyClients -Name B -Count 1 -Arguments '--no-ping'
Wait-LogCount -Name B -Pattern 'S_ENTER_GAME success=1' -Expected 1

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --move-test'
Wait-LogCount -Name A -Pattern 'MOVE_TEST' -Expected 1

Assert-LogContains -Name A -Pattern 'MOVE_TEST ok'
Wait-LogCount -Name B -Pattern 'S_MOVE' -Expected 2
Assert-ServerAlive
