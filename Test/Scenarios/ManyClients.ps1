# 많은 수 동시 접속: 100명 입장 -> 전원 S_ENTER_GAME -> 한꺼번에 종료해도 서버가 살아 있는지
$count = 100

Start-GameServer

Start-DummyClients -Name A -Count $count -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected $count -TimeoutSec 20
Assert-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected $count

Stop-DummyClients -Name A
Wait-Seconds 2

Assert-ServerAlive
