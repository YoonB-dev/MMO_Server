# 이동 입력 검증: --input-test 클라 1명이 입장 후 이상한 입력을 섞어 70개를 한꺼번에 보냄
#   NaN 방향 1개    -> reason invalid dir 1줄
#   중복 seq 1개    -> reason old seq 1줄
#   1초 30개 초과   -> reason rate 1줄 이상 (1초 경계에 걸리면 개수가 달라져서 정확한 수는 안 봄)
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --input-test'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 1
Wait-LogCount -Name server -Pattern 'reason rate' -Expected 1

Assert-LogCount -Name server -Pattern 'reason invalid dir' -Expected 1
Assert-LogCount -Name server -Pattern 'reason old seq' -Expected 1
Assert-ServerAlive
