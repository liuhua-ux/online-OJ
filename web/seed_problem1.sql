-- 把仓库里原有的那套固定测试点，正式登记成第 1 题
-- 用法：sudo mysql --default-character-set=utf8mb4 oj < seed_problem1.sql
--
-- 时限/内存填的是「判题器里写死的值」：judge.cpp 里 alarm(2) = 2 秒，
-- MLE 阈值 256MB。这样至少值是对得上的（见 FIX-17：限制还没真正传进判题器）。

INSERT INTO problems
    (problem_id, title, description, input_format, output_format,
     sample_input, sample_output, time_limit_ms, memory_limit_mb,
     is_published, is_deleted, created_at)
VALUES
    (1,
     'A+B 问题',
     '读入两个整数 a 和 b，输出它们的和。',
     '一行，两个整数 a 和 b，中间用一个空格隔开。',
     '一个整数，表示 a + b 的值。',
     '10 20',
     '30',
     2000, 256, 1, 0, NOW());

-- 文件已经在磁盘上：judge_demo/testdata/1/{1,2,3}.{in,ans}
-- 这里只是把「有几组、在哪、多大」登记下来，内容不在库里
INSERT INTO testcases
    (problem_id, ordinal, input_path, answer_path, input_size, answer_size)
VALUES
    (1, 1, '1/1.in', '1/1.ans', 6, 3),
    (1, 2, '1/2.in', '1/2.ans', 8, 4),
    (1, 3, '1/3.in', '1/3.ans', 4, 3);
