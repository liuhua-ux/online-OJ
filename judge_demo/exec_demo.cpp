#include <iostream>
#include <fstream>
#include <unistd.h>      
#include <sys/wait.h>  
#include <string>
#include <fcntl.h>
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
            pid_t run_pid = fork();
            if(run_pid==0)
            {
                int in = open("input.txt",O_RDONLY);
                int out = open("output.txt",O_WRONLY|O_CREAT|O_TRUNC,0644);
                
                if(in == -1||out == -1)
                {
                    cerr<<"open failed";
                    return 1;
                }
                dup2(in,0);
                dup2(out,1);
                close(in);
                close(out);

                execlp("./test","./test",(char*)NULL);
                cerr<<"run exec failed\n";
                return 1;
            }
            waitpid(run_pid, &status,0);
            cout<<"run done\n";
            ifstream got("output.txt");
            ifstream want("expected.txt");
            string a,b,line;
            while(getline(got,line))
            {
                a += line+"\n";
            }
            while(getline(want,line))
            {
                b += line+"\n";
            }
            if(a==b)
            {
                cout<<"AC\n";
            }
            else
            {
                cout<<"WA\n";
            }
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