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
#include <stdexcept>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <mysql/mysql.h>
#include "db_config.h"
#include <sys/wait.h>
#include <random>

using namespace std;

struct Task
{
    int conn_fd;
    string req;
};

queue<Task> task_queue;
mutex mtx;
condition_variable cv;

const char *JUDGE_BIN = "/home/ubuntu/online-OJ/judge_demo/judge";
const string PROBLEM_PREFIX = "/problem/";
const string SUBMIT_PREFIX = "/submit/";

const map<string, string>
    content_types = {
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
    else if (code == 400)
    {
        reason = "Bad Request";
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
                                           "Connection: close\r\n"
                                           "\r\n" +
                  body;

    write(conn_fd, resp.c_str(), resp.size());
}

string make_job_id()
{
    static const char *hex = "0123456789abcdef";
    thread_local random_device rd;

    string id;
    for (int i = 0; i < 8; i++)
    {
        id += hex[rd() % 16];
    }
    return id;
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

string page_head(const string &title)
{
    return "<!DOCTYPE html>\n"
           "<html lang=\"zh-CN\">\n"
           "<head>\n"
           "<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>" + html_escape(title) + " · 自研 OJ</title>\n"
           "<link rel=\"icon\" href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='8' fill='%231d4ed8'/></svg>\">\n"
           "<link rel=\"stylesheet\" href=\"/style.css\">\n"
           "</head>\n"
           "<body>\n"
           "<header class=\"nav\">\n"
           "  <a class=\"brand\" href=\"/\"><span class=\"brand-mark\" aria-hidden=\"true\">{ }</span>自研 OJ</a>\n"
           "  <nav>\n"
           "    <a href=\"/\">首页</a>\n"
           "    <a href=\"/problems\">题库</a>\n"
           "    <a href=\"/about.html\">关于</a>\n"
           "    <a href=\"/echo\">回显测试</a>\n"
           "  </nav>\n"
           "</header>\n"
           "<main class=\"wrap\">\n";
}

string page_tail()
{
    return "</main>\n"
           "<footer class=\"foot\">二期 C++ 自研服务端：socket、epoll、线程池</footer>\n"
           "</body>\n"
           "</html>\n";
}

string render_submit_form(int problem_id)
{
    return "<h2>提交代码</h2>\n"
           "<form class=\"editor\" method=\"POST\" action=\"/submit/" +
           to_string(problem_id) + "\">\n"
                                   "  <div class=\"editor-bar\"><span class=\"dot\" aria-hidden=\"true\"></span><span>submission.cpp</span><span class=\"lang\">C++17</span></div>\n"
                                   "  <textarea name=\"code\" spellcheck=\"false\" placeholder=\"在这里粘贴你的 C++ 代码\"></textarea>\n"
                                   "  <div class=\"editor-foot\">\n"
                                   "    <button type=\"submit\">提交判题</button>\n"
                                   "    <span class=\"hint\">提交后在后台编译、运行、比对</span>\n"
                                   "  </div>\n"
                                   "</form>\n";
}

size_t request_total_len(const string &req)
{
    size_t head_end = req.find("\r\n\r\n");
    if (head_end == string::npos)
        return string::npos;

    size_t total = head_end + 4;

    size_t pos = req.find("Content-Length:");
    if (pos != string::npos && pos < head_end)
    {
        size_t start = pos + string("Content-Length:").size();
        size_t end = req.find("\r\n", start);

        if (end != string::npos && end > start)
        {
            try
            {
                total += stoi(req.substr(start, end - start));
            }
            catch (const exception &)
            {
            }
        }
    }
    return total;
}

bool run_judge(const string &src_path, const string &data_dir, int tl_ms, int ml_mb, string &out)
{
    const int CHILD_NOT_STARTED = 127;

    size_t slash = src_path.rfind('/');
    string work_dir = src_path.substr(0, slash);

    int fd[2];
    if (pipe(fd) == -1)
    {
        cerr << "pipe failed: " << strerror(errno) << endl;
        return false;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        cerr << "fork failed: " << strerror(errno) << endl;
        close(fd[0]);
        close(fd[1]);
        return false;
    }

    if (pid == 0)
    {
        close(fd[0]);
        dup2(fd[1], 1);
        close(fd[1]);

        if (chdir(work_dir.c_str()) == -1)
        {
            _exit(CHILD_NOT_STARTED);
        }

        execl(JUDGE_BIN,
              "judge",
              src_path.c_str(),
              data_dir.c_str(),
              to_string(tl_ms).c_str(),
              to_string(ml_mb).c_str(),
              (char *)NULL);

        _exit(CHILD_NOT_STARTED);
    }

    close(fd[1]);
    out.clear();
    char buf[256];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof(buf))) > 0)
    {
        out.append(buf, n);
    }
    close(fd[0]);

    int status;
    if (waitpid(pid, &status, 0) == -1)
    {
        cerr << "waitpid failed: " << strerror(errno) << endl;
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) != CHILD_NOT_STARTED;
}

MYSQL *get_conn()
{
    thread_local MYSQL *conn = nullptr;

    if (conn == nullptr)
    {
        conn = mysql_init(nullptr);
        if (conn == nullptr)
        {
            return nullptr;
        }
        if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, DB_PORT, NULL, 0) == NULL)
        {
            mysql_close(conn);
            conn = nullptr;
            return nullptr;
        }
    }
    return conn;
}

string render_problems()
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return page_head("题库") + "<p class=\"empty\">数据库暂时不可用。</p>\n" + page_tail();
    }

    if (mysql_query(conn, "SELECT problem_id, title, time_limit_ms, memory_limit_mb "
                          "FROM problems WHERE is_deleted = 0 AND is_published = 1") != 0)
    {
        cerr << "query failed: " << mysql_error(conn) << endl;
        return page_head("题库") + "<p class=\"empty\">题库暂时读不出来。</p>\n" + page_tail();
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL)
    {
        return page_head("题库") + "<p class=\"empty\">题库暂时读不出来。</p>\n" + page_tail();
    }

    stringstream page;
    page << page_head("题库");
    page << "<header class=\"page-head\"><h1>题库</h1>"
            "<p class=\"lead\">挑一道题，写完代码交上来。</p></header>\n";
    page << "<div class=\"problem-list\">\n";

    int count = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL)
    {
        page << "<a class=\"problem-row\" href=\"/problem/" << row[0] << "\">"
             << "<span class=\"problem-id\">#" << row[0] << "</span>"
             << "<span class=\"problem-title\">" << html_escape(row[1]) << "</span>"
             << "<span class=\"problem-meta\">" << row[2] << " ms / " << row[3] << " MB</span>"
             << "</a>\n";
        count++;
    }

    page << "</div>\n";
    if (count == 0)
    {
        page << "<p class=\"empty\">暂无已发布的题目。</p>\n";
    }
    page << page_tail();

    mysql_free_result(res);
    return page.str();
}

bool render_problem(int problem_id, string &html)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT title, description, input_format, output_format, "
                 "sample_input, sample_output, time_limit_ms, memory_limit_mb "
                 "FROM problems WHERE problem_id = " +
                 to_string(problem_id) +
                 " AND is_deleted = 0 AND is_published = 1";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        cerr << "query failed: " << mysql_error(conn) << endl;
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL)
    {
        mysql_free_result(res);
        return false;
    }

    stringstream page;
    page << page_head(row[0]);
    page << "<p class=\"breadcrumb\"><a href=\"/problems\">题库</a> / 第 " << problem_id << " 题</p>\n";

    page << "<header class=\"problem-header\">\n";
    page << "<h1>" << html_escape(row[0]) << "</h1>\n";
    page << "<div class=\"chips\"><span class=\"chip\">时间 " << row[6] << " ms</span>"
            "<span class=\"chip\">内存 " << row[7] << " MB</span></div>\n";
    page << "</header>\n";

    page << "<div class=\"article\">\n";
    page << "<div class=\"statement\">" << html_escape(row[1]) << "</div>\n";

    page << "<h2>输入格式</h2>\n";
    page << "<div class=\"statement\">" << html_escape(row[2]) << "</div>\n";

    page << "<h2>输出格式</h2>\n";
    page << "<div class=\"statement\">" << html_escape(row[3]) << "</div>\n";

    page << "<h2>样例输入</h2>\n";
    page << "<pre>" << html_escape(row[4]) << "</pre>\n";

    page << "<h2>样例输出</h2>\n";
    page << "<pre>" << html_escape(row[5]) << "</pre>\n";

    page << render_submit_form(problem_id);
    page << "</div>\n";
    page << page_tail();

    html = page.str();

    mysql_free_result(res);
    return true;
}

bool get_problem_limits(int problem_id, int &tl_ms, int &ml_mb)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT time_limit_ms, memory_limit_mb FROM problems WHERE problem_id = " +
                 to_string(problem_id) + " AND is_deleted = 0 AND is_published = 1";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        cerr << "query failed: " << mysql_error(conn) << endl;
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == NULL)
    {
        mysql_free_result(res);
        return false;
    }

    tl_ms = stoi(row[0]);
    ml_mb = stoi(row[1]);

    mysql_free_result(res);
    return true;
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
            page << page_head("回显结果")
                 << "<header class=\"page-head\"><h1>回显结果</h1>"
                    "<p class=\"lead\">服务器把你填的字原样拼了回来 —— 并且已经转义。</p></header>\n"
                 << "<div class=\"form\">\n"
                 << "<p>名字：" << html_escape(form["name"]) << "</p>\n"
                 << "<p>留言：" << html_escape(form["msg"]) << "</p>\n"
                 << "</div>\n"
                 << "<p class=\"note\"><a href=\"/echo\">再来一次</a></p>\n"
                 << page_tail();
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
    else if (target.compare(0, PROBLEM_PREFIX.size(), PROBLEM_PREFIX) == 0)
    {
        string num = target.substr(PROBLEM_PREFIX.size());
        string html;
        if (!num.empty() && num.find_first_not_of("0123456789") == string::npos)
        {
            try
            {
                if (render_problem(stoi(num), html))
                {
                    send_response(conn_fd, 200, html, "text/html; charset=utf-8");
                    return;
                }
            }
            catch (const exception &)
            {
            }
        }
        send_response(conn_fd, 404, "请输入正确的网址\n");
    }
    else if (method == "POST" && target.compare(0, SUBMIT_PREFIX.size(), SUBMIT_PREFIX) == 0)
    {
        string num = target.substr(SUBMIT_PREFIX.size());
        int problem_id = 0;
        bool ok_id = !num.empty() && num.find_first_not_of("0123456789") == string::npos;

        if (ok_id)
        {
            try
            {
                problem_id = stoi(num);
            }
            catch (const exception &)
            {
                ok_id = false;
            }
        }

        int tl_ms = 0, ml_mb = 0;
        if (!ok_id || !get_problem_limits(problem_id, tl_ms, ml_mb))
        {
            send_response(conn_fd, 404, "没有这道题\n");
            return;
        }

        string code;
        if (form.count("code"))
        {
            code = form["code"];
        }

        if (code.find_first_not_of(" \t\r\n") == string::npos)
        {
            send_response(conn_fd, 400, "代码不能为空\n");
            return;
        }

        string job_id = make_job_id();
        string msg = "收到:" + to_string(code.size()) + " 字节，题号 " +
                     to_string(problem_id) + ",job_id=" + job_id + "\n";
        send_response(conn_fd, 200, msg);
    }
    else if (target == "/problems")
    {
        send_response(conn_fd, 200, render_problems(), "text/html; charset=utf-8");
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

void worker()
{
    get_conn();
    Task t;
    while (true)
    {
        {
            unique_lock<mutex> lock(mtx);
            cv.wait(lock, []
                    { return !task_queue.empty(); });
            t = task_queue.front();
            task_queue.pop();
        }
        handle_request(t.conn_fd, t.req);
        close(t.conn_fd);
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

    thread t1(worker), t2(worker), t3(worker), t4(worker);

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

                    size_t need = request_total_len(conn_bufs[fd]);
                    if (need != string::npos && conn_bufs[fd].size() >= need)
                    {
                        string req = conn_bufs[fd];
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        conn_bufs.erase(fd);
                        {
                            lock_guard<mutex> lock(mtx);
                            task_queue.push({fd, req});
                        }
                        cv.notify_one();
                    }
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
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        perror("read");
                    }
                }
            }
        }
    }
    return 0;
}
