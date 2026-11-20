-- 인벤토리: 캐릭터마다 아이템 종류별 개수 (칸 개념 없음). 개수가 0이 되면 줄을 지움
--   item_id는 data_item을 가리키지만 FK는 걸지 않음: data_item은 Setup마다 지우고 다시 만드는 기획 테이블이라
--   (FK가 있으면 Setup이 data_item을 못 지움) -> 서버가 불러올 때 data_item에 있는지 확인
CREATE TABLE player_item (
    character_id  BIGINT    NOT NULL,
    item_id       INT       NOT NULL,
    count         INT       NOT NULL,
    updated_at    DATETIME  NOT NULL DEFAULT (UTC_TIMESTAMP()),
    PRIMARY KEY (character_id, item_id),
    CONSTRAINT chk_item_count CHECK (count > 0),
    CONSTRAINT fk_item_character FOREIGN KEY (character_id) REFERENCES player_character (character_id)
) ENGINE=InnoDB;
