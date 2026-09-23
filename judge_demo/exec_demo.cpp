#include <iostream>
#include <unistd.h>      
#include <sys/wait.h>  
#include <string>
using namespace std;

int main()
{
    int fd[2];
    if(pipe(fd)==-1)
    {
        cerr<<"pipe failed\n";
        return 1;
    }

    pid_t pid = fork();

    if (pid == 0) 
    {
        close(fd[0]);
        dup2(fd[1],2);
        close(fd[1]);

        execlp("g++", "g++", "-o", "test", "test.cpp", (char*)NULL);

        cerr << "exec failed\n";
        return 1;
    }
    else 
    {
        close(fd[1]);

        string error;
        char buf[128];
        ssize_t n;
        while((n=read(fd[0],buf,sizeof(buf)-1))>0)
        {
            buf[n]='\0';
            error += buf;
        }
        close(fd[0]);

        int status;
        waitpid(pid, &status, 0); 

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            cout << "compile ok\n";
        }
        else
        {
            cout << "Compile Error (exit code " << WEXITSTATUS(status) << ")\n";
            cout<<"catch "<<error.size()<<" bytes"<<"\n";
            cout << "--------------------\n";
            cout << error;
            cout << "--------------------\n";
            return 1;
        }
        cout << "child done\n";
    }
}