#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <fcntl.h>
#include <csignal>
#include <sys/resource.h>
#include <ctime>
using namespace std;

// 编译 src 到 ./test
// 成功：打印 compile ok，返回 true
// 失败：打印编译器输出，返回 false
bool compile(string src)
{
    int fd[2];
    if (pipe(fd) == -1)
    {
        cerr << "pipe failed\n";
        return false;
    }

    pid_t pid = fork();

    if (pid == 0)
    {
        close(fd[0]);
        dup2(fd[1], 2);
        close(fd[1]);

        execlp("g++", "g++", "-o", "test", src.c_str(), (char *)NULL);

        cerr << "exec failed\n";
        _exit(1);
    }

    close(fd[1]);

    string error;
    char buf[128];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        error += buf;
    }
    close(fd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        cout << "compile ok\n";
        return true;
    }

    cout << "Compile Error (exit code " << WEXITSTATUS(status) << ")\n";
    cout << "catch " << error.size() << " bytes\n";
    cout << "--------------------\n";
    cout << error;
    cout << "--------------------\n";
    return false;
}

// 读 /proc/<pid>/status 里的 VmHWM（峰值 RSS，单位 kB）
// 读不到（进程不存在 / 没有这一行 / 文件打不开）→ 返回 -1
long read_vmhwm_kb(pid_t pid)
{
    string road = "/proc/" + to_string(pid) + "/status";
    string line;
    ifstream got(road);
    if (!got.is_open())
    {
        cerr << "Cannot open: " << road << "\n";
        return -1;
    }
    while (getline(got, line))
    {
        if (line.find("VmHWM:") != string::npos)
        {
            size_t pos = line.find_first_of("0123456789");
            if (pos == string::npos)
            {
                return -1;
            }
            return stoi(line.substr(pos));
        }
    }
    return -1;
}

bool is_child_exec_done(pid_t pid)
{
    string path = "/proc/" + to_string(pid) + "/comm";
    ifstream f(path);
    if (!f.is_open())
    {
        return false; // 进程没了/还没建好，当「没准备好」
    }
    string comm;
    getline(f, comm); // "test\n" → getline 把 \n 吃掉
    return comm == "test";
}

// 跑一组数据并判定
// 返回结论字符串："AC" / "WA" / "TLE" / "MLE" / "RE"（具体原因函数内已打印）
string run_one_test(string in_path, string ans_path, int tl_ms, int ml_mb)
{
    pid_t run_pid = fork();
    if (run_pid == 0)
    {
        int in = open(in_path.c_str(), O_RDONLY);
        int out = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (in == -1 || out == -1)
        {
            cerr << "open failed";
            _exit(1); // 子进程 ①
        }
        dup2(in, 0);
        dup2(out, 1);
        close(in);
        close(out);

        struct rlimit lim;
        lim.rlim_cur = 1024LL * 1024 * 1024;
        lim.rlim_max = 1024LL * 1024 * 1024;
        setrlimit(RLIMIT_AS, &lim);

        // 用户程序崩溃时不该在服务器上留 core 文件。
        // ⚠️ 本机 core_pattern 是管道模式（|/usr/share/apport/apport），而管道模式下
        // 内核不检查 RLIMIT_CORE —— 所以这条只在"文件模式"下有效。要让 RE 的 time_ms
        // 不被 apport 拖成近 1200ms，服务器还得把 core_pattern 改回普通文件（见 NOTES §39）。
        // （试过 prctl(PR_SET_DUMPABLE, 0)：execve 会把它重置成默认值，等于没设。）
        struct rlimit core = {0, 0};
        setrlimit(RLIMIT_CORE, &core);

        alarm((tl_ms + 999) / 1000);

        execlp("./test", "./test", (char *)NULL);
        cerr << "run exec failed\n";
        _exit(1); // 子进程 ②
    }

    int run_status;
    struct rusage ru;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool oom_killed = false;

    while (true)
    {
        pid_t r = wait4(run_pid, &run_status, WNOHANG, &ru);
        if (r == run_pid)
            break;
        if (r == -1)
            break;

        if (is_child_exec_done(run_pid)) // ← 2a 在这
        {
            long hwm = read_vmhwm_kb(run_pid);
            if (hwm > 0 && hwm > (long)ml_mb * 1024)
            {
                if (kill(run_pid, SIGKILL) == 0)
                    oom_killed = true;
            }
        }

        usleep(5000);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long cost_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    cout << "run done\n";
    cout << "peak memory: " << ru.ru_maxrss << " KB\n";
    cout << "time: " << cost_ms << " ms\n";

    // 判定优先级（FIX-02）：TLE → MLE → RE。
    // 被闹钟打断的程序没跑完，它的 ru_maxrss 只是"跑到一半时的峰值"，
    // 不能拿这个不完整的数去判它超内存 —— 否则"又死循环又吃内存"会被报成 MLE，
    // 用户看到的结论和他实际写错的地方对不上。
    if (oom_killed)
    {
        cout << "Memory Limit Exceeded (killed by monitor)\n";
        return "MLE";
    }

    if (WIFSIGNALED(run_status) && WTERMSIG(run_status) == SIGALRM)
    {
        cout << "Time Limit Exceeded\n";
        return "TLE";
    }

    if (ru.ru_maxrss > ml_mb * 1024)
    {
        cout << "Memory Limit Exceeded (" << ru.ru_maxrss << " KB)\n";
        return "MLE"; // 父进程 ①
    }

    if (WIFSIGNALED(run_status))
    {
        cout << "Runtime Error (signal " << WTERMSIG(run_status) << ")\n";
        return "RE";
    }

    ifstream got("output.txt");
    ifstream want(ans_path);
    string a, b, line;
    while (getline(got, line))
    {
        a += line + "\n";
    }
    while (getline(want, line))
    {
        b += line + "\n";
    }
    if (a == b)
    {
        cout << "AC\n";
        return "AC"; // AC
    }
    else
    {
        cout << "WA\n";
        return "WA"; // 父进程 ③
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cerr << "Usage: ./judge <source_file> <testdata_dir>\n";
        return 1;
    }

    int tl_ms = (argc > 3) ? atoi(argv[3]) : 2000;
    int ml_mb = (argc > 4) ? atoi(argv[4]) : 256;

    if (!compile(argv[1]))
    {
        cout << "VERDICT: CE\n";
        return 1;
    }
    int passed = 0;
    for (int i = 1;; i++)
    {
        string in_path = string(argv[2]) + "/" + to_string(i) + ".in";
        string ans_path = string(argv[2]) + "/" + to_string(i) + ".ans";
        ifstream check(ans_path); // 只读打开：容器里 testdata 是只读挂载，fstream 默认带写权限会打不开
        if (!check.is_open())
        {
            break;
        }
        cout << "Test " << i << "\n";

        string v = run_one_test(in_path, ans_path, tl_ms, ml_mb);
        if (v == "AC")
        {
            passed++;
        }
        else
        {
            cout << "VERDICT: " << v << "\n";
            return 1;
        }
    }

    if (passed == 0)
    {
        cout << "no test data found\n";
        cout << "VERDICT: NO_DATA\n";
        return 1;
    }
    cout << "all " << passed << " test passed\n";
    cout << "VERDICT: AC\n";
    return 0;
}