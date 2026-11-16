-- 닉네임 규칙: 비어 있으면 안 됨, 중복 금지(대소문자 무시), 영문/숫자/완성형 한글(가~힣) 2글자 이상
--   길이 = 영문/숫자 1 + 한글 2, 합계 20 이하. utf8mb4에서 영문/숫자는 1바이트, 한글은 3바이트라
--   (바이트 수 - 글자 수) / 2 = 한글 수 -> 글자 수 + 한글 수 = 가중치 합
--   아웃게임 서버도 같은 규칙으로 검사함 (여기는 코드 버그나 손으로 고친 값을 막는 이중 방어)
ALTER TABLE player_character MODIFY nickname VARCHAR(20) NOT NULL, ADD CONSTRAINT uq_character_nickname UNIQUE (nickname), ADD CONSTRAINT chk_character_nickname CHECK (REGEXP_LIKE(nickname, '^[A-Za-z0-9\\x{AC00}-\\x{D7A3}]{2,20}$') AND CHAR_LENGTH(nickname) + (LENGTH(nickname) - CHAR_LENGTH(nickname)) / 2 <= 20);
