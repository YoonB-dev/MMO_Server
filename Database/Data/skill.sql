-- 스킬 기획 데이터 (data_skill)
--   Setup-Database.ps1이 실행될 때마다 지우고 다시 만든다 -> 값을 바꾸려면 DB가 아니라 이 파일을 고치고 커밋
--   문장은 줄 끝의 ;로 끝낸다 (; 뒤에 주석 금지). Setup이 그 기준으로 나눠서 하나씩 실행
--   단위: 시간 ms, 거리 m (Unreal은 cm라서 클라에서 x100)
--   skill_id는 클라도 같은 번호를 씀 (C_SKILL.skill_id)

DROP TABLE IF EXISTS data_skill;

-- 테이블 사이 규칙 (테이블이 달라서 CHECK로는 못 막음)
--   기본 공격 range_m > data_monster.stop_range: 짧으면 쫓아와서 멈춘 몬스터를 못 때림
--   원거리 range_m + 서버 여유 1m < data_monster.lose_range: 길면 맞은 몬스터가 쫓기 시작하자마자 포기함
CREATE TABLE data_skill (
    skill_id     INT          NOT NULL PRIMARY KEY,
    name         VARCHAR(32)  NOT NULL,            -- 사람이 보는 용도 (서버는 안 읽음)
    cooldown_ms  INT          NOT NULL,            -- 서버 틱(50ms)보다 짧으면 의미 없음
    range_m      FLOAT        NOT NULL,            -- 서버 위치 기준. 서버가 지연 여유 1m를 더해서 검사
    damage       INT          NOT NULL,
    mp_cost          INT          NOT NULL,            -- 쓸 때 드는 MP (기본 공격은 0)
    requires_target  TINYINT      NOT NULL,            -- 1: 대상이 꼭 있어야 함 (없거나 잘못되면 거절), 0: 빗나가도 나감 (허공 공격)
    CONSTRAINT chk_skill_cooldown CHECK (cooldown_ms >= 50),
    CONSTRAINT chk_skill_range    CHECK (range_m > 0),
    CONSTRAINT chk_skill_damage   CHECK (damage >= 0),
    CONSTRAINT chk_skill_mp       CHECK (mp_cost >= 0),
    CONSTRAINT chk_skill_target   CHECK (requires_target IN (0, 1))
) ENGINE=InnoDB;

INSERT INTO data_skill (skill_id, name, cooldown_ms, range_m, damage, mp_cost, requires_target) VALUES
    (1, 'BasicAttack', 500, 2, 10, 0, 0),
    (2, 'RangedShot', 5000, 10, 30, 10, 1);
