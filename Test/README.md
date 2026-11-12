# Test

서버와 DummyClient를 실제로 띄워서 접속/입장/종료 흐름을 확인하는 시나리오 테스트.

## 실행

```powershell
.\Test\Run-Test.ps1                          # 시나리오 목록 보기
.\Test\Run-Test.ps1 -Scenario EnterLeave     # 하나 실행 (여러 개: -Scenario Ping,EnterLeave)
.\Test\Run-Test.ps1 -All                     # 전체 실행
.\Test\Run-Test.ps1 -All -Build              # 솔루션 빌드 후 전체 실행
.\Test\Run-Test.ps1 -All -Configuration Release
```

스크립트 실행이 막혀 있으면 (이번 실행에만 제한 해제, 시스템 설정은 그대로):

```powershell
powershell -ExecutionPolicy Bypass -File .\Test\Run-Test.ps1 -All
```

결과는 `[PASS]` / `[FAIL]`로 출력되고, 하나라도 실패하면 종료 코드가 1이다.
로그는 `Test/logs/<실행 시각>/<시나리오>/`에 프로세스별로 남는다 (`server.txt`, `A.txt`, ...). git에는 올라가지 않는다.

## 주의

- 서버는 `GameServer/config.ini`의 IP/Port로 뜬다. 같은 포트로 이미 서버가 떠 있으면 시작 단계에서 실패한다.
- 서버는 시작할 때 `config.ini`의 [Database]로 MySQL에 접속한다 (`GameServer.cpp`의 `USE_DATABASE`, 기본 true). MySQL 서비스가 켜져 있어야 한다.
- `Run-Test.ps1`은 시나리오 전에 `Database/Setup-Database.ps1`을 불러서 DB가 없으면 만들고, 기획 데이터(`Database/Data/*.sql`)를 다시 적용하고, 안 적용한 Migrations를 적용한다. 실패하면 시나리오를 돌리지 않는다.
- 실행 파일은 `Binary/<구성>/`에서 가져온다. 코드를 바꿨으면 `-Build`를 붙이거나 VS에서 먼저 빌드한다. `-Build`는 아웃게임 서버(`dotnet build OutGameServer`)도 같이 빌드한다.
- 아웃게임 서버는 처음 한 번 `OutGameServer/Setup-Secrets.ps1` 실행과 HTTPS 개발 인증서 신뢰(`dotnet dev-certs https --trust`)가 필요하다. `Start-GameServer`가 아웃게임 서버도 같이 띄우므로 **모든 게임 서버 시나리오**에 필요하다 (DummyClient가 로그인한 토큰으로 입장함). Setup-Secrets는 같은 JWT 키를 `config.ini` [Auth]에도 써 준다.
- DummyClient 계정은 `<그룹 이름><시나리오 시각>_<번호>` (예: `A1112153012_1`)로 시나리오마다 새로 가입된다. 계정/캐릭터는 Migrations 테이블이라 지워지지 않고 쌓인다.

## 시나리오

| 이름 | 확인 내용 |
|---|---|
| `DbConnect` | 서버가 시작할 때 DB에 접속하고 확인 쿼리까지 성공 (`[DB] connected: mmo_db`), 기획 데이터(몬스터, 스킬)를 읽어서 3마리 스폰 |
| `Ping` | 1명 접속, S_PING 왕복 |
| `EnterLeave` | A 3명 + B 3명 입장, 스폰 수, B 종료 시 A가 디스폰 9개 수신 |
| `ManyClients` | 100명 동시 입장, 한꺼번에 종료해도 서버 생존 |
| `MoveInputValidation` | 이상한 이동 입력(NaN, 중복 seq, 초당 30개 초과)을 서버가 버리는지 |
| `Move` | 오른쪽 약 1초 이동 후 멈춤: 위치 약 5m, yaw 90, ack 2. 다른 클라도 S_MOVE 수신 |
| `MonsterPatrol` | 몬스터가 Idle <-> Patrol 반복, 클라가 이동(speed 1.5)/멈춤(speed 0) S_MOVE 수신 |
| `MonsterChase` | 몬스터 쪽으로 걸어가 5m 안에서 멈추면, 몬스터가 Chase로 바뀌어 다가와서 1.5m 앞에 멈춤 |
| `MonsterTurnSend` | 위 추적 후 옆으로 피하면, 몬스터 방향이 10도 넘게 꺾일 때 4틱을 기다리지 않고 S_MOVE가 옴 (angle 정책) |
| `MonsterReturn` | 위 추적 후 접속을 끊으면, 몬스터가 포기(left)하고 스폰으로 돌아가 Idle |
| `MonsterLeash` | 위 추적 후 반대로 도망치면, 몬스터가 스폰에서 20m를 넘을 때 포기(leash)하고 speed 6으로 돌아가 Idle |
| `MonsterAttack` | 위 추적 후 공격 묶음: 거절 4개(없는 스킬, 원거리 타겟 0, 원거리 플레이어 id, 쿨타임) + 정상 1개(HP 200 -> 190). 이후 1초마다 기본 공격: 먼 몬스터, 허공은 빗나감(S_SKILL target=0), 그다음부터 170까지. 서버 통계에 거절/빗나감 이유별 횟수 |
| `MonsterDeath` | 4명이 같은 몬스터를 때려서 HP 0 -> 모두 S_DIE 수신, 10초(respawn_ms) 뒤 같은 스폰 자리에 새 id로 리스폰(S_SPAWN hp=200) |
| `MonsterReturnHeal` | 때린 뒤 도망쳐서 몬스터가 포기(leash)하면, 돌아가는 동안 공격이 무적으로 빗나가고(데미지 없음) 스폰에 도착하면 HP 200으로 회복(S_CHANGE_HP damage=0) |
| `MonsterRanged` | 몬스터에게서 10m(감지 거리 8m 밖)에 멈춰 원거리 스킬 -> 맞은 몬스터가 쫓아옴(aggro), HP 200 -> 170, MP 100 -> 80 -> 1초마다 +1. 바로 또 쏜 것은 쿨타임 거절이고 MP를 안 깎음 |
| `MonsterHit` | 몬스터 앞에 가만히 서 있으면, 몬스터가 예고(S_SKILL skill=101 cast=600 range=2 angle=45) 후 공격 -> 서버 `attack hit`, 내 HP 100 -> 84, 이후 자연 회복 85 |
| `MonsterDodge` | 나를 노린 예고를 보자마자 반대로 달리면 판정 순간 부채꼴 밖 -> 서버 `attack miss`, 데미지 없음 |
| `PlayerDeath` | (몬스터 데미지를 100으로 덮어씀) 맞아 죽으면 S_DIE revive=5000, 몬스터는 포기(dead), 죽은 동안 이동/공격은 dead로 거절, 5초 뒤 (0, 0)에서 부활(S_REVIVE hp=100, MP 50) |
| `EnterGameAuth` | 입장 토큰 검증: 로그인한 토큰 2명 입장(서버 `[DB] loaded character`) + 직접 만든 토큰 대조군 통과. 위조(sub만 바꿈)/다른 키/alg none/빈 토큰은 `error=1`, 만료는 `error=2`. 실패한 토큰은 DB까지 안 가고, 연결은 유지 |
| `AuthFlow` | 아웃게임 서버: 가입(201 / 중복 409 / 형식 400) -> 로그인 실패 401(틀린 비밀번호와 없는 아이디가 같은 응답) -> 로그인(JWT claims iss/aud/sub, 15분) -> refresh 회전 -> 옛 refresh 재사용 시 401 + 계정 토큰 전부 폐기 -> logout 뒤 refresh 401 |

## 시나리오 추가

`Scenarios/_Template.ps1`을 복사해서 `Scenarios/<이름>.ps1`로 저장하면 바로 `-Scenario <이름>`으로 실행된다.
`_`로 시작하는 파일은 목록에서 빠진다.

```powershell
Start-GameServer
Start-DummyClients -Name A -Count 3 -Arguments '--no-ping'
Wait-LogCount -Name A -Pattern 'S_ENTER_GAME success=1' -Expected 3
Stop-DummyClients -Name A
Assert-ServerAlive
```

### 함수 (TestLib.psm1)

| 함수 | 설명 |
|---|---|
| `Set-GameData -Sql "<문장>"` | 이 시나리오에서만 기획 데이터 수치를 덮어씀. `Start-GameServer` 전에 호출. 시나리오가 끝나면(실패해도) Run-Test가 원래 값으로 복구 |
| `Start-GameServer [-TimeoutSec 10]` | (아웃게임 서버가 아직 없으면 먼저 띄우고) 서버 실행, 로그에 `listening`이 뜰 때까지 대기. 실패하면 시나리오 중단 |
| `Start-DummyClients -Name -Count [-Arguments]` | 이름 붙인 DummyClient 그룹 실행 (프로세스 하나에 Count명). 계정 이름 `<Name><시나리오 시각>_<번호>`로 가입/로그인 후 입장 |
| `Stop-DummyClients -Name` | 그룹 강제 종료 (연결이 한꺼번에 끊김) |
| `Wait-Seconds n` | 대기 |
| `Wait-LogCount -Name -Pattern -Expected [-TimeoutSec 10]` | 패턴이 Expected 줄 이상 나올 때까지 대기 (시간 초과는 실패) |
| `Assert-LogCount -Name -Pattern -Expected` | 줄 수가 정확히 같은지 |
| `Assert-LogContains` / `Assert-LogNotContains -Name -Pattern` | 한 번 이상 나왔는지 / 안 나왔는지 |
| `Assert-ServerAlive` | 서버 프로세스가 살아 있는지 |
| `Start-OutGameServer [-TimeoutSec 20]` | 아웃게임 서버 실행, `Now listening on`이 뜰 때까지 대기. 로그 이름 `outgame` |
| `Invoke-AuthApi <경로> @{...}` | 아웃게임 서버에 JSON POST. 4xx/5xx도 예외 없이 `@{ Status; Body }`로 돌려줌 |
| `Assert-That <조건> <메시지>` | 로그가 아니라 값(응답 코드 등)으로 확인 |
| `Assert-ProcessAlive -Name` | 이름 붙인 프로세스(`outgame` 등)가 살아 있는지 |
| `New-TestJwt [-Sub] [-ExpOffsetSec 900] [-Alg HS256\|none] [-SecretBase64] [-Issuer] [-Audience]` | 테스트용 Access Token을 직접 만듦. 기본은 `Binary/<구성>/config.ini`의 키로 서명한 정상 토큰. 음수 `-ExpOffsetSec` = 만료, `-Alg none` = 서명 없음 |
| `Get-RunStamp` | 이번 시나리오의 시각 문자열 (`MMddHHmmss`). 시나리오 안에서 새 계정 이름 만들 때 |

- `-Name`은 로그 파일 이름이기도 하다. 서버 로그는 `server`.
- `-Pattern`은 정규식이고, 패턴이 들어 있는 **줄 수**를 센다.
- 시나리오가 끝나거나 중간에 실패해도 띄운 프로세스는 모두 정리된다.

### DummyClient 옵션

```
DummyClient.exe [--count N] [--ip 127.0.0.1] [--port 7777] [--account-prefix dummy] [--auth-host localhost] [--auth-port 7100] [--token T] [--no-ping] [--monster-log] [--chase-test] [--turn-test] [--flee-test] [--attack-test] [--ranged-test] [--dodge-test] [--death-test]
```

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--count N` | 3 | 접속할 클라이언트 수 |
| `--ip`, `--port` | 127.0.0.1, 7777 | 서버 주소 |
| `--account-prefix` | dummy | 계정 이름 = `<prefix>_1`, `<prefix>_2` ... 시작할 때 아웃게임 서버에 가입(이미 있으면 넘어감) + 로그인을 N명 동시에(`std::async`) 하고, 받은 토큰으로 입장. 하나라도 실패하면 종료(-1) |
| `--auth-host`, `--auth-port` | localhost, 7100 | 아웃게임 서버 주소 (HTTPS) |
| `--token T` | 로그인 | 로그인 없이 모든 클라가 T를 토큰으로 보냄 (토큰 공격 테스트용, `""`면 빈 토큰). 입장 실패는 `S_ENTER_GAME success=0 ... error=N` |
| `--no-ping` | ping 사용 | 1초마다 C_PING 보내기 끄기 |
| `--monster-log` | 끔 | S_MOVE의 몬스터 항목을 `MONSTER_MOVE ...` 한 줄씩 출력 |
| `--chase-test` | 끔 | 가장 가까운 몬스터 쪽으로 걸어가 5m 안에서 멈추고, 몬스터가 다가와 멈추면 `CHASE_TEST ok/fail` |
| `--turn-test` | 끔 | `--chase-test` 후 옆으로 피하면서, 4틱 주기 밖에서 온 몬스터 이동마다 `TURN_SEND` |
| `--flee-test` | 끔 | `--chase-test` 후 몬스터 반대 방향으로 계속 달림 (몬스터 복귀 확인용) |
| `--attack-test` | 끔 | `--chase-test` 후 앞에 멈춘 몬스터에게 검증용 공격 묶음을 보내고, 이후 S_PING(1초)마다 기본 공격 (처음 두 번은 먼 몬스터, 허공으로 빗나감, ping을 끄면 묶음만). 타겟이 죽으면(S_DIE) 멈춤 |
| `--ranged-test` | 끔 | 가장 가까운 몬스터 쪽으로 걸어가 10m에서 멈추고, 멈춤이 적용되면 원거리 스킬(2번)을 두 번 연달아 보냄 (`RANGED_TEST shot`) |
| `--dodge-test` | 끔 | `--chase-test` 후 나를 노린 몬스터 예고(S_SKILL cast > 0)를 받으면 바로 몬스터 반대 방향으로 달림 (`DODGE`) |
| `--death-test` | 끔 | `--chase-test` 후 내가 죽으면(S_DIE id가 나) 죽은 상태에서 이동 1개 + 기본 공격 1개를 보냄 (`DEAD_INPUT sent`) |

로그 형식은 `[Client <번호>] <내용>`이고, 한 줄씩 통째로 출력돼서 여러 세션이 동시에 찍어도 섞이지 않는다.
새 동작(예: 이동)을 테스트하려면 DummyClient에 옵션을 추가하고 `-Arguments`로 넘긴다.
