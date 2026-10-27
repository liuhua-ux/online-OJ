-- 二期：FIX-21 乐观锁版本号
-- 用法：sudo mysql --default-character-set=utf8mb4 oj < migrate_add_problem_version.sql
--
-- 编辑页把 version 一起带上；保存时 WHERE version = 表单里的版本号。
-- 影响 0 行 = 这道题在你打开页面之后被别人改过 → 拒绝并提示刷新。

ALTER TABLE problems ADD COLUMN version INT NOT NULL DEFAULT 1;
