-- 몬스터 기획 데이터 (data_monster, data_monster_spawn)
--   Setup-Database.ps1이 실행될 때마다 지우고 다시 만든다 -> 값을 바꾸려면 DB가 아니라 이 파일을 고치고 커밋
--   문장은 줄 끝의 ;로 끝낸다 (; 뒤에 주석 금지). Setup이 그 기준으로 나눠서 하나씩 실행
--   단위: 거리 m, 속도 m/s, 시간 초 (Unreal은 cm라서 클라에서 x100)

-- 자식(FK로 data_monster를 가리키는 쪽)부터 지워야 함
DROP TABLE IF EXISTS data_monster_spawn;
DROP TABLE IF EXISTS data_monster;

-- 몬스터 종류별 AI 수치 (전에 Monster.h에 상수로 있던 값)
-- lose_range가 detect_range보다 커야 경계에서 Chase/포기가 왔다 갔다 하지 않음
CREATE TABLE data_monster (
    monster_id    INT          NOT NULL PRIMARY KEY,
    name          VARCHAR(32)  NOT NULL,            -- 사람이 보는 용도 (서버는 아직 안 읽음)
    max_hp        INT          NOT NULL,            -- 스폰할 때 HP
    patrol_range  FLOAT        NOT NULL,            -- 스폰 기준 ±범위 안에서 배회
    patrol_speed  FLOAT        NOT NULL,
    idle_min_sec  FLOAT        NOT NULL,            -- 배회 사이 대기 시간
    idle_max_sec  FLOAT        NOT NULL,
    detect_range  FLOAT        NOT NULL,            -- 이 안에 플레이어가 들어오면 Chase
    stop_range    FLOAT        NOT NULL,            -- 타겟과 이만큼 가까우면 멈춤
    chase_speed   FLOAT        NOT NULL,
    lose_range    FLOAT        NOT NULL,            -- 타겟이 이보다 멀어지면 포기
    leash_range   FLOAT        NOT NULL,            -- 스폰에서 이보다 멀리 끌려오면 포기
    return_speed  FLOAT        NOT NULL,
    CONSTRAINT chk_monster_hp    CHECK (max_hp > 0),
    CONSTRAINT chk_monster_idle  CHECK (idle_min_sec <= idle_max_sec),
    CONSTRAINT chk_monster_lose  CHECK (lose_range > detect_range),
    CONSTRAINT chk_monster_stop  CHECK (stop_range < detect_range),
    CONSTRAINT chk_monster_speed CHECK (patrol_speed > 0 AND chase_speed > 0 AND return_speed > 0)
) ENGINE=InnoDB;

-- 어디에 어떤 몬스터가 나오나 (플레이어 입장 위치 (0, 0)에서 15m 넘게 떨어뜨림 -> 들어오자마자 쫓기지 않게)
CREATE TABLE data_monster_spawn (
    spawn_id    INT    NOT NULL PRIMARY KEY,
    monster_id  INT    NOT NULL,
    pos_x       FLOAT  NOT NULL,
    pos_z       FLOAT  NOT NULL,
    CONSTRAINT fk_spawn_monster FOREIGN KEY (monster_id) REFERENCES data_monster (monster_id)
) ENGINE=InnoDB;

INSERT INTO data_monster
    (monster_id, name, max_hp, patrol_range, patrol_speed, idle_min_sec, idle_max_sec,
     detect_range, stop_range, chase_speed, lose_range, leash_range, return_speed)
VALUES
    (1, 'Slime', 200, 5, 2, 1, 3, 8, 1.5, 4, 12, 20, 6);

INSERT INTO data_monster_spawn (spawn_id, monster_id, pos_x, pos_z) VALUES
    (1, 1, 15, 15),
    (2, 1, -20, 10),
    (3, 1, 0, -25);
