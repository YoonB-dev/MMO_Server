-- 성장과 재화: 레벨, 경험치(이번 레벨에서 쌓인 양), 골드. 기존 캐릭터는 기본값(1, 0, 0)으로 채워짐
--   경험치 / 골드는 계속 쌓이는 값이라 BIGINT. 음수는 서버 버그로만 생기므로 DB에서도 막음
--   최대 레벨, 레벨별 필요 경험치는 기획 데이터(data_level)라서 여기서 검사하지 않음
ALTER TABLE player_character ADD COLUMN level INT NOT NULL DEFAULT 1 AFTER nickname, ADD COLUMN exp BIGINT NOT NULL DEFAULT 0 AFTER level, ADD COLUMN gold BIGINT NOT NULL DEFAULT 0 AFTER exp, ADD CONSTRAINT chk_character_progress CHECK (level >= 1 AND exp >= 0 AND gold >= 0);
