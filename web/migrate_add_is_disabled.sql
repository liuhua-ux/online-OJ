-- 给 users 加「禁用」标志（需求 OJ-SYS-01：管理员可禁用账号）
--
-- 为什么加一列、而不是把用户删掉：
-- 需求原文写着「禁用账号后该用户**不能登录**，但其**历史提交记录保留**」——
-- 行必须留着。删人就等于让他所有提交变成孤儿记录
-- （跟题目用软删除是同一个道理）。
--
-- 用法：sudo mysql oj < migrate_add_is_disabled.sql

ALTER TABLE users
    ADD COLUMN is_disabled TINYINT NOT NULL DEFAULT 0;   -- 0=正常 1=已禁用
