-- 레벨 기획 데이터 (data_level): 레벨마다 다음 레벨까지 필요한 경험치
--   Setup-Database.ps1이 실행될 때마다 지우고 다시 만든다 -> 값을 바꾸려면 DB가 아니라 이 파일을 고치고 커밋
--   문장은 줄 끝의 ;로 끝낸다 (; 뒤에 주석 금지). Setup이 그 기준으로 나눠서 하나씩 실행
--   규칙 (줄 사이 관계라 CHECK로는 못 막음 -> 서버가 불러올 때 확인)
--     level은 1부터 빠짐없이, 마지막 줄만 exp_to_next = 0 (= 최대 레벨, 경험치가 더 안 쌓임)

DROP TABLE IF EXISTS data_level;

CREATE TABLE data_level (
    level        INT  NOT NULL PRIMARY KEY,
    exp_to_next  INT  NOT NULL,            -- 이 레벨에서 다음 레벨까지 필요한 경험치 (0 = 최대 레벨)
    CONSTRAINT chk_level_level CHECK (level >= 1),
    CONSTRAINT chk_level_exp   CHECK (exp_to_next >= 0)
) ENGINE=InnoDB;

-- 대략 1.5배씩. 슬라임 경험치 20 기준으로 1 -> 2레벨이 슬라임 5마리
INSERT INTO data_level (level, exp_to_next) VALUES
    (1, 100),
    (2, 150),
    (3, 220),
    (4, 330),
    (5, 500),
    (6, 750),
    (7, 1100),
    (8, 1650),
    (9, 2500),
    (10, 0);
