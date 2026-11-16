-- 계정당 여러 캐릭터: PK를 account_id에서 character_id(자동 증가)로. 이미 있는 줄에는 1부터 번호가 붙음
ALTER TABLE player_character DROP PRIMARY KEY, ADD COLUMN character_id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST;
