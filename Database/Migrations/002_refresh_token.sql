-- 유저 데이터: Refresh Token (OutGameServer의 /auth/refresh, /auth/logout)
--   토큰 원문은 저장하지 않고 SHA-256만 저장 -> DB가 털려도 그 값으로는 refresh를 못 함
--   회전(rotation): refresh에 쓴 토큰은 폐기(revoked_at)하고, 새로 발급한 토큰의 해시를 replaced_by에 남김
--   이미 폐기된 토큰이 다시 오면 도난으로 보고 그 계정의 살아 있는 토큰을 전부 폐기 (재사용 감지)
CREATE TABLE refresh_token (
    token_hash   BINARY(32)  NOT NULL PRIMARY KEY,
    account_id   BIGINT      NOT NULL,
    expires_at   DATETIME    NOT NULL,
    created_at   DATETIME    NOT NULL DEFAULT (UTC_TIMESTAMP()),
    revoked_at   DATETIME    NULL,
    replaced_by  BINARY(32)  NULL,
    INDEX idx_refresh_account (account_id),
    CONSTRAINT fk_refresh_account FOREIGN KEY (account_id) REFERENCES account (account_id)
) ENGINE=InnoDB;
