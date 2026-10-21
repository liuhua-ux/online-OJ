-- 二期：会话表（登录功能）
-- 用法：mysql oj < migrate_add_sessions.sql
--
-- 一期 Flask 用的是签名 cookie，服务端不存任何东西，
-- 所以这张表对一期**完全无影响** —— 两边互不读对方的会话。
--
-- 表里存的是「这张票对应谁」，不存密码、不存任何可以反推出密码的东西。

CREATE TABLE IF NOT EXISTS sessions (
    session_id  CHAR(32)     NOT NULL,   -- 32 位小写十六进制 = 128 bit。凭证，不能猜
    username    VARCHAR(32)  NOT NULL,   -- 这张票属于谁
    created_at  DATETIME     NOT NULL,   -- 什么时候发的票
    expires_at  DATETIME     NOT NULL,   -- 什么时候作废（查的时候要带上这个条件）
    PRIMARY KEY (session_id),
    KEY idx_expires (expires_at)         -- 将来清理过期行要扫这一列
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
