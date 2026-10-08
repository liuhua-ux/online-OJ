-- 给 submissions 加「结构化判题结果」字段（FIX-06 / 需求 OJ-REC-01）
--
-- 背景：submissions.result 存的是判题器的**原始输出文本**，页面要显示
-- 「通过 N/M 组、耗时、峰值内存」就只能靠解析文本；排行榜统计「谁通过了哪题」
-- 也只能去 LIKE 匹配 'all 3 test passed' —— 太脆。
-- 现在判题器会打一行 VERDICT，把这些解析出来存成字段，页面和排行榜直接用。
--
-- ⚠️ 引入这些列之前的老提交，这五列都是 NULL ——
--    表示「当时没记」，不是「没通过」。页面显示时要区别对待。
--
-- 用法：sudo mysql oj < migrate_add_verdict.sql

ALTER TABLE submissions
    ADD COLUMN verdict   VARCHAR(8) NULL,   -- AC / WA / TLE / MLE / RE / CE / NO_DATA
    ADD COLUMN passed    INT        NULL,   -- 过了几组
    ADD COLUMN total     INT        NULL,   -- 共几组
    ADD COLUMN time_ms   INT        NULL,   -- 最慢那一组的耗时（毫秒）
    ADD COLUMN memory_kb INT        NULL;   -- 各组峰值内存里的最大值（KB）
