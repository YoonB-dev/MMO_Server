# 방향 꺾임 전송 (기본 정책 angle): 몬스터가 앞에 멈춘 뒤 A가 옆으로 피하면
#   몬스터가 따라오며 10도 넘게 꺾일 때 4틱 주기를 기다리지 않고 바로 S_MOVE가 옴 -> 'TURN_SEND'
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --monster-log --turn-test'
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name A -Pattern 'TURN_SEND' -Expected 1
Assert-ServerAlive
