# 접속 + 왕복 확인: 1명이 접속해서 S_PING을 2번 이상 받는지
Start-GameServer

Start-DummyClients -Name A -Count 1
Wait-LogCount -Name A -Pattern 'S_PING RTT' -Expected 2 -TimeoutSec 6

Assert-LogContains -Name A -Pattern 'Connected to server'
Assert-ServerAlive
