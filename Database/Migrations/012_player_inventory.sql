-- 인벤토리를 칸 단위로: 캐릭터마다 칸 번호(0부터) -> 아이템, 개수
--   한 칸에 쌓이는 최대 개수는 data_item.max_stack (장비는 1이라 한 칸에 하나씩)
--   칸 수(20)는 서버 규칙이라 여기서 상한을 검사하지 않음 (나중에 칸을 늘려도 DB는 그대로)
--   item_id는 data_item을 가리키지만 FK는 걸지 않음 (player_item과 같은 이유: Setup마다 다시 만드는 기획 테이블)
CREATE TABLE player_inventory (
    character_id  BIGINT    NOT NULL,
    slot          INT       NOT NULL,
    item_id       INT       NOT NULL,
    count         INT       NOT NULL,
    updated_at    DATETIME  NOT NULL DEFAULT (UTC_TIMESTAMP()),
    PRIMARY KEY (character_id, slot),
    CONSTRAINT chk_inventory_slot  CHECK (slot >= 0),
    CONSTRAINT chk_inventory_count CHECK (count > 0),
    CONSTRAINT fk_inventory_character FOREIGN KEY (character_id) REFERENCES player_character (character_id)
) ENGINE=InnoDB;
