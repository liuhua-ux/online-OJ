#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
using namespace std;

int main()
{
    int fd[2];
    if(pipe(fd) == -1)
    {
        cerr << "pipe failed\n";
        return 1;
    }

    pid_t pid=fork();
    if(pid==0)
    {
        close(fd[0]);
        string msg="hello from child\n";
        write(fd[1], msg.c_str(), msg.size());
        close(fd[1]);
        return 0;
    }
    else
    {
        close(fd[1]);
        string got;
        char buf[128];
        ssize_t n;
        while((n=read(fd[0],buf,sizeof(buf)-1))>0)
        {
            buf[n]='\0';
            got += buf;
        }
        close (fd[0]);
        int status;
        waitpid(pid,&status,0);
        cout << "父进程收到: " << got;
        cout << "子进程退出码: " << WEXITSTATUS(status) << endl;
    }
}