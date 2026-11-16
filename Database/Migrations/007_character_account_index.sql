-- 계정의 캐릭터 목록 조회용 인덱스. 008에서 PK(account_id)를 없애기 전에 먼저 만듦
--   FK(fk_character_account)는 account_id에 인덱스가 반드시 있어야 해서, PK를 지우면 이 인덱스가 그 역할을 이어받음
ALTER TABLE player_character ADD INDEX idx_character_account (account_id);
