-- 迁移：给**已经存在的**库加 problem_id 列
-- 用法：sudo mysql oj < migrate_add_problem_id.sql
--
-- 为什么不能只改 schema.sql：
--   schema.sql 里是 CREATE TABLE IF NOT EXISTS —— 表已经在了，那句就什么都不做。
--   schema.sql 管的是「将来新建一个库长什么样」；这个文件管的是「这个库现在怎么变过去」。

ALTER TABLE submissions
    ADD COLUMN problem_id INT NULL AFTER username;

ALTER TABLE submissions
    ADD CONSTRAINT fk_submission_problem FOREIGN KEY (problem_id)
        REFERENCES problems(problem_id);
