# 방향 꺾임 전송 (기본 정책 angle): 몬스터가 앞에 멈춘 뒤 A가 옆으로 피하면
#   몬스터가 따라오며 10도 넘게 꺾일 때 4틱 주기를 기다리지 않고 바로 S_MOVE가 옴 -> 'TURN_SEND'
#   전송 정책만 보는 테스트라 몬스터 공격은 끔: 공격 사거리를 멈추는 거리(1.5m)보다 짧게 덮어씀
#     (공격하면 몬스터가 예고 동안 멈춰서 A가 멀어지고, 멀리서 쫓으면 방향이 조금씩만 꺾임)
Set-GameData -Sql "UPDATE data_skill SET range_m = 0.1 WHERE skill_id = 101"
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --monster-log --turn-test'
Wait-LogCount -Name A -Pattern 'CHASE_TEST ok' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name A -Pattern 'TURN_SEND' -Expected 1
Assert-ServerAlive
