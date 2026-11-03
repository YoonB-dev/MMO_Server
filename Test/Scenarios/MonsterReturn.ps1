# 복귀 (left): 몬스터가 쫓아와 멈춘 뒤 A 접속 종료 -> 몬스터가 포기하고 스폰으로 돌아가 Idle
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --chase-test'
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15

Stop-DummyClients -Name A
Wait-LogCount -Name server -Pattern 'give up .*\(left\)' -Expected 1
Wait-LogCount -Name server -Pattern 'Return -> Idle' -Expected 1
Assert-ServerAlive
