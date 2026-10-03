-- OJ 一期：建表脚本
-- 用法：sudo mysql oj < schema.sql

-- 提交记录
CREATE TABLE IF NOT EXISTS submissions (
    job_id      CHAR(8)      NOT NULL,
    status      VARCHAR(16)  NOT NULL,
    result      MEDIUMTEXT   NULL,
    created_at  DATETIME     NOT NULL,
    PRIMARY KEY (job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 用户
CREATE TABLE IF NOT EXISTS users (
    user_id        INT AUTO_INCREMENT PRIMARY KEY,
    username       VARCHAR(32)  NOT NULL,
    password_hash  VARCHAR(255) NOT NULL,
    created_at     DATETIME     NOT NULL,
    UNIQUE KEY uniq_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
