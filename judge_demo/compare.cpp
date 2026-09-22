#include <iostream>
#include <fstream>
#include <string>
using namespace std;

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		cerr << "Usage: ./compare <fileA> <fileB>\n";
		return 1;
	}
	ifstream got(argv[1]);
	ifstream want(argv[2]);
	string a, b, line;

	if (!got.is_open())
	{
		cerr << "Cannot open: " << argv[1] << "\n";
		return 1;
	}
	if (!want.is_open())
	{
		cerr << "Cannot open: " << argv[2] << "\n";
		return 1;
	}

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
	}
	else
	{
		cout << "WA\n";
	}
}