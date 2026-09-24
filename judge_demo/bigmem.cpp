#include <iostream>
using namespace std;

int main()
{
    const long long MB = 1024LL * 1024;

    for (int i = 1; i <= 8; i++)
    {
        char *p = new char[100 * MB];
        for (long long j = 0; j < 100 * MB; j += 4096)
            p[j] = 1; 
        cerr << "using " << i * 100 << " MB\n";
    }

    int a, b;
    cin >> a >> b;
    cout << a + b << "\n";
    return 0;
}