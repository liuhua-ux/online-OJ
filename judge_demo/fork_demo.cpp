#include <iostream>
#include <unistd.h>
using namespace std;

int main()
{
	pid_t pid = fork();
	if (pid == 0) 
	{
		cout << "I am the child\n";
	}
	else
	{
		cout << "I am the father,the child' ID is " << pid << "\n";
	}
}