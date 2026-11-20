# 복귀 (leash): 몬스터가 쫓아와 멈춘 뒤 A가 반대로 도망 -> 몬스터가 쫓다가 스폰에서 20m를 넘으면 포기
#   A: 돌아가는 몬스터(speed 6)를 봄 / server: 스폰 도착 후 Idle
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --monster-log --flee-test'
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'give up .*\(leash\)' -Expected 1
Wait-LogCount -Name A -Pattern 'MONSTER_MOVE .*speed=6' -Expected 1
Wait-LogCount -Name server -Pattern 'Return -> Idle' -Expected 1
Assert-ServerAlive
