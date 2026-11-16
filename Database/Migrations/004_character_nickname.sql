-- 캐릭터 닉네임 (채팅, 이름표). 이미 있는 줄 때문에 일단 NULL 허용 -> 005에서 채우고 006에서 NOT NULL
ALTER TABLE player_character ADD COLUMN nickname VARCHAR(20) NULL AFTER account_id;
