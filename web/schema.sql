-- OJ 一期：建表脚本
-- 用法：sudo mysql oj < schema.sql
--
-- ⚠️ 两件必须知道的事：
-- 1. 这里的 CREATE TABLE IF NOT EXISTS 只对「还没有这些表」的新库生效。
--    对已经建好的库，改这个文件**不会**改数据库 —— 那要靠 ALTER TABLE
--    （见 migrate_add_problem_id.sql）。
-- 2. 顺序有讲究：带外键的表必须排在它引用的表**后面**，否则建表会失败。

-- 用户
CREATE TABLE IF NOT EXISTS users (
    user_id        INT AUTO_INCREMENT PRIMARY KEY,
    username       VARCHAR(32)  NOT NULL,
    password_hash  VARCHAR(255) NOT NULL,
    is_admin       TINYINT      NOT NULL DEFAULT 0,   -- 0=学生 1=管理员；不支持自助注册管理员
    is_disabled    TINYINT      NOT NULL DEFAULT 0,   -- 0=正常 1=已禁用（禁用后不能登录，历史提交保留）
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

-- 提交记录
-- 必须排在 problems 后面 —— 它有外键指过去
CREATE TABLE IF NOT EXISTS submissions (
    job_id      CHAR(8)      NOT NULL,
    username    VARCHAR(32)  NULL,
    problem_id  INT          NULL,   -- NULL 只表示「这一列引入之前的老数据」
    status      VARCHAR(16)  NOT NULL,
    result      MEDIUMTEXT   NULL,   -- 判题器原始输出（出怪问题时唯一的现场）
    created_at  DATETIME     NOT NULL,
    -- ⚠️ FIX-06 这几列在实际库里排在 created_at **之后** ——
    -- 因为 ALTER TABLE ADD COLUMN 默认往**最后**加，不认你心里想的顺序。
    -- 这里跟实际库保持一致，免得「新库」和「老库」列顺序不同。
    verdict     VARCHAR(8)   NULL,   -- AC/WA/TLE/MLE/RE/CE/NO_DATA；老数据为 NULL
    passed      INT          NULL,   -- 过了几组
    total       INT          NULL,   -- 共几组
    time_ms     INT          NULL,   -- 最慢那一组的耗时（毫秒）
    memory_kb   INT          NULL,   -- 各组峰值内存里的最大值（KB）
    PRIMARY KEY (job_id),
    CONSTRAINT fk_submission_problem FOREIGN KEY (problem_id)
        REFERENCES problems(problem_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
