-- 2026-10-22 二期：保存提交的源码
--
-- 动机有两个：
--   1. 判完就 rmtree 临时目录 → 用户再回到题目页，刚才写的代码没了（浏览器不保证恢复表单）
--   2. 需求 OJ-REC-01 要求结果页展示源码，原先源码没落库，这条一直是 ☆
--
-- 老数据为 NULL（当时没存）；展示时按「（老数据·无源码）」处理。

ALTER TABLE submissions
    ADD COLUMN source MEDIUMTEXT NULL AFTER memory_kb;
