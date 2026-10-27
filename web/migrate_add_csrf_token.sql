-- 二期：FIX-19 CSRF 令牌
-- 用法：sudo mysql --default-character-set=utf8mb4 oj < migrate_add_csrf_token.sql
--
-- 每张会话票多一个令牌，登录时和票号一起签发。
-- 令牌**只放在页面里的隐藏字段**，不进 Cookie ——
-- 别的网站能让你带上 Cookie，但读不到你页面里的内容，所以拿不到令牌。

ALTER TABLE sessions ADD COLUMN csrf_token CHAR(32) NOT NULL DEFAULT '';

-- 迁移前已经签发的旧票没有令牌。用 MD5(票号 + 随机数) 补一个：
-- 不是密码学上最好的来源，但这只是一次性补票，且旧票本来就会在 24 小时内过期。
UPDATE sessions
   SET csrf_token = MD5(CONCAT(session_id, RAND(), NOW()))
 WHERE csrf_token = '';
