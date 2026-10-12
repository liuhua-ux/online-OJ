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
using namespace std;

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

string urldecode(const string &s)
{
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '+')
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
    for(size_t i = 0;i<page.size();i++)
    {
        if(page[i]=='&')
        {
            out += "&amp;";
        }
        else if(page[i]=='<')
        {
            out += "&lt;";
        }
        else if(page[i]=='>')
        {
            out += "&gt;";
        }
        else if(page[i]=='"')
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

    map<string, string> types;
    types["html"] = "text/html; charset=utf-8";
    types["css"] = "text/css; charset=utf-8";
    types["js"] = "application/javascript; charset=utf-8";
    types["png"] = "image/png";
    types[""] = "application/octet-stream";

    while (true)
    {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd == -1)
        {
            perror("accept");
            continue;
        }

        // 暂时处理堵死
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

        if (head_end == string::npos)
        {
            close(conn_fd);
            continue;
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
                     << "<html><head><meta charset=\"utf-8\"><title>回显</title></head>\n"
                     << "<body>\n"
                     << "<h1>回显结果</h1>\n"
                     << "<p>名字：" << html_escape(form["name"]) << "</p>\n"
                     << "<p>留言：" << html_escape(form["msg"]) << "</p>\n"
                     << "<p><a href=\"/echo\">再来一次</a></p>\n"
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
                string type;

                if (pos != string::npos)
                {
                    ext = road.substr(pos + 1, road.size() - pos);
                }
                if (types.count(ext))
                {
                    type = types[ext];
                }
                else
                {
                    type = types[""];
                }
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
        close(conn_fd);
    }
    return 0;
}
