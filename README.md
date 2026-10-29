# MMO_Server

IOCP 서버 코어(ServerCore) 위에 MMO 컨텐츠를 올리는 서버 프로젝트.

## 구성

| 폴더 | 내용 |
|---|---|
| `ServerCore/` | IOCP 네트워크, 세션, 버퍼, 락, JobQueue, DB 커넥션 풀 (정적 라이브러리) |
| `GameServer/` | 게임 서버 실행 파일. 컨텐츠는 여기에 추가 |
| `DummyClient/` | 접속 확인용 클라이언트 (1초마다 C_PING -> S_PING 왕복 시간 출력) |
| `Common/protoc-21.12-win64/bin/` | `.proto` 원본, `protoc.exe`, 패킷 생성기(`GenPackets.exe`, `Templates/`) |
| `Tools/PacketGenerator/` | 패킷 생성기 소스 (템플릿 수정 후 `MakeExe.bat`으로 exe 재생성) |
| `Libraries/` | Protobuf 헤더(`Include/`)와 라이브러리(`Libs/Protobuf/`), ServerCore 빌드 결과(`Libs/ServerCore/`) |

## 처음 설정

1. `Libraries/Libs/Protobuf/`에 Protobuf 라이브러리 복사 (git에 없음, `Debug/libprotobufd.lib`, `Release/libprotobuf.lib`)
2. `GameServer/config.example.ini`를 같은 폴더에 `config.ini`로 복사
3. `MMO_Server.sln`을 열고 x64로 빌드 (ServerCore -> GameServer -> DummyClient 순서로 빌드되게 설정됨)
4. `Binary/<구성>/GameServer.exe` 실행 후 `DummyClient.exe` 실행 -> `S_PING RTT`가 찍히면 연결 정상

## 패킷 추가

1. `Common/protoc-21.12-win64/bin/Protocol.proto` 맨 아래에 `message C_XXX` / `message S_XXX` 추가 (파일은 CP949로 저장)
2. 빌드하면 GameServer 빌드 전 이벤트가 `*.pb.*`, `ClientPacketHandler.h`, `ServerPacketHandler.h`를 생성
3. `GameServer/ClientPacketHandler.cpp`에 `Handle_C_XXX`, `DummyClient/ServerPacketHandler.cpp`에 `Handle_S_XXX` 작성
