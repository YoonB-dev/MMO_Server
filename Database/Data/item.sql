-- 아이템 기획 데이터 (data_item)
--   Setup-Database.ps1이 실행될 때마다 지우고 다시 만든다 -> 값을 바꾸려면 DB가 아니라 이 파일을 고치고 커밋
--   문장은 줄 끝의 ;로 끝낸다 (; 뒤에 주석 금지). Setup이 그 기준으로 나눠서 하나씩 실행
--   item_id는 클라도 같은 번호를 씀 (아이콘, 이름 표시). 0번은 쓰지 않음 (드랍에서 0 = 골드)

DROP TABLE IF EXISTS data_item;

CREATE TABLE data_item (
    item_id    INT          NOT NULL PRIMARY KEY,
    name       VARCHAR(32)  NOT NULL,            -- 사람이 보는 용도 (서버는 안 읽음)
    max_stack  INT          NOT NULL,            -- 한 캐릭터가 가질 수 있는 최대 개수 (넘으면 못 주움)
    CONSTRAINT chk_item_id    CHECK (item_id > 0),
    CONSTRAINT chk_item_stack CHECK (max_stack >= 1)
) ENGINE=InnoDB;

INSERT INTO data_item (item_id, name, max_stack) VALUES
    (1001, 'SlimeJelly', 999),
    (2001, 'RustySword', 1);
