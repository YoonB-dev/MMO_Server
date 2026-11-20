-- 유저 데이터: 계정 (OutGameServer가 가입/로그인에 씀)
--   Migrations는 Setup-Database.ps1이 번호 순서대로 "한 번만" 적용하고 schema_version에 기록 -> 절대 지우거나 다시 만들지 않음
--   이미 적용된 파일은 고치지 말고, 바꿀 게 있으면 새 번호 파일을 추가 (MySQL의 CREATE / ALTER는 되돌릴 수 없음)
--   시각은 전부 UTC (UTC_TIMESTAMP)

-- 비밀번호 원문은 저장하지 않음: PBKDF2-SHA256(비밀번호, salt, 반복 수) 결과만
--   salt: 계정마다 다른 랜덤 값 -> 같은 비밀번호라도 해시가 다르고, 미리 계산한 표로 역추적할 수 없음 (비밀 아님)
--   password_iterations: 해시할 때 쓴 반복 수 -> 나중에 기본값을 올려도 옛 계정은 그 값으로 확인
CREATE TABLE account (
    account_id          BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    login_id            VARCHAR(20)  NOT NULL,
    password_hash       BINARY(32)   NOT NULL,
    password_salt       BINARY(16)   NOT NULL,
    password_iterations INT          NOT NULL,
    created_at          DATETIME     NOT NULL DEFAULT (UTC_TIMESTAMP()),
    CONSTRAINT uq_account_login UNIQUE (login_id),
    CONSTRAINT chk_account_iterations CHECK (password_iterations > 0)
) ENGINE=InnoDB;
