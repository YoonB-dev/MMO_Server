-- 새 캐릭터 기본값: 시작 위치 (0, 0), HP/MP 가득. 아웃게임 서버는 account_id, nickname만 넣음
--   GameServer/Player.h의 SPAWN_X, SPAWN_Z, MAX_HP, MAX_MP와 같은 값이어야 함
ALTER TABLE player_character ALTER pos_x SET DEFAULT 0, ALTER pos_z SET DEFAULT 0, ALTER hp SET DEFAULT 100, ALTER mp SET DEFAULT 100;
