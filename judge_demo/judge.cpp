#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <fcntl.h>
#include <csignal>
#include <sys/resource.h>
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

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        cerr << "Usage: ./judge <source_file> <testdata_dir>\n";
        return 1;
    }

    if(!compile(argv[1]))
    {
        return 1;
    }
    int passed = 0;
    for (int i = 1;; i++)
    {
        string in_path = string(argv[2]) + "/" + to_string(i) + ".in";
        string ans_path = string(argv[2]) + "/" + to_string(i) + ".ans";
        fstream check(ans_path);
        if (!check.is_open())
        {
            break;
        }
        cout << "Test " << i << "\n";

        pid_t run_pid = fork();
        if (run_pid == 0)
        {
            int in = open(in_path.c_str(), O_RDONLY);
            int out = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (in == -1 || out == -1)
            {
                cerr << "open failed";
                return 1;
            }
            dup2(in, 0);
            dup2(out, 1);
            close(in);
            close(out);

            struct rlimit lim;
            lim.rlim_cur = 1024LL * 1024 * 1024;
            lim.rlim_max = 1024LL * 1024 * 1024;
            setrlimit(RLIMIT_AS, &lim);

            alarm(2);

            execlp("./test", "./test", (char *)NULL);
            cerr << "run exec failed\n";
            return 1;
        }
        int run_status;
        struct rusage ru;
        wait4(run_pid, &run_status, 0, &ru);

        cout << "run done\n";
        cout << "peak memory: " << ru.ru_maxrss << " KB\n";

        if (ru.ru_maxrss > 256LL * 1024)
        {
            cout << "Memory Limit Exceeded (" << ru.ru_maxrss << " KB)\n";
            return 1;
        }

        if (WIFSIGNALED(run_status))
        {
            int sig = WTERMSIG(run_status);
            if (sig == SIGALRM)
            {
                cout << "Time Limit Exceeded\n";
            }
            else
            {
                cout << "Runtime Error (signal " << sig << ")\n";
            }
            return 1;
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
            passed++;
        }
        else
        {
            cout << "WA\n";
            return 1;
        }
    }
    if (passed == 0)
    {
        cout << "no test data found\n";
        return 1;
    }
    cout << "all " << passed << " test passed\n";
}
