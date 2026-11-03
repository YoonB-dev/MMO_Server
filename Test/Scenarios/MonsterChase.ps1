# 몬스터 추적: A가 가장 가까운 몬스터 쪽으로 걸어가서 5m 안에서 멈춤
#   server: 8m 안에 들어오면 Chase로 바뀜
#   A: 몬스터가 다가와서 1.5m 앞에 멈춤 -> 'CHASE_TEST ok'
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --monster-log --chase-test'
Wait-LogCount -Name server -Pattern '-> Chase' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name A -Pattern 'CHASE_TEST' -Expected 1 -TimeoutSec 15

Assert-LogContains -Name A -Pattern 'CHASE_TEST ok'
Assert-ServerAlive
