-- 迁移：给 users 表加「管理员」标志
-- 用法：sudo mysql oj < migrate_add_is_admin.sql
--
-- schema.sql 只管「将来新建的库」，对已经存在的库什么都不做 —— 见 NOTES.md §9。

ALTER TABLE users
    ADD COLUMN is_admin TINYINT NOT NULL DEFAULT 0 AFTER password_hash;

-- 把 testuser 提为管理员，方便测试（文档 OJ-USR-01 明确：不支持自助注册管理员）
UPDATE users SET is_admin = 1 WHERE username = 'testuser';
