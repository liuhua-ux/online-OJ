#include <iostream>
using namespace std;

// 这个文件故意有一个语法错误：cin 那一行少了分号。
// 用途：判题器的 CE 回归用例。它永远编译不过，请勿"修好"。
int main()
{
    int a = 0, b = 0;
    cin >> a >> b
    cout << a + b << "\n";
    return 0;
}
