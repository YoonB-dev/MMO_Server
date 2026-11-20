# 새 시나리오 템플릿: 이 파일을 복사해서 이름을 바꾸면 (예: Scenarios/MyTest.ps1)
#   .\Test\Run-Test.ps1 -Scenario MyTest 로 실행된다. (_로 시작하는 파일은 목록에서 제외)
#
# 쓸 수 있는 함수 (TestLib.psm1)
#   Start-GameServer                                   서버 실행, listening 뜰 때까지 대기
#   Start-DummyClients -Name A -Count 3 [-Arguments '--no-ping']
#   Stop-DummyClients  -Name A                          그룹 강제 종료 (연결 끊김)
#   Wait-Seconds 2
#   Wait-LogCount      -Name A -Pattern '...' -Expected 3 [-TimeoutSec 10]
#   Assert-LogCount    -Name A -Pattern '...' -Expected 3
#   Assert-LogContains / Assert-LogNotContains -Name A -Pattern '...'
#   Assert-ServerAlive
#   (서버 로그 이름은 'server')

Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 1

Assert-ServerAlive
