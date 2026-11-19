# 같이 잡다가 한 명이 죽으면, 몬스터는 돌아가지 않고 같이 때리던 다른 사람을 쫓음 (돌아가서 풀피가 되지 않음)
#   몬스터 공격 데미지를 100으로 -> 맞으면 한 대에 죽음
#   A 2명(--attack-test)이 같은 몬스터를 때림 -> 몬스터가 한 명을 죽이면 다른 한 명으로 retarget
#   -> 그 사람도 죽이면 더는 없음 -> 그때 돌아감 (give up ... (dead))
Set-GameData @('UPDATE data_skill SET damage = 100 WHERE skill_id = 101')
Start-GameServer

Start-DummyClients -Name A -Count 2 -Arguments '--attack-test'
Wait-LogCount -Name server -Pattern 'retarget \d+ -> \d+ \(dead\)' -Expected 1 -TimeoutSec 30
Wait-LogCount -Name server -Pattern 'give up target \d+ \(dead\)' -Expected 1 -TimeoutSec 20
Assert-LogCount -Name server -Pattern 'retarget \d+ -> \d+ \(dead\)' -Expected 1
Assert-ServerAlive
