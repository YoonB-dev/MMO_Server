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
- `GameServer.cpp`의 `USE_DATABASE`가 true면 DB 연결이 되어야 서버가 뜬다.
- 실행 파일은 `Binary/<구성>/`에서 가져온다. 코드를 바꿨으면 `-Build`를 붙이거나 VS에서 먼저 빌드한다.

## 시나리오

| 이름 | 확인 내용 |
|---|---|
| `Ping` | 1명 접속, S_PING 왕복 |
| `EnterLeave` | A 3명 + B 3명 입장, 스폰 수, B 종료 시 A가 디스폰 9개 수신 |
| `ManyClients` | 100명 동시 입장, 한꺼번에 종료해도 서버 생존 |

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
| `Start-GameServer [-TimeoutSec 10]` | 서버 실행, 로그에 `listening`이 뜰 때까지 대기. 실패하면 시나리오 중단 |
| `Start-DummyClients -Name -Count [-Arguments]` | 이름 붙인 DummyClient 그룹 실행 (프로세스 하나에 Count명) |
| `Stop-DummyClients -Name` | 그룹 강제 종료 (연결이 한꺼번에 끊김) |
| `Wait-Seconds n` | 대기 |
| `Wait-LogCount -Name -Pattern -Expected [-TimeoutSec 10]` | 패턴이 Expected 줄 이상 나올 때까지 대기 (시간 초과는 실패) |
| `Assert-LogCount -Name -Pattern -Expected` | 줄 수가 정확히 같은지 |
| `Assert-LogContains` / `Assert-LogNotContains -Name -Pattern` | 한 번 이상 나왔는지 / 안 나왔는지 |
| `Assert-ServerAlive` | 서버 프로세스가 살아 있는지 |

- `-Name`은 로그 파일 이름이기도 하다. 서버 로그는 `server`.
- `-Pattern`은 정규식이고, 패턴이 들어 있는 **줄 수**를 센다.
- 시나리오가 끝나거나 중간에 실패해도 띄운 프로세스는 모두 정리된다.

### DummyClient 옵션

```
DummyClient.exe [--count N] [--ip 127.0.0.1] [--port 7777] [--no-ping]
```

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--count N` | 3 | 접속할 클라이언트 수 |
| `--ip`, `--port` | 127.0.0.1, 7777 | 서버 주소 |
| `--no-ping` | ping 사용 | 1초마다 C_PING 보내기 끄기 |

로그 형식은 `[Client <번호>] <내용>`이고, 한 줄씩 통째로 출력돼서 여러 세션이 동시에 찍어도 섞이지 않는다.
새 동작(예: 이동)을 테스트하려면 DummyClient에 옵션을 추가하고 `-Arguments`로 넘긴다.
