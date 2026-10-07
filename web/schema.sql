-- OJ 一期：建表脚本
-- 用法：sudo mysql oj < schema.sql

-- 提交记录
CREATE TABLE IF NOT EXISTS submissions (
    job_id      CHAR(8)      NOT NULL,
    username    VARCHAR(32)  NULL,
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

-- 题目
CREATE TABLE IF NOT EXISTS problems (
    problem_id       INT AUTO_INCREMENT PRIMARY KEY,
    title            VARCHAR(128) NOT NULL,
    description      MEDIUMTEXT   NOT NULL,   -- 题面正文
    input_format     TEXT         NOT NULL,   -- 输入格式说明
    output_format    TEXT         NOT NULL,   -- 输出格式说明
    sample_input     TEXT         NOT NULL,   -- 样例输入
    sample_output    TEXT         NOT NULL,   -- 样例输出
    time_limit_ms    INT          NOT NULL,   -- 时间限制（毫秒）
    memory_limit_mb  INT          NOT NULL,   -- 内存限制（MB）
    is_published     TINYINT      NOT NULL DEFAULT 0,   -- 0=草稿 1=已发布
    is_deleted       TINYINT      NOT NULL DEFAULT 0,   -- 软删除
    created_at       DATETIME     NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 测试点元信息。内容不在这里，落在磁盘 judge_demo/testdata/<problem_id>/<序号>.in|.ans
CREATE TABLE IF NOT EXISTS testcases (
    testcase_id  INT AUTO_INCREMENT PRIMARY KEY,
    problem_id   INT          NOT NULL,
    ordinal      INT          NOT NULL,   -- 第几个测试点，从 1 连续，不许跳号
    input_path   VARCHAR(255) NOT NULL,   -- 相对路径，如 1/1.in
    answer_path  VARCHAR(255) NOT NULL,   -- 相对路径，如 1/1.ans
    input_size   INT          NOT NULL,   -- 字节
    answer_size  INT          NOT NULL,   -- 字节
    UNIQUE KEY uniq_problem_ordinal (problem_id, ordinal),
    CONSTRAINT fk_testcase_problem FOREIGN KEY (problem_id)
        REFERENCES problems(problem_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
