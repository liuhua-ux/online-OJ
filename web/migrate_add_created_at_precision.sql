-- 二期：FIX-16 提交时间改为微秒精度
-- 用法：sudo mysql --default-character-set=utf8mb4 oj < migrate_add_created_at_precision.sql
--
-- 原来 DATETIME 只到秒，同一秒的两条提交谁先谁后不确定，
-- 「最新一条」这类查询会随刷新换结果。
-- DATETIME(6) 保留微秒；老数据的微秒部分为 .000000，不影响。

ALTER TABLE submissions MODIFY created_at DATETIME(6) NOT NULL;
