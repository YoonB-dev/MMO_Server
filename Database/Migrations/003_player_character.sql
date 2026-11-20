-- 유저 데이터: 캐릭터 (계정당 1개). GameServer가 입장할 때 DB 큐에서 불러옴 (없으면 기본값으로 만듦)
--   계정당 1개라 account_id가 곧 키. 최대 HP/MP는 레벨이 생길 때까지 코드 상수라 저장하지 않음
--   시각은 UTC
CREATE TABLE player_character (
    account_id  BIGINT    NOT NULL PRIMARY KEY,
    pos_x       FLOAT     NOT NULL,
    pos_z       FLOAT     NOT NULL,
    hp          INT       NOT NULL,
    mp          INT       NOT NULL,
    created_at  DATETIME  NOT NULL DEFAULT (UTC_TIMESTAMP()),
    updated_at  DATETIME  NOT NULL DEFAULT (UTC_TIMESTAMP()),
    CONSTRAINT fk_character_account FOREIGN KEY (account_id) REFERENCES account (account_id)
) ENGINE=InnoDB;
