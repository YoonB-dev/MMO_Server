## 요약
<!-- 무엇을, 왜 바꿨는지 2~3줄 -->

## 변경 내용
<!-- 영역별로. 해당 없는 줄은 지우기 -->
- **ServerCore**:
- **GameServer**:
- **DummyClient**:
- **Test**:

## 패킷 / 프로토콜
<!-- 없으면 "없음" -->
| 패킷 | 방향 | 변경 | 담기는 데이터 |
|---|---|---|---|
|  | C->S / S->C | 추가 / 필드 추가 / 보내는 시점 변경 |  |

- [ ] 새 패킷은 `Protocol.proto` 맨 아래에 추가함 (기존 패킷 ID가 밀리지 않음)
- 언리얼 클라 protobuf 재생성 필요: 예 / 아니오

## 동작 흐름 / 스레드
<!-- 어떤 함수가 어떤 순서로 불리는지, 어느 JobQueue(스레드)에서 도는지.
     공유 상태(Room, Player 등)는 DoAsync로 넘겼는지 -->

## 테스트
- [ ] `Debug|x64` 빌드, 경고 없음
- [ ] `Test/Run-Test.ps1 -All` 통과 (__개)
- 추가/변경한 시나리오:
- 수동 확인:

## 측정 (해당할 때만)
<!-- Room 10초 통계 줄(avg interval / work avg, max / monster moves).
     전/후 비교라면 같은 조건으로 둘 다 -->

## 체크리스트
- [ ] 인코딩 유지: ServerCore, proto는 CP949 / GameServer, DummyClient는 UTF-8 BOM + CRLF
- [ ] 콘솔 출력 문자열은 영어
- [ ] 생성 파일(`*.pb.*`, `*PacketHandler.h`)은 직접 고치지 않음
- [ ] `GameServer/config.ini`(DB 비밀번호)가 포함되지 않음
