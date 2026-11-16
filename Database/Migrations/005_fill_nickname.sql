-- 이미 만들어 둔 테스트 계정의 캐릭터: Player + 계정 번호 (지금은 계정당 1줄이라 겹치지 않음)
--   새 캐릭터는 아웃게임 서버(POST /characters)에서 사용자가 정한 닉네임으로만 만들어짐
UPDATE player_character SET nickname = CONCAT('Player', account_id) WHERE nickname IS NULL;
