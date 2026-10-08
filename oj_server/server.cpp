#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
using namespace std;

int main()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 10) == -1)
    {
        perror("listen");
        return 1;
    }

    cout << "listening on 8080.....\n"
         << flush;
    while (true)
    {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd == -1)
        {
            perror("accept");
            continue;
        }

        char buf[4096];
        ssize_t n = read(conn_fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            cout << "=== 收到请求 ===\n"
                 << buf << "\n"
                 << flush;
        }

        string body = "Hello, OJ\n";
        string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " +
            to_string(body.size()) + "\r\n"
            "\r\n" 
            + body;
        write(conn_fd, resp.c_str(), resp.size());
    }

    return 0;
}
