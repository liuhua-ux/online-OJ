-- OJ 一期：提交记录表
-- 用法：sudo mysql oj < schema.sql

CREATE TABLE IF NOT EXISTS submissions (
    job_id      CHAR(8)      NOT NULL,
    status      VARCHAR(16)  NOT NULL,
    result      MEDIUMTEXT   NULL,
    created_at  DATETIME     NOT NULL,
    PRIMARY KEY (job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
