#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <map>
#include <string>
#include <fstream>
#include <climits>
#include <cstdlib>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>

using namespace std;

const map<string, string> content_types = {
    {"html", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "application/javascript; charset=utf-8"},
    {"png", "image/png"},
    {"", "application/octet-stream"},
};

void send_response(int conn_fd, int code, const string &body, const string &content_type = "text/plain; charset=utf-8")
{
    string reason;
    if (code == 200)
    {
        reason = "OK";
    }
    else if (code == 404)
    {
        reason = "Not Found";
    }
    else if (code == 500)
    {
        reason = "Internal Server Error";
    }
    else
    {
        reason = "Unknown";
    }

    string resp = "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n"
                                                                 "Content-Type: " +
                  content_type + "\r\n"
                                 "Content-Length: " +
                  to_string(body.size()) + "\r\n"
                                           "\r\n" +
                  body;

    write(conn_fd, resp.c_str(), resp.size());
}

int hexval(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    else
    {
        return -1;
    }
}

string urldecode(const string &s, bool plus_is_space = true)
{
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (plus_is_space && s[i] == '+')
        {
            out += ' ';
        }
        else if (s[i] == '%' && i + 2 < s.size() &&
                 hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0)
        {
            out += (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

map<string, string> parse_form(const string &body)
{
    map<string, string> form;
    istringstream iss(body);
    string pair;
    while (getline(iss, pair, '&'))
    {
        size_t eq = pair.find('=');
        if (eq == string::npos)
        {
            continue;
        }
        string key = pair.substr(0, eq);
        string val = pair.substr(eq + 1);
        form[urldecode(key)] = urldecode(val);
    }
    return form;
}

string html_escape(const string &page)
{
    string out;
    for (size_t i = 0; i < page.size(); i++)
    {
        if (page[i] == '&')
        {
            out += "&amp;";
        }
        else if (page[i] == '<')
        {
            out += "&lt;";
        }
        else if (page[i] == '>')
        {
            out += "&gt;";
        }
        else if (page[i] == '"')
        {
            out += "&quot;";
        }
        else
        {
            out += page[i];
        }
    }
    return out;
}

void handle_request(int conn_fd, const string &req)
{
    size_t head_end = req.find("\r\n\r\n");
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

        cout << " body = [" << req.substr(head_end + 4, body_need) << "]\n";
    }
    else
    {
        cout << "no body" << "\n";
    }

    string body = req.substr(head_end + 4, body_need);
    map<string, string> form = parse_form(body);
    for (const auto &kv : form)
    {
        cout << "form: " << kv.first << " = " << kv.second << "\n";
    }

    size_t total_len = head_end + 4 + body_need;
    if (req.size() > total_len)
    {
        cout << "粘包：多出 " << req.size() - total_len << " 字节，属于下一个请求（本步不处理）\n";
    }

    if (target == "/")
    {
        ifstream f("www/index.html");
        stringstream ss;
        ss << f.rdbuf();
        string home = ss.str();
        send_response(conn_fd, 200, home, "text/html; charset=utf-8");
    }
    else if (target == "/echo")
    {
        if (method == "POST")
        {
            stringstream page;
            page << "<!DOCTYPE html>\n"
                 << "<html lang=\"zh-CN\">\n"
                 << "<head>\n"
                 << "<meta charset=\"utf-8\">\n"
                 << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                 << "<title>回显结果 · 自研 OJ</title>\n"
                 << "<link rel=\"stylesheet\" href=\"/style.css\">\n"
                 << "</head>\n"
                 << "<body>\n"
                 << "<header class=\"nav\">\n"
                 << "<a class=\"brand\" href=\"/\">自研 OJ</a>\n"
                 << "<nav>\n"
                 << "<a href=\"/\">首页</a>\n"
                 << "<a href=\"/about.html\">关于</a>\n"
                 << "<a href=\"/echo\">回显测试</a>\n"
                 << "</nav>\n"
                 << "</header>\n"
                 << "<main class=\"wrap\">\n"
                 << "<h1>回显结果</h1>\n"
                 << "<p class=\"lead\">服务器把你填的字原样拼了回来 —— 并且已经转义。</p>\n"
                 << "<div class=\"form\">\n"
                 << "<p>名字：" << html_escape(form["name"]) << "</p>\n"
                 << "<p>留言：" << html_escape(form["msg"]) << "</p>\n"
                 << "</div>\n"
                 << "<p class=\"note\"><a href=\"/echo\">再来一次</a></p>\n"
                 << "</main>\n"
                 << "<footer class=\"foot\">二期 C++ 自研服务端 · 正在学 socket 与 HTTP</footer>\n"
                 << "</body></html>\n";
            send_response(conn_fd, 200, page.str(), "text/html; charset=utf-8");
        }
        else
        {
            ifstream f("www/echo.html");
            stringstream ss;
            ss << f.rdbuf();
            string echo = ss.str();
            send_response(conn_fd, 200, echo, "text/html; charset=utf-8");
        }
    }
    else
    {
        target = urldecode(target, false);
        string road = "www" + target;

        char root_buf[PATH_MAX];
        char full_buf[PATH_MAX];
        char *root_p = realpath("www", root_buf);
        char *full_p = realpath(road.c_str(), full_buf);

        string root, real;
        if (root_p != NULL)
        {
            root = root_buf;
        }
        if (full_p != NULL)
        {
            real = full_buf;
        }

        bool ok = !root.empty() && real.compare(0, root.size() + 1, root + "/") == 0;
        if (!ok)
        {
            send_response(conn_fd, 404, "请输入正确的网址\n");
        }

        else
        {
            ifstream f(road);
            string ext;
            size_t pos = road.rfind('.');

            if (pos != string::npos)
            {
                ext = road.substr(pos + 1, road.size() - pos);
            }
            auto it = content_types.find(ext);
            string type = (it != content_types.end()) ? it->second : content_types.at("");

            if (f.is_open())
            {
                stringstream ss;
                ss << f.rdbuf();
                string resource = ss.str();
                send_response(conn_fd, 200, resource, type);
            }
            else
            {
                send_response(conn_fd, 404, "请输入正确的网址\n");
            }
        }
    }
}

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

    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    int epfd = epoll_create1(0);

    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[64];
    map<int, string> conn_bufs;
    while (true)
    {
        int n = epoll_wait(epfd, events, 64, -1);

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == listen_fd)
            {
                while (true)
                {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd == -1)
                    {
                        break;
                    }

                    fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) | O_NONBLOCK);
                    ev.events = EPOLLIN;
                    ev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);

                    cout << "接到新连接 fd = " << conn_fd << "\n"
                         << flush;
                }
            }
            else
            {
                char buf[4096];
                ssize_t cnt = read(fd, buf, sizeof(buf));

                if (cnt > 0)
                {
                    conn_bufs[fd].append(buf, cnt);
                    cout << "fd " << fd << " 收到 " << cnt << " 字节，累积 " << conn_bufs[fd].size() << " 字节" << endl;
                }
                else if (cnt == 0)
                {
                    cout << "fd " << fd << " 对方关闭，清理" << endl;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    conn_bufs.erase(fd);
                    close(fd);
                }
                else
                {
                    if(errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        perror("read");
                    }
                }
            }
        }
    }
    return 0;
} 
