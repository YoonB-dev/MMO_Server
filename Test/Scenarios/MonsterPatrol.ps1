# 몬스터 배회: 3마리가 Idle(1~3초) <-> Patrol(1.5m/s)을 반복
#   server: 목적지에 도착해서 Idle로 돌아오는 로그
#   A: 몬스터가 움직이는(state=1, speed=1.5) S_MOVE와 멈춘(state=0, speed=0) S_MOVE를 받음
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --monster-log'
Wait-LogCount -Name server -Pattern 'Patrol -> Idle' -Expected 3
Wait-LogCount -Name A -Pattern 'MONSTER_MOVE .*state=1 speed=1\.5' -Expected 1
Wait-LogCount -Name A -Pattern 'MONSTER_MOVE .*state=0 speed=0' -Expected 1
Assert-ServerAlive
