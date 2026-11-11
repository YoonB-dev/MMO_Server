# 몬스터 예고 공격 (피함)
#   A(--dodge-test): 몬스터 쪽으로 걸어가 멈춘 뒤, 나를 노린 예고(S_SKILL cast > 0)를 받자마자 반대로 달림
#   server: 0.6초 뒤 판정할 때 A가 부채꼴 밖 -> 'attack miss', 데미지 없음
#   (예고 시간 0.6초 동안 5m/s로 약 3m 벗어남 -> 사거리 2m 밖)
Start-GameServer

Start-DummyClients -Name A -Count 1 -Arguments '--no-ping --dodge-test'
Wait-LogCount -Name A -Pattern 'DODGE' -Expected 1 -TimeoutSec 15
Wait-LogCount -Name server -Pattern 'attack miss' -Expected 1

Assert-LogNotContains -Name server -Pattern 'attack hit'
Assert-LogNotContains -Name A -Pattern 'damage=16 '
Assert-ServerAlive
