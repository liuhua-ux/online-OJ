#include <iostream>
#include <unistd.h>      
#include <sys/wait.h>  
using namespace std;

int main()
{
    pid_t pid = fork();

    if (pid == 0) 
    {
        execlp("g++", "g++", "-o", "test", "test.cpp", (char*)NULL);

        cerr << "exec failed\n";
        return 1;
    }
    else 
    {
        int status;
        waitpid(pid, &status, 0); 

        if (WIFEXITED(status) && WIFEXITED(status) == 0)
        {
            cout << "compile ok\n";
        }
        else
        {
            cout << "Compile Error (exit code " << WEXITSTATUS(status) << ")\n";
            return 1;
        }
        cout << "child done\n";
    }
}