-- 몬스터 기획 데이터 (data_monster, data_monster_spawn)
--   Setup-Database.ps1이 실행될 때마다 지우고 다시 만든다 -> 값을 바꾸려면 DB가 아니라 이 파일을 고치고 커밋
--   문장은 줄 끝의 ;로 끝낸다 (; 뒤에 주석 금지). Setup이 그 기준으로 나눠서 하나씩 실행
--   단위: 거리 m, 속도 m/s, 시간 초 (Unreal은 cm라서 클라에서 x100)

-- 자식(FK로 data_monster를 가리키는 쪽)부터 지워야 함
DROP TABLE IF EXISTS data_monster_drop;
DROP TABLE IF EXISTS data_monster_spawn;
DROP TABLE IF EXISTS data_monster;

-- 몬스터 종류별 수치 (HP, AI, 공격 스킬)
-- lose_range가 detect_range보다 커야 경계에서 Chase/포기가 왔다 갔다 하지 않음
-- skill_id는 data_skill을 가리키지만 FK는 걸지 않음: Setup이 파일을 이름순으로 실행해서 이 파일이 skill.sql보다 먼저 돎
--   (FK가 있으면 처음엔 data_skill이 없어서, 다시 실행할 땐 skill.sql의 DROP이 참조 중이라 실패) -> 서버가 로드할 때 확인
CREATE TABLE data_monster (
    monster_id    INT          NOT NULL PRIMARY KEY,
    name          VARCHAR(32)  NOT NULL,            -- 사람이 보는 용도 (서버는 안 읽음)
    level         INT          NOT NULL,            -- 이름표에 보이는 레벨 (지금은 표시용. 경험치 계산에는 안 씀)
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
    skill_id      INT          NOT NULL,            -- 공격 스킬 (data_skill, 101번부터)
    exp_reward    INT          NOT NULL,            -- 처치한 사람(마지막 공격자)이 받는 경험치
    gold_min      INT          NOT NULL,            -- 바닥에 떨어지는 골드 범위 (둘 다 0이면 골드 없음)
    gold_max      INT          NOT NULL,
    CONSTRAINT chk_monster_level CHECK (level >= 1),
    CONSTRAINT chk_monster_hp    CHECK (max_hp > 0),
    CONSTRAINT chk_monster_idle  CHECK (idle_min_sec <= idle_max_sec),
    CONSTRAINT chk_monster_lose  CHECK (lose_range > detect_range),
    CONSTRAINT chk_monster_stop  CHECK (stop_range < detect_range),
    CONSTRAINT chk_monster_speed CHECK (patrol_speed > 0 AND chase_speed > 0 AND return_speed > 0),
    CONSTRAINT chk_monster_reward CHECK (exp_reward >= 0 AND gold_min >= 0 AND gold_min <= gold_max)
) ENGINE=InnoDB;

-- 어디에 어떤 몬스터가 나오나 (플레이어 입장 위치 (0, 0)에서 15m 넘게 떨어뜨림 -> 들어오자마자 쫓기지 않게)
--   respawn_ms는 종류(data_monster)가 아니라 스폰 자리마다: 같은 슬라임이라도 자리에 따라 다르게 둘 수 있게
CREATE TABLE data_monster_spawn (
    spawn_id    INT    NOT NULL PRIMARY KEY,
    monster_id  INT    NOT NULL,
    pos_x       FLOAT  NOT NULL,
    pos_z       FLOAT  NOT NULL,
    respawn_ms  INT    NOT NULL,            -- 죽은 뒤 같은 자리에 다시 나오기까지 (ms)
    CONSTRAINT chk_spawn_respawn CHECK (respawn_ms >= 1000),
    CONSTRAINT fk_spawn_monster FOREIGN KEY (monster_id) REFERENCES data_monster (monster_id)
) ENGINE=InnoDB;

-- 몬스터가 죽을 때 떨어뜨리는 아이템 (줄마다 따로 확률을 굴림 -> 여러 개가 같이 떨어질 수 있음)
--   item_id는 data_item(item.sql)을 가리키지만 FK는 걸지 않음 (skill_id와 같은 이유) -> 서버가 로드할 때 확인
CREATE TABLE data_monster_drop (
    monster_id       INT  NOT NULL,
    item_id          INT  NOT NULL,
    chance_permille  INT  NOT NULL,            -- 떨어질 확률 (1000분율: 500 = 50%)
    min_count        INT  NOT NULL,            -- 떨어질 때 개수 범위
    max_count        INT  NOT NULL,
    PRIMARY KEY (monster_id, item_id),
    CONSTRAINT chk_drop_chance CHECK (chance_permille > 0 AND chance_permille <= 1000),
    CONSTRAINT chk_drop_count  CHECK (min_count >= 1 AND min_count <= max_count),
    CONSTRAINT fk_drop_monster FOREIGN KEY (monster_id) REFERENCES data_monster (monster_id)
) ENGINE=InnoDB;

INSERT INTO data_monster
    (monster_id, name, level, max_hp, patrol_range, patrol_speed, idle_min_sec, idle_max_sec,
     detect_range, stop_range, chase_speed, lose_range, leash_range, return_speed, skill_id, exp_reward, gold_min, gold_max)
VALUES
    (1, 'Slime', 3, 200, 5, 1.5, 1, 3, 8, 1.5, 3.5, 12, 20, 6, 101, 20, 5, 15),
    (2, 'TestBoss', 30, 50000000, 1, 1, 2, 4, 15, 1.5, 2, 25, 30, 6, 102, 0, 0, 0);

-- TestBoss(2)는 스폰 줄이 없음: 부하 테스트(Run-Load.ps1 -Boss -> GameServer --load-boss 2)에서만 (0, 0)에 나옴
--   HP 5천만: 400명이 0.5초마다 10씩 때려도(초당 8,000) 측정 2분 동안 안 죽음. 경험치 / 골드 / 드랍 없음 (측정에 드랍이 안 섞이게)

INSERT INTO data_monster_spawn (spawn_id, monster_id, pos_x, pos_z, respawn_ms) VALUES
    (1, 1, 15, 15, 10000),
    (2, 1, -20, 10, 10000),
    (3, 1, 0, -25, 10000);

INSERT INTO data_monster_drop (monster_id, item_id, chance_permille, min_count, max_count) VALUES
    (1, 1001, 500, 1, 2),
    (1, 2001, 50, 1, 1);
