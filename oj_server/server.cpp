#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <map>
#include <string>
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
        string req;
        size_t head_end = string::npos;
        while (true)
        {
            ssize_t n = read(conn_fd, buf, sizeof(buf));
            if (n == 0)
            {
                break;
            }

            if (n < 0)
            {
                cout << "don't connect" << "\n";
                break;
            }

            req.append(buf, n);
            head_end = req.find("\r\n\r\n");
            if (head_end != string::npos)
            {
                    cout << "=== request received ===\n"
                         << req << "\n"
                         << flush;
                    break;
            }
        }

        istringstream iss(req);
        string method, target, version;
        iss >> method >> target >> version;
        cout << "method  = " << method << endl;
        cout << "target  = " << target << endl;
        cout << "version = " << version << endl;

        string line;
        string length;
        getline(iss, line);
        map<string, string> headers;
        while (getline(iss, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                break;
            }
            size_t pos = line.find(':');
            if (pos == string::npos)
                continue;

            string name = line.substr(0, pos);
            string value = line.substr(pos + 1);
            if (!value.empty() && value.front() == ' ')
            {
                value.erase(0, 1);
            }

            headers[name] = value;
            cout << "[" << line << "]   长度 = " << line.size() << endl;
        }

        for (const auto &kv : headers)
        {
            cout << kv.first << "=" << kv.second << "\n";
        }

        size_t body_need = 0;
        if (headers.count("Content-Length"))
        {
            length = headers["Content-Length"];
            cout << " body length = " << length << "\n";
            body_need = stoi(length);
            while (req.size() < head_end + 4 + body_need)
            {
                ssize_t n = read(conn_fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                req.append(buf, n);
            }
            cout << " body = [" << req.substr(head_end + 4, body_need) << "]\n";
        }
        else
        {
            cout << "no body" << "\n";
        }

        size_t total_len = head_end + 4 + body_need;
        if(req.size()>total_len)
        {
            cout << "粘包：多出 " << req.size()-total_len << " 字节，属于下一个请求（本步不处理）\n";
        }

        string body = "Hello, OJ\n";
        string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " +
            to_string(body.size()) + "\r\n"
                                     "\r\n" +
            body;
        write(conn_fd, resp.c_str(), resp.size());
        close(conn_fd);
    }

    return 0;
}
