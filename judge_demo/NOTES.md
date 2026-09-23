# 判题器笔记（设计与进展）

> 记录核心设计决策与实测结论，可直接用作实验报告素材。

## 当前能力

    ./judge <源文件> <输入文件> <标准答案>

五个判定全通：CE / AC / WA / TLE / RE

## 整体结构

    编译一次 ──→ 运行 N 次（当前 N = 1）──→ 比对
    fork + exec g++          fork + exec ./test
    pipe + dup2(→2)          open + dup2(→0, 1)
    读报错判 CE              判 TLE / RE / AC / WA

## 关键设计决策

### 1. 抓 g++ 报错（pipe + dup2）

- 子进程：close(fd[0]) → dup2(fd[1], 2) → close(fd[1]) → exec g++
- 父进程：先 close(fd[1])，再 read 到 EOF
- **父进程必须先 close(fd[1])**：不关的话 read 永远等不到 EOF，程序卡死（已实测）
- **read 必须写在 waitpid 之前**：否则报错超过管道容量（约 64KB）时双向死锁
- read 会"短读"，所以要 while 循环；缓冲区留一格给结束符，用 sizeof(buf) - 1

### 2. 喂输入 / 收输出（open + dup2）

- 子进程：open 输入（只读）与输出（写入 + 创建 + 清空），再 dup2 到 0 号和 1 号
- 用文件而不用管道：避免 64KB 死锁；天然对接比对逻辑；调试时可直接 cat output.txt

### 3. 超时（alarm + 信号）

- alarm 只有一处位置正确：**子进程里、exec 之前**
- fork **不继承**闹钟（实测：父进程 alarm(2) 到点被杀，子进程活满 5 秒）
- exec **保留**闹钟（实测：exec 成 sleep(10) 的程序，1.003 秒被 SIGALRM 杀死）

### 4. 判定"被信号杀死"

- WIFEXITED 为真 → 正常退出，用 WEXITSTATUS 取退出码
- WIFSIGNALED 为真 → 被信号杀死，用 WTERMSIG 取信号号
- SIGALRM(14) → TLE；SIGSEGV(11) → RE
- 注意：WIFEXITED 为假时，WEXITSTATUS 取出的值毫无意义

### 5. 比对

- 逐行读，每行末尾补换行再拼接
- **不能丢行边界**：否则输出 "3\n0" 与答案 "30" 会被判成相等，误判 AC
- 对判题器来说，"误判 AC" 远比 "误判 WA" 严重（错的被当成对的，没人会发现）

## 下一步

1. 多测试点：testdata/1.in、1.ans、2.in …，编译一次运行 N 次，全过才 AC
2. 内存限制（setrlimit）
3. 拆函数（模块化）
4. Web + 数据库
