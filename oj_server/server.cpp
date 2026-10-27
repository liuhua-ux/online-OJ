#include "auth.h"
#include "db.h"
#include "log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cctype>
#include <cstdint>
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
#include <atomic>
#include <thread>
#include <condition_variable>
#include <queue>
#include <mysql/mysql.h>
#include <sys/wait.h>
#include <hiredis/hiredis.h>
#include <sys/time.h>
#include <vector>
#include <random>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <arpa/inet.h>

using namespace std;

struct Task
{
    int conn_fd;
    string req;
};

struct JudgeTask
{
    string job_id;
    string work_dir;
    int problem_id;
    int tl_ms;
    int ml_mb;
    string username;
};

queue<Task> task_queue;
mutex mtx;
condition_variable cv;

queue<JudgeTask> judge_queue;
mutex judge_mtx;
condition_variable judge_cv;

const char *JUDGE_BIN = "/home/ubuntu/online-OJ/judge_demo/judge";
const string TESTDATA_DIR = "/home/ubuntu/online-OJ/judge_demo/testdata";
const string PROBLEM_PREFIX = "/problem/";
const string SUBMIT_PREFIX = "/submit/";
const string RESULT_PREFIX = "/result/";

// 排行榜缓存。榜单是派生数据，过期重算即可（需求 OJ-RNK-01）
// ⚠️ 一期 Flask 也在用 Redis，它的键是 "oj:rank"、存的是 JSON。
// 同一台 Redis 是共享命名空间：键一样但格式不同 → 双方解析都失败、互相覆盖，缓存白做。
// 二期用独立的键，两期可以并存对照。
const string RANK_CACHE_KEY = "oj:rank:v3"; // v3：改成长度前缀格式（FIX-23）
const int RANK_CACHE_TTL = 60;       // 秒
const int REDIS_TIMEOUT_US = 200000; // 200ms：Redis 挂了也不能让页面一起卡死

// 请求体 / 源码上限（FIX-25）
const size_t MAX_BODY_BYTES = 1024 * 1024;        // 语义上限：超过回 413
const size_t MAX_CODE_BYTES = 256 * 1024;         // 源码上限
const size_t MAX_REQUEST_BYTES = 2 * 1024 * 1024; // 单连接接收缓冲硬上限（防内存）

// 判题队列上限（FIX-12，需求 OJ-JDG-08「队列有界」）：
// 超出容量时拒绝新提交并明确提示，而不是无限堆积把内存吃光。
const size_t JUDGE_QUEUE_MAX = 64;

// 判题工作线程数。需求 OJ-JDG-08 要求「可配置」，所以读环境变量
// OJ_JUDGE_THREADS（有效范围 1..JUDGE_THREADS_MAX），缺省 2。
// 缺省给 2 是取舍：每个判题进程要 fork 出 g++ 和跑测试点的子进程，
// 开多了会把机器压住（NOTES §29d）。
const int JUDGE_THREADS_DEFAULT = 2;
const int JUDGE_THREADS_MAX = 32;

const string ADMIN_PREFIX = "/admin/";

const map<string, string>
    content_types = {
        {"html", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "application/javascript; charset=utf-8"},
        {"png", "image/png"},
        {"", "application/octet-stream"},
};

// 渲染页面时要显示"谁在登录"。每个请求在同一个线程里跑完，所以 thread_local 够用。
thread_local string nav_user;

// 请求上下文（每个工作线程一份）：handle_request 填，send_response 出口记访问日志
thread_local string req_method;
thread_local string req_path;
thread_local string req_user;
thread_local chrono::steady_clock::time_point req_start;

void send_response(int conn_fd, int code, const string &body, const string &content_type = "text/plain; charset=utf-8", const string &extra_headers = "")
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
    else if (code == 413)
    {
        reason = "Payload Too Large";
    }
    else if (code == 403)
    {
        reason = "Forbidden";
    }
    else if (code == 500)
    {
        reason = "Internal Server Error";
    }
    else if (code == 503)
    {
        reason = "Service Unavailable";
    }
    else if (code == 302)
    {
        reason = "Found";
    }
    else
    {
        reason = "Unknown";
    }

    string resp = "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n"
                                                                 "Content-Type: " +
                  content_type + "\r\n"
                                 "Content-Length: " +
                  to_string(body.size()) + "\r\n" +
                  extra_headers +
                  "Connection: close\r\n"
                  "\r\n" +
                  body;

    write(conn_fd, resp.c_str(), resp.size());

    char ip[INET_ADDRSTRLEN] = "?";
    sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(conn_fd, (sockaddr *)&peer, &peer_len) == 0)
    {
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    }
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - req_start)
                       .count();
    log_write(LOG_INFO, "request",
              "method=" + (req_method.empty() ? string("?") : req_method) +
                  " path=" + log_sanitize(req_path) +
                  " status=" + to_string(code) +
                  " ms=" + to_string(elapsed) +
                  " user=" + log_sanitize(req_user) +
                  " ip=" + ip);
}

void send_redirect(int conn_fd, const string &location, const string &extra_headers = "")
{
    send_response(conn_fd, 302, "", "text/plain; charset=utf-8",
                  "Location: " + location + "\r\n" + extra_headers);
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

// 和 parse_form 一样，但**同名参数全都留着** —— 测试点是 N 个 tc_in / N 个 tc_ans，
// 普通 map 只会留下最后一个
map<string, vector<string>> parse_form_multi(const string &body)
{
    map<string, vector<string>> out;
    size_t start = 0;
    while (start <= body.size())
    {
        size_t amp = body.find('&', start);
        string pair_s = body.substr(start, (amp == string::npos) ? string::npos : amp - start);
        if (!pair_s.empty())
        {
            size_t eq = pair_s.find('=');
            string k = urldecode(pair_s.substr(0, eq));
            string v = (eq == string::npos) ? "" : urldecode(pair_s.substr(eq + 1));
            out[k].push_back(v);
        }
        if (amp == string::npos)
        {
            break;
        }
        start = amp + 1;
    }
    return out;
}

// 浏览器交上来的多行文本换行是 \r\n，落盘前统一成 \n。
// 不归一化的话，判题器逐行比对时多一个看不见的 \r，判谁都是 WA
string normalize_newlines(const string &text)
{
    string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '\r')
        {
            if (i + 1 < text.size() && text[i + 1] == '\n')
            {
                continue; // \r\n 的 \r 丢掉，\n 下一轮自然留下
            }
            out += '\n'; // 单独的 \r 也当换行
        }
        else
        {
            out += text[i];
        }
    }
    return out;
}

string read_text_file(const string &path)
{
    ifstream f(path, ios::binary);
    if (!f)
    {
        return "";
    }
    stringstream ss;
    ss << f.rdbuf();
    return ss.str();
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

bool user_is_admin(const string &username);

string nav_right()
{
    if (nav_user.empty())
    {
        return "    <a href=\"/login\">登录</a>\n"
               "    <a href=\"/register\">注册</a>\n";
    }
    // 管理员才显示「管理」入口。每次现查库 —— 权限不放进 cookie（签出去的票收不回来）
    string admin_link;
    if (user_is_admin(nav_user))
    {
        admin_link = "    <a href=\"/admin/problems\">管理</a>\n";
    }
    return "    <a href=\"/submissions\">我的提交</a>\n"
           "    <a href=\"/rank\">排行榜</a>\n" +
           admin_link +
           "    <a href=\"/profile\">" +
           html_escape(nav_user) + "</a>\n"
                                   "    <a href=\"/logout\">退出</a>\n";
}

string page_head(const string &title, const string &extra_head = "")
{
    return "<!DOCTYPE html>\n"
           "<html lang=\"zh-CN\">\n"
           "<head>\n"
           "<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n" +
           extra_head +
           "<title>" +
           html_escape(title) + " · 自研 OJ</title>\n"
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
                                "    <a href=\"/echo\">回显测试</a>\n" +
           nav_right() +
           "  </nav>\n"
           "</header>\n"
           "<main class=\"wrap\">\n";
}

string page_tail()
{
    return "</main>\n"
           "<footer class=\"foot\">二期 C++ 自研服务端:socket、epoll、线程池</footer>\n"
           "</body>\n"
           "</html>\n";
}

string render_submit_form(int problem_id, const string &last_source)
{
    return "<h2>提交代码</h2>\n"
           "<form class=\"editor\" method=\"POST\" action=\"/submit/" +
           to_string(problem_id) + "\">\n"
                                   "  <div class=\"editor-bar\"><span class=\"dot\" aria-hidden=\"true\"></span><span>submission.cpp</span><span class=\"lang\">C++17</span></div>\n"
                                   "  <textarea name=\"code\" spellcheck=\"false\" placeholder=\"在这里粘贴你的 C++ 代码\">" +
           html_escape(last_source) +
           "</textarea>\n"
           "  <div class=\"editor-foot\">\n"
           "    <button type=\"submit\">提交判题</button>\n"
           "    <span class=\"hint\">提交后在后台编译、运行、比对</span>\n"
           "  </div>\n"
           "</form>\n"
           "<script src=\"/vendor/codemirror/codemirror.min.js\"></script>\n"
           "<script src=\"/vendor/codemirror/mode/clike.min.js\"></script>\n"
           "<script src=\"/vendor/codemirror/addon/closebrackets.min.js\"></script>\n"
           "<script src=\"/vendor/codemirror/addon/matchbrackets.min.js\"></script>\n"
           "<script>\n"
           "(function () {\n"
           "  var ta = document.querySelector('textarea[name=code]');\n"
           "  if (!ta || typeof CodeMirror === 'undefined') { return; }   // 脚本没加载出来就退回裸 textarea\n"
           "  var dark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;\n"
           "  CodeMirror.fromTextArea(ta, {\n"
           "    mode: 'text/x-c++src',\n"
           "    lineNumbers: true,\n"
           "    indentUnit: 4,\n"
           "    tabSize: 4,\n"
           "    indentWithTabs: false,\n"
           "    matchBrackets: true,\n"
           "    autoCloseBrackets: true,\n"
           "    lineWrapping: true,\n"
           "    theme: dark ? 'material-darker' : 'default',\n"
           "    extraKeys: { Tab: function (cm) { cm.replaceSelection('    ', 'end'); } }\n"
           "  });\n"
           "})();\n"
           "</script>\n";
}

// 解析 Content-Length。返回 -1：没有这个头（或请求头还没收全）；
// 0：解析成功，值写入 out；1：有这个头但值不合法
int content_length_of(const string &req, size_t &out)
{
    size_t head_end = req.find("\r\n\r\n");
    if (head_end == string::npos)
    {
        return -1;
    }

    size_t pos = req.find("\r\n");
    if (pos == string::npos)
    {
        return -1;
    }
    pos += 2; // 跳过请求行

    bool found = false;
    size_t value = 0;

    while (pos < head_end)
    {
        size_t line_end = req.find("\r\n", pos);
        if (line_end == string::npos || line_end > head_end)
        {
            line_end = head_end;
        }

        string line = req.substr(pos, line_end - pos);
        pos = line_end + 2;

        size_t colon = line.find(':');
        if (colon == string::npos)
        {
            continue;
        }

        string name = line.substr(0, colon);
        size_t nb = name.find_first_not_of(" \t");
        size_t ne = name.find_last_not_of(" \t");
        if (nb == string::npos)
        {
            continue;
        }
        name = name.substr(nb, ne - nb + 1);

        for (char &ch : name)
        {
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        }
        if (name != "content-length")
        {
            continue;
        }

        if (found)
        {
            return 1; // 同一个头出现两次：直接拒绝
        }

        string raw = line.substr(colon + 1);
        size_t vb = raw.find_first_not_of(" \t");
        size_t ve = raw.find_last_not_of(" \t");
        if (vb == string::npos)
        {
            return 1;
        }
        raw = raw.substr(vb, ve - vb + 1);

        if (raw.find_first_not_of("0123456789") != string::npos)
        {
            return 1; // 负号、字母、小数点都不认
        }

        for (char ch : raw)
        {
            size_t digit = static_cast<size_t>(ch - '0');
            if (value > (SIZE_MAX - digit) / 10)
            {
                return 1; // 溢出
            }
            value = value * 10 + digit;
        }

        found = true;
    }

    if (!found)
    {
        return -1;
    }

    out = value;
    return 0;
}

size_t request_total_len(const string &req)
{
    size_t head_end = req.find("\r\n\r\n");
    if (head_end == string::npos)
        return string::npos;

    size_t total = head_end + 4;

    // 值不合法时也按「没有 body」处理：请求头一到就派发，
    // 由 handle_request 统一回 400，而不是让连接一直等下去
    size_t body_len = 0;
    if (content_length_of(req, body_len) == 0)
    {
        total += body_len;
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
        log_write(LOG_ERROR, "judge_spawn", "stage=pipe err=" + string(strerror(errno)));
        return false;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        log_write(LOG_ERROR, "judge_spawn", "stage=fork err=" + string(strerror(errno)));
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
        log_write(LOG_ERROR, "judge_spawn", "stage=waitpid err=" + string(strerror(errno)));
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) != CHILD_NOT_STARTED;
}

struct JudgeSummary
{
    string verdict;
    int passed = 0;
    int time_ms = 0;
    int memory_kb = 0;
    bool has_time = false;
    bool has_memory = false;
};

JudgeSummary parse_judge(const string &out)
{
    JudgeSummary s;
    istringstream iss(out);
    string line;

    while (getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line.compare(0, 9, "VERDICT: ") == 0)
        {
            s.verdict = line.substr(9);
        }
        else if (line == "AC")
        {
            s.passed++;
        }
        else if (line.compare(0, 6, "time: ") == 0 &&
                 line.size() > 9 &&
                 line.compare(line.size() - 3, 3, " ms") == 0)
        {
            size_t start = 6;
            size_t end = line.size() - 3;
            int v = stoi(line.substr(start, end - start));
            if (v > s.time_ms)
            {
                s.time_ms = v;
            }
            s.has_time = true;
        }
        else if (line.compare(0, 13, "peak memory: ") == 0 &&
                 line.size() > 16 &&
                 line.compare(line.size() - 3, 3, " KB") == 0)
        {
            size_t start = 13;
            size_t end = line.size() - 3;
            int v = stoi(line.substr(start, end - start));
            if (v > s.memory_kb)
            {
                s.memory_kb = v;
            }
            s.has_memory = true;
        }
    }
    return s;
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
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
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

bool render_problem(int problem_id, const string &viewer, string &html)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    // 这个人上次交这道题的源码（没登录就查不了，也不该查）
    string last_source;
    if (!viewer.empty())
    {
        string q = "SELECT source FROM submissions WHERE username = '" +
                   escape_sql(viewer) + "' AND problem_id = " +
                   to_string(problem_id) +
                   " ORDER BY created_at DESC, job_id DESC LIMIT 1";
        if (mysql_query(conn, q.c_str()) == 0)
        {
            MYSQL_RES *sr = mysql_store_result(conn);
            MYSQL_ROW srow = (sr != nullptr) ? mysql_fetch_row(sr) : nullptr;
            if (srow != nullptr && srow[0] != nullptr)
            {
                last_source = srow[0];
            }
            if (sr != nullptr)
            {
                mysql_free_result(sr);
            }
        }
    }

    string sql = "SELECT title, description, input_format, output_format, "
                 "sample_input, sample_output, time_limit_ms, memory_limit_mb "
                 "FROM problems WHERE problem_id = " +
                 to_string(problem_id) +
                 " AND is_deleted = 0 AND is_published = 1";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
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
    page << page_head(row[0],
                      "<link rel=\"stylesheet\" href=\"/vendor/codemirror/codemirror.min.css\">\n"
                      "<link rel=\"stylesheet\" href=\"/vendor/codemirror/theme/material-darker.min.css\">\n");
    page << "<p class=\"breadcrumb\"><a href=\"/problems\">题库</a> / 第 " << problem_id << " 题</p>\n";

    page << "<header class=\"problem-header\">\n";
    page << "<h1>" << html_escape(row[0]) << "</h1>\n";
    page << "<div class=\"chips\"><span class=\"chip\">时间 " << row[6] << " ms</span>"
                                                                           "<span class=\"chip\">内存 "
         << row[7] << " MB</span></div>\n";
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

    page << render_submit_form(problem_id, last_source);
    page << "</div>\n";
    page << page_tail();

    html = page.str();

    mysql_free_result(res);
    return true;
}

// 数据库里的 NULL 在页面上统一变成空串 / 破折号
string cell(const char *p)
{
    return p ? p : "";
}

string num_or_dash(const char *p)
{
    return p ? string(p) : "—";
}

// job_id 是我们自己生成的 8 位小写十六进制；格式不对直接 404，不进数据库
bool valid_job_id(const string &id)
{
    return id.size() == 8 &&
           id.find_first_not_of("0123456789abcdef") == string::npos;
}

// 这条提交属于谁；找不到这条记录返回 false
bool get_submission_owner(const string &job_id, string &owner)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
        return false;

    string sql = "SELECT username FROM submissions WHERE job_id = '" +
                 escape_sql(job_id) + "'";
    if (mysql_query(conn, sql.c_str()) != 0)
        return false;

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool found = (row != nullptr);
    if (found)
    {
        owner = (row[0] != nullptr) ? row[0] : ""; // 老数据是 NULL
    }

    mysql_free_result(res);
    return found;
}

bool user_is_admin(const string &username)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
        return false;

    string sql = "SELECT is_admin FROM users WHERE username = '" +
                 escape_sql(username) + "'";
    if (mysql_query(conn, sql.c_str()) != 0)
        return false;

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool admin = (row != nullptr && row[0] != nullptr && string(row[0]) == "1");

    mysql_free_result(res);
    return admin;
}

// 管理页统一守卫。true = 放行；false = 已经替你把响应发了（302 或 403）
bool admin_guard(int conn_fd, const string &current_user)
{
    if (current_user.empty())
    {
        send_redirect(conn_fd, "/login");
        return false;
    }

    if (!user_is_admin(current_user))
    {
        stringstream page;
        page << page_head("没有权限")
             << "<p class=\"empty\">需要管理员权限。</p>\n"
             << page_tail();
        send_response(conn_fd, 403, page.str(), "text/html; charset=utf-8");
        return false;
    }

    return true;
}

string render_submissions(const string &viewer, const string &filter_user)
{
    bool admin = user_is_admin(viewer);

    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return page_head("我的提交") +
               "<p class=\"empty\">数据库暂时不可用。</p>\n" + page_tail();
    }

    // 学生：只查本人，?user= 传什么都没用
    // 管理员：不填就查全部，填了就按用户名筛
    string where;
    if (!admin)
    {
        where = " WHERE s.username = '" + escape_sql(viewer) + "'";
    }
    else if (!filter_user.empty())
    {
        where = " WHERE s.username = '" + escape_sql(filter_user) + "'";
    }

    // 排序用完整精度，显示只到秒 —— 微秒只是用来定序的，人不用看
    string sql = "SELECT s.job_id, s.username, s.problem_id, p.title, s.status, "
                 "DATE_FORMAT(s.created_at, '%Y-%m-%d %H:%i:%s') "
                 "FROM submissions s LEFT JOIN problems p "
                 "ON s.problem_id = p.problem_id" +
                 where +
                 " ORDER BY s.created_at DESC, s.job_id DESC LIMIT 100";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return page_head("我的提交") +
               "<p class=\"empty\">提交记录暂时读不出来。</p>\n" + page_tail();
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return page_head("我的提交") +
               "<p class=\"empty\">提交记录暂时读不出来。</p>\n" + page_tail();
    }

    string title;
    if (!admin)
        title = "我的提交";
    else if (filter_user.empty())
        title = "全部提交记录";
    else
        title = filter_user + " 的提交";

    stringstream page;
    page << page_head("提交记录");
    page << "<header class=\"page-head\"><h1>" << html_escape(title)
         << "</h1></header>\n";

    // 管理员的筛选框：GET 只读，不写数据
    if (admin)
    {
        page << "<form class=\"filter\" method=\"GET\" action=\"/submissions\">\n"
             << "<input name=\"user\" value=\"" << html_escape(filter_user)
             << "\" placeholder=\"按用户名筛选\">\n"
             << "<button type=\"submit\">筛选</button>\n"
             << "<a class=\"note\" href=\"/submissions\">看全部</a>\n"
             << "</form>\n";
    }

    int count = 0;
    page << "<table class=\"sub-table\">\n"
         << "<tr><th>提交号</th><th>题目</th><th>提交者</th>"
            "<th>状态</th><th>提交时间</th></tr>\n";

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        string job_id = (row[0] != nullptr) ? row[0] : "";
        string who = (row[1] != nullptr) ? row[1] : "游客";

        string prob;
        if (row[2] != nullptr)
        {
            prob = "第 " + string(row[2]) + " 题 " +
                   ((row[3] != nullptr) ? row[3] : "");
        }
        else
        {
            prob = "（老数据·无题号）";
        }

        string status = (row[4] != nullptr) ? row[4] : "";
        string created = (row[5] != nullptr) ? row[5] : "";

        page << "<tr>"
             << "<td><a href=\"/result/" << html_escape(job_id) << "\">"
             << html_escape(job_id) << "</a></td>"
             << "<td>" << html_escape(prob) << "</td>"
             << "<td>" << html_escape(who) << "</td>"
             << "<td>" << html_escape(status) << "</td>"
             << "<td>" << html_escape(created) << "</td>"
             << "</tr>\n";
        count++;
    }
    mysql_free_result(res);

    page << "</table>\n";
    if (count == 0)
    {
        page << "<p class=\"empty\">还没有提交记录。</p>\n";
    }
    page << page_tail();
    return page.str();
}

// 连 Redis：**带超时**。没超时的话 Redis 一挂，请求就挂在那里，"降级"无从谈起
redisContext *redis_connect()
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = REDIS_TIMEOUT_US;

    redisContext *c = redisConnectWithTimeout("127.0.0.1", 6379, tv);
    if (c == nullptr || c->err)
    {
        if (c != nullptr)
        {
            log_write(LOG_ERROR, "redis",
                      string("fn=") + __func__ + " err=" + string(c->errstr));
            redisFree(c);
        }
        return nullptr;
    }

    redisSetTimeout(c, tv);
    return c;
}

// 真来源：MySQL。成功返回 true 并填 rows
bool rank_query_db(vector<pair<string, int>> &rows)
{
    rows.clear();

    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    // 需求 OJ-RNK-01：同一用户同一题多次通过只算一次 → COUNT(DISTINCT problem_id)
    // 老数据（username 或 problem_id 为 NULL）没法归属到「谁通过了哪题」，不计入
    string sql = "SELECT username, COUNT(DISTINCT problem_id) AS solved "
                 "FROM submissions "
                 "WHERE verdict = 'AC' "
                 "AND username IS NOT NULL AND problem_id IS NOT NULL "
                 "GROUP BY username "
                 "ORDER BY solved DESC, username ASC "
                 "LIMIT 100";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return false;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        rows.push_back({row[0] ? row[0] : "", row[1] ? atoi(row[1]) : 0});
    }

    mysql_free_result(res);
    return true;
}

// 缓存格式（v3）：<用户名字节数>:<用户名>:<题数>\n
// 为什么不用 TAB 分隔：注册只限制了用户名长度、没限制字符，
// 用户名里出现 TAB 或换行就会把整份缓存撑错位（FIX-23）。
// 长度前缀不依赖「猜哪个字符是分隔符」—— 长度是事实，不是约定。
string rank_cache_encode(const vector<pair<string, int>> &rows)
{
    string text;
    for (const auto &row : rows)
    {
        text += to_string(row.first.size()) + ":" + row.first + ":" +
                to_string(row.second) + "\n";
    }
    return text;
}

bool rank_cache_decode(const string &text, vector<pair<string, int>> &rows)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        // 1) 用户名字节数
        size_t colon = text.find(':', pos);
        if (colon == string::npos || colon == pos)
        {
            return false;
        }

        int name_len = 0;
        try
        {
            name_len = stoi(text.substr(pos, colon - pos));
        }
        catch (const exception &)
        {
            return false;
        }
        if (name_len < 0)
        {
            return false;
        }

        // 2) 精确取 name_len 个字节 —— 用户名里有冒号、换行都无所谓
        size_t name_begin = colon + 1;
        size_t name_end = name_begin + static_cast<size_t>(name_len);
        if (name_end >= text.size() || text[name_end] != ':')
        {
            return false;
        }
        string name = text.substr(name_begin, name_len);

        // 3) 题数，到行尾为止
        size_t nl = text.find('\n', name_end + 1);
        if (nl == string::npos)
        {
            return false;
        }

        int count = 0;
        try
        {
            count = stoi(text.substr(name_end + 1, nl - name_end - 1));
        }
        catch (const exception &)
        {
            return false;
        }

        rows.push_back({name, count});
        pos = nl + 1;
    }
    return true;
}

// 读缓存：true = 命中。Redis 任何问题都当"没有缓存"，绝不抛给页面
bool rank_cache_get(vector<pair<string, int>> &rows)
{
    redisContext *c = redis_connect();
    if (c == nullptr)
    {
        return false;
    }

    redisReply *r = (redisReply *)redisCommand(c, "GET %s", RANK_CACHE_KEY.c_str());
    bool hit = false;

    // 关键：只有 STRING 才算命中。NIL(4) = 没有缓存；
    // 空串也是 STRING —— "缓存了一个空榜"和"没缓存"必须分得开
    if (r != nullptr && r->type == REDIS_REPLY_STRING)
    {
        vector<pair<string, int>> parsed;
        if (rank_cache_decode(string(r->str, r->len), parsed))
        {
            rows = parsed;
            hit = true;
        }
    }

    if (r != nullptr)
    {
        freeReplyObject(r);
    }
    redisFree(c);
    return hit;
}

// 写缓存：失败无所谓，页面已经能出数据了
void rank_cache_set(const vector<pair<string, int>> &rows)
{
    string text = rank_cache_encode(rows);

    redisContext *c = redis_connect();
    if (c == nullptr)
    {
        return;
    }

    redisReply *r = (redisReply *)redisCommand(c, "SETEX %s %d %b",
                                               RANK_CACHE_KEY.c_str(),
                                               RANK_CACHE_TTL,
                                               text.data(), text.size());
    if (r != nullptr)
    {
        freeReplyObject(r);
    }
    redisFree(c);
}

// 有新 AC 就作废缓存，下次访问重算。WA/TLE 不影响榜单，不删
void rank_cache_invalidate()
{
    redisContext *c = redis_connect();
    if (c == nullptr)
    {
        return;
    }

    redisReply *r = (redisReply *)redisCommand(c, "DEL %s", RANK_CACHE_KEY.c_str());
    if (r != nullptr)
    {
        freeReplyObject(r);
    }
    redisFree(c);
}

string render_rank()
{
    vector<pair<string, int>> rows;
    bool from_cache = rank_cache_get(rows);

    if (!from_cache)
    {
        if (!rank_query_db(rows))
        {
            return page_head("排行榜") +
                   "<p class=\"empty\">排行榜暂时读不出来。</p>\n" + page_tail();
        }
        rank_cache_set(rows);
    }

    stringstream page;
    page << page_head("排行榜");
    page << "<header class=\"page-head\"><h1>排行榜</h1>"
            "<p class=\"lead\">按通过题数排名，同一道题通过多次只算一次。</p></header>\n";

    // 标出来源 —— 演示「Redis 挂了会降级查库」时一眼能看见
    page << "<p class=\"note\">数据来源："
         << (from_cache ? "Redis 缓存" : "直接查数据库") << "</p>\n";

    if (rows.empty())
    {
        page << "<p class=\"empty\">还没有人通过任何题目。</p>\n";
    }
    else
    {
        page << "<table class=\"sub-table\">\n"
             << "<tr><th>名次</th><th>用户</th><th>通过题数</th></tr>\n";
        for (size_t i = 0; i < rows.size(); i++)
        {
            page << "<tr><td>" << (i + 1) << "</td>"
                 << "<td>" << html_escape(rows[i].first) << "</td>"
                 << "<td>" << rows[i].second << "</td></tr>\n";
        }
        page << "</table>\n";
    }

    page << page_tail();
    return page.str();
}

bool render_result(const string &job_id, string &html)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT status, verdict, passed, total, time_ms, "
                 "memory_kb, result, problem_id, "
                 "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), source "
                 "FROM submissions WHERE job_id = '" +
                 job_id + "'";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
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

    string status = cell(row[0]);
    string verdict = cell(row[1]);
    string passed = num_or_dash(row[2]);
    string total = num_or_dash(row[3]);
    string time_ms = num_or_dash(row[4]);
    string memory = num_or_dash(row[5]);
    string raw = cell(row[6]);
    string prob_id = cell(row[7]);
    string created = cell(row[8]);
    string source = cell(row[9]);

    bool finished = (status == "完成");

    stringstream page;
    page << page_head("提交结果",
                      finished ? ""
                               : "<meta http-equiv=\"refresh\" content=\"2\">\n");

    page << "<p class=\"breadcrumb\"><a href=\"/problems\">题库</a> / "
         << "<a href=\"/problem/" << prob_id << "\">第 " << prob_id << " 题</a>"
         << " / 提交 " << job_id << "</p>\n";

    page << "<header class=\"problem-header\">\n";
    page << "<h1>判题结果</h1>\n";
    page << "<div class=\"chips\"><span class=\"chip\">提交号 " << job_id
         << "</span><span class=\"chip\">" << html_escape(created) << "</span></div>\n";
    page << "</header>\n";

    if (!finished)
    {
        page << "<div class=\"pending\"><span class=\"spinner\"></span>"
             << html_escape(status) << "，页面每 2 秒自动刷新…</div>\n";
    }
    else
    {
        string cls = "verdict-other";
        if (verdict == "AC")
        {
            cls = "verdict-ac";
        }
        else if (verdict == "WA")
        {
            cls = "verdict-wa";
        }
        else if (verdict == "CE" || verdict == "TLE" || verdict == "RE" ||
                 verdict == "MLE" || verdict == "NO_DATA" || verdict == "SE")
        {
            cls = "verdict-err";
        }

        page << "<h2>提交的源码</h2>\n";
        if (source.empty())
        {
            page << "<p class=\"empty\">（老数据·无源码）</p>\n";
        }
        else
        {
            page << "<pre class=\"result-output\">" << html_escape(source) << "</pre>\n";
        }

        page << "<section class=\"result-card\">\n";
        page << "<div class=\"verdict-badge " << cls << "\">"
             << html_escape(verdict.empty() ? "—" : verdict) << "</div>\n";
        page << "<div class=\"result-stats\">\n";
        page << "<div><b>" << passed << " / " << total << "</b><span>测试点</span></div>\n";
        page << "<div><b>" << time_ms << "</b><span>最慢耗时 (ms)</span></div>\n";
        page << "<div><b>" << memory << "</b><span>峰值内存 (KB)</span></div>\n";
        page << "</div>\n";
        page << "</section>\n";

        page << "<h2>判题器输出</h2>\n";
        page << "<pre class=\"result-output\">" << html_escape(raw) << "</pre>\n";
    }

    page << "<p class=\"note\"><a href=\"/problem/" << prob_id
         << "\">← 回题目</a>　<a href=\"/problems\">题库</a></p>\n";
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
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
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

void handle_request(int conn_fd, const string &req);

void worker()
{
    get_conn();
    Task t;
    while (true)
    {
        auto wait_start = chrono::steady_clock::now();
        size_t qlen = 0;
        {
            unique_lock<mutex> lock(mtx);
            cv.wait(lock, []
                    { return !task_queue.empty(); });
            t = task_queue.front();
            task_queue.pop();
            qlen = task_queue.size();
        }
        auto wait_ms = chrono::duration_cast<chrono::milliseconds>(
                           chrono::steady_clock::now() - wait_start)
                           .count();
        log_write(LOG_DEBUG, "queue",
                  "len=" + to_string(qlen) + " wait_ms=" + to_string(wait_ms));
        handle_request(t.conn_fd, t.req);
        close(t.conn_fd);
    }
}

bool set_status(const string &job_id, const string &status)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "UPDATE submissions SET status = '" + status +
                 "' WHERE job_id = '" + job_id + "'";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }
    return true;
}

bool finish_job(const string &job_id, const string &result_text,
                const JudgeSummary &s, int total, bool started)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string verdict = started ? s.verdict : "SE";
    string result = started ? result_text : "判题器没有跑起来\n" + result_text;

    string esc_result = escape_sql(result);
    string esc_verdict = escape_sql(verdict);

    string time_sql = s.has_time ? to_string(s.time_ms) : "NULL";
    string mem_sql = s.has_memory ? to_string(s.memory_kb) : "NULL";

    string sql = "UPDATE submissions SET "
                 "status = '完成', "
                 "result = '" +
                 esc_result + "', "
                              "verdict = '" +
                 esc_verdict + "', "
                               "passed = " +
                 to_string(s.passed) + ", "
                                       "total = " +
                 to_string(total) + ", "
                                    "time_ms = " +
                 time_sql + ", "
                            "memory_kb = " +
                 mem_sql +
                 " WHERE job_id = '" + job_id + "'";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }

    // 榜单是派生数据：只有 AC 会改变「谁通过了哪题」，其它结论不碰缓存
    if (verdict == "AC")
    {
        rank_cache_invalidate();
    }
    return true;
}

int count_testcases(int problem_id)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return 0;
    }

    string sql = "SELECT COUNT(*) FROM testcases WHERE problem_id = " +
                 to_string(problem_id);

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return 0;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL)
    {
        return 0;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    int n = (row != NULL) ? stoi(row[0]) : 0;

    mysql_free_result(res);
    return n;
}

// field_value 定义在后面（渲染表单那边），这里先用前向声明
string field_value(const map<string, string> &v, const string &k);

struct ProblemRow
{
    string title, description, input_format, output_format, sample_input, sample_output;
    int time_limit_ms = 0;
    int memory_limit_mb = 0;
    bool is_published = false;
    bool is_deleted = false;
    int version = 1; // 乐观锁：编辑页读到几，保存时必须还是几（FIX-21）
};

// 管理页专用：软删除的也查得到（学生侧那个查询会把已删除当不存在）
bool get_problem_for_admin(int problem_id, ProblemRow &p)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT title, description, input_format, output_format, "
                 "sample_input, sample_output, time_limit_ms, memory_limit_mb, "
                 "is_published, is_deleted, version FROM problems WHERE problem_id = " +
                 to_string(problem_id);

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    bool found = (row != nullptr);
    if (found)
    {
        p.title = row[0] ? row[0] : "";
        p.description = row[1] ? row[1] : "";
        p.input_format = row[2] ? row[2] : "";
        p.output_format = row[3] ? row[3] : "";
        p.sample_input = row[4] ? row[4] : "";
        p.sample_output = row[5] ? row[5] : "";
        p.time_limit_ms = row[6] ? stoi(row[6]) : 0;
        p.memory_limit_mb = row[7] ? stoi(row[7]) : 0;
        p.is_published = (row[8] != nullptr && string(row[8]) == "1");
        p.is_deleted = (row[9] != nullptr && string(row[9]) == "1");
        p.version = row[10] ? stoi(row[10]) : 1;
    }

    mysql_free_result(res);
    return found;
}

// 按库里的元信息把测试点从磁盘读回来，填进表单
vector<pair<string, string>> load_testcases(int problem_id)
{
    vector<pair<string, string>> cases;

    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return cases;
    }

    string sql = "SELECT input_path, answer_path FROM testcases "
                 "WHERE problem_id = " +
                 to_string(problem_id) + " ORDER BY ordinal";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return cases;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return cases;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL)
    {
        string in_text = row[0] ? read_text_file(TESTDATA_DIR + "/" + row[0]) : "";
        string ans_text = row[1] ? read_text_file(TESTDATA_DIR + "/" + row[1]) : "";
        cases.push_back({in_text, ans_text});
    }

    mysql_free_result(res);
    return cases;
}

struct TestcaseMeta
{
    string in_path, ans_path;
    int in_size = 0;
    int ans_size = 0;
};

// 按顺序从 1 重新编号写盘。**不能先删目录再写**：
// 中途失败（磁盘满、进程被杀）会让这道题**一个测试点都不剩**，已发布的题变成「交上来一律不通过」（FIX-20）。
// 修法：先写临时目录，写完整了再 rename 整体换过去 —— 同一文件系统内 rename 是原子的，
// 判题器要么看到旧的整套，要么看到新的整套，看不到「写了一半」。
bool save_testcases_to_disk(int problem_id,
                            const vector<pair<string, string>> &cases,
                            vector<TestcaseMeta> &meta)
{
    // 同一个进程里两个管理员可能同时保存 → 临时目录名要带序号
    static atomic<unsigned long> seq{0};

    string dir = TESTDATA_DIR + "/" + to_string(problem_id);
    string tag = to_string(problem_id) + "-" + to_string(getpid()) + "-" +
                 to_string(seq.fetch_add(1));
    string tmp = TESTDATA_DIR + "/.tmp-" + tag;
    string old = TESTDATA_DIR + "/.old-" + tag;
    std::error_code ec;

    std::filesystem::create_directories(tmp, ec);
    if (ec)
    {
        log_write(LOG_ERROR, "fs",
                  "fn=" + string(__func__) + " err=" + ec.message());
        return false;
    }

    meta.clear();
    for (size_t i = 0; i < cases.size(); i++)
    {
        int ordinal = static_cast<int>(i) + 1;
        string in_text = normalize_newlines(cases[i].first);
        string ans_text = normalize_newlines(cases[i].second);

        string in_file = "/" + to_string(ordinal) + ".in";
        string ans_file = "/" + to_string(ordinal) + ".ans";

        ofstream fin(tmp + in_file, ios::binary);
        fin << in_text;
        if (fin.fail())
        {
            log_write(LOG_ERROR, "fs", "fn=" + string(__func__) + " failed=in ordinal=" + to_string(ordinal));
            fin.close();
            std::filesystem::remove_all(tmp, ec);
            return false;
        }

        ofstream fans(tmp + ans_file, ios::binary);
        fans << ans_text;
        if (fans.fail())
        {
            log_write(LOG_ERROR, "fs", "fn=" + string(__func__) + " failed=ans ordinal=" + to_string(ordinal));
            fans.close();
            std::filesystem::remove_all(tmp, ec);
            return false;
        }

        // 库里存相对路径：换机器、换目录，库里的数据不用改
        TestcaseMeta m;
        m.in_path = to_string(problem_id) + in_file;
        m.ans_path = to_string(problem_id) + ans_file;
        m.in_size = static_cast<int>(in_text.size());
        m.ans_size = static_cast<int>(ans_text.size());
        meta.push_back(m);
    }

    // 换过去：旧目录先挪开 → 临时目录改名 → 删旧目录
    bool had_old = std::filesystem::exists(dir);
    if (had_old)
    {
        std::filesystem::rename(dir, old, ec);
        if (ec)
        {
            log_write(LOG_ERROR, "fs",
                      "fn=" + string(__func__) + " step=stash err=" + ec.message());
            std::filesystem::remove_all(tmp, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp, dir, ec);
    if (ec)
    {
        log_write(LOG_ERROR, "fs",
                  "fn=" + string(__func__) + " step=swap err=" + ec.message());
        // 换失败就把旧的放回去，别让这道题凭空消失
        if (had_old)
        {
            std::error_code ec2;
            std::filesystem::rename(old, dir, ec2);
        }
        std::error_code ec3;
        std::filesystem::remove_all(tmp, ec3);
        return false;
    }

    if (had_old)
    {
        // 删不掉旧目录不影响正确性，只是留垃圾 —— 记一条日志就够
        std::error_code ec4;
        std::filesystem::remove_all(old, ec4);
        if (ec4)
        {
            log_write(LOG_WARN, "fs",
                      "fn=" + string(__func__) + " step=cleanup err=" + ec4.message());
        }
    }

    return true;
}

// 元信息整份换掉。DELETE + INSERT 裹在同一个事务里：
// 不裹的话 DELETE 完 INSERT 挂了 = 这道题的测试点元信息整个消失
bool replace_testcases(int problem_id, const vector<TestcaseMeta> &meta)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    if (mysql_query(conn, "START TRANSACTION") != 0)
    {
        return false;
    }

    string del = "DELETE FROM testcases WHERE problem_id = " + to_string(problem_id);
    if (mysql_query(conn, del.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        mysql_query(conn, "ROLLBACK");
        return false;
    }

    for (size_t i = 0; i < meta.size(); i++)
    {
        string sql = "INSERT INTO testcases "
                     "(problem_id, ordinal, input_path, answer_path, input_size, answer_size) "
                     "VALUES (" +
                     to_string(problem_id) + ", " + to_string(i + 1) + ", '" +
                     escape_sql(meta[i].in_path) + "', '" +
                     escape_sql(meta[i].ans_path) + "', " +
                     to_string(meta[i].in_size) + ", " +
                     to_string(meta[i].ans_size) + ")";

        if (mysql_query(conn, sql.c_str()) != 0)
        {
            log_write(LOG_ERROR, "db",
                      "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
            mysql_query(conn, "ROLLBACK");
            return false;
        }
    }

    return mysql_query(conn, "COMMIT") == 0;
}

enum class UpdateResult
{
    Ok,
    Conflict, // 版本号对不上 = 别人先保存了
    Error
};

// 乐观锁：UPDATE 带上 version 条件，成功后版本号 +1。
// 影响 0 行只有两种可能：题被软删了，或者版本号变了（别人先保存）——
// 前者在调用方前面已经拦过，所以这里按「冲突」处理（FIX-21）。
UpdateResult update_problem(int problem_id, const map<string, string> &v,
                            int tl_ms, int ml_mb, bool publish, int expected_version)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return UpdateResult::Error;
    }

    // AND is_deleted = 0：软删除的题不许再改
    string sql = "UPDATE problems SET title = '" + escape_sql(field_value(v, "title")) +
                 "', description = '" + escape_sql(field_value(v, "description")) +
                 "', input_format = '" + escape_sql(field_value(v, "input_format")) +
                 "', output_format = '" + escape_sql(field_value(v, "output_format")) +
                 "', sample_input = '" + escape_sql(field_value(v, "sample_input")) +
                 "', sample_output = '" + escape_sql(field_value(v, "sample_output")) +
                 "', time_limit_ms = " + to_string(tl_ms) +
                 ", memory_limit_mb = " + to_string(ml_mb) +
                 ", is_published = " + to_string(publish ? 1 : 0) +
                 ", version = version + 1" +
                 " WHERE problem_id = " + to_string(problem_id) +
                 " AND is_deleted = 0" +
                 " AND version = " + to_string(expected_version);

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return UpdateResult::Error;
    }

    if (mysql_affected_rows(conn) == 0)
    {
        return UpdateResult::Conflict;
    }

    return UpdateResult::Ok;
}

struct UserRow
{
    int user_id = 0;
    string username;
    bool is_admin = false;
    bool is_disabled = false;
    string created_at;
};

vector<UserRow> list_users()
{
    vector<UserRow> rows;

    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return rows;
    }

    if (mysql_query(conn, "SELECT user_id, username, is_admin, is_disabled, created_at "
                          "FROM users ORDER BY user_id") != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return rows;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return rows;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL)
    {
        UserRow u;
        u.user_id = stoi(row[0]);
        u.username = row[1] ? row[1] : "";
        u.is_admin = (row[2] != nullptr && string(row[2]) == "1");
        u.is_disabled = (row[3] != nullptr && string(row[3]) == "1");
        u.created_at = row[4] ? row[4] : "";
        rows.push_back(u);
    }

    mysql_free_result(res);
    return rows;
}

// 管理页的操作一律走数字主键：用户名可能含空格等字符，塞进 URL 还要再编码
bool get_user_by_id(int user_id, UserRow &u)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT user_id, username, is_admin, is_disabled, created_at "
                 "FROM users WHERE user_id = " +
                 to_string(user_id);
    if (mysql_query(conn, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    bool found = (row != nullptr);
    if (found)
    {
        u.user_id = stoi(row[0]);
        u.username = row[1] ? row[1] : "";
        u.is_admin = (row[2] != nullptr && string(row[2]) == "1");
        u.is_disabled = (row[3] != nullptr && string(row[3]) == "1");
        u.created_at = row[4] ? row[4] : "";
    }

    mysql_free_result(res);
    return found;
}

int count_active_admins()
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return 0;
    }

    if (mysql_query(conn, "SELECT COUNT(*) FROM users "
                          "WHERE is_admin = 1 AND is_disabled = 0") != 0)
    {
        return 0;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return 0;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    int n = (row != NULL && row[0] != NULL) ? stoi(row[0]) : 0;
    mysql_free_result(res);
    return n;
}

bool set_user_password(int user_id, const string &hash)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "UPDATE users SET password_hash = '" + escape_sql(hash) +
                 "' WHERE user_id = " + to_string(user_id);
    return mysql_query(conn, sql.c_str()) == 0;
}

bool set_user_disabled(int user_id, bool disabled)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "UPDATE users SET is_disabled = " + to_string(disabled ? 1 : 0) +
                 " WHERE user_id = " + to_string(user_id);
    if (mysql_query(conn, sql.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }

    // 解禁不需要踢人：禁用期间登不进来，手里不可能有票
    if (!disabled)
    {
        return true;
    }

    // 禁用立刻踢掉这个人的所有会话，旧 Cookie 从下一次请求起失效
    string sql_kick = "DELETE s FROM sessions s JOIN users u "
                      "ON s.username = u.username WHERE u.user_id = " + to_string(user_id);
    if (mysql_query(conn, sql_kick.c_str()) != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return false;
    }

    return true;
}

bool set_user_admin(int user_id, bool admin)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "UPDATE users SET is_admin = " + to_string(admin ? 1 : 0) +
                 " WHERE user_id = " + to_string(user_id);
    return mysql_query(conn, sql.c_str()) == 0;
}

// 从 "/admin/user/7/toggle_admin" 抠出 7 和 "toggle_admin"
bool parse_admin_user_path(const string &path, int &user_id, string &action)
{
    const string prefix = "/admin/user/";
    if (path.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }

    string rest = path.substr(prefix.size());
    size_t slash = rest.find('/');
    if (slash == string::npos)
    {
        return false;
    }

    string num = rest.substr(0, slash);
    action = rest.substr(slash + 1);

    if (num.empty() || num.find_first_not_of("0123456789") != string::npos)
    {
        return false;
    }

    try
    {
        user_id = stoi(num);
    }
    catch (const exception &)
    {
        return false;
    }
    return true;
}

// 从 "/admin/problem/12/edit" 抠出 12 和 "edit"；不是这个形状返回 false
bool parse_admin_problem_path(const string &path, int &problem_id, string &action)
{
    const string prefix = "/admin/problem/";
    if (path.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }

    string rest = path.substr(prefix.size());
    size_t slash = rest.find('/');
    if (slash == string::npos)
    {
        return false;
    }

    string num = rest.substr(0, slash);
    action = rest.substr(slash + 1);

    if (num.empty() || num.find_first_not_of("0123456789") != string::npos)
    {
        return false;
    }

    try
    {
        problem_id = stoi(num);
    }
    catch (const exception &)
    {
        return false;
    }
    return true;
}

// 读表单里的值。**用 find，不用 operator[]** —— 读的时候 [] 会凭空造一个空键（决策 5）
string field_value(const map<string, string> &v, const string &k)
{
    auto it = v.find(k);
    return (it == v.end()) ? "" : it->second;
}

// 「整串都是数字」+ 正数。abc / 1.5 / 空串 / 0 / -5 全都过不了
bool parse_positive_int(const string &s, int &out)
{
    long v = 0;
    if (!to_long(s, v))
    {
        return false;
    }
    if (v <= 0 || v > 2147483647L)
    {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// 保存撞车时给管理员的页面：不说「保存失败」含糊话，直接说发生了什么、下一步点哪
string render_conflict_page(int problem_id)
{
    stringstream page;
    page << page_head("保存冲突");
    page << "<header class=\"page-head\"><h1>这道题刚被别人改过</h1>"
            "<p class=\"lead\">你打开这个页面之后，另一个管理员先保存了一次。"
            "为了不把他的改动悄悄盖掉，这次保存没有生效。</p></header>\n";
    page << "<p><a href=\"/admin/problem/" << problem_id
         << "/edit\">刷新这一页（会看到他的版本，再改一遍）</a></p>\n";
    page << page_tail();
    return page.str();
}

string render_problem_form(const string &heading, const string &action,
                           const map<string, string> &v, const string &error,
                           const string &csrf,
                           const vector<pair<string, string>> *cases = nullptr,
                           int extra_rows = 0)
{
    stringstream page;
    page << page_head(heading);
    page << "<p class=\"breadcrumb\"><a href=\"/admin/problems\">题目管理</a> / "
         << heading << "</p>\n";
    page << "<header class=\"page-head\"><h1>" << heading << "</h1>"
                                                             "<p class=\"lead\">新建的题默认是草稿，先配好测试点再发布。</p></header>\n";

    if (!error.empty())
    {
        page << "<p class=\"empty\">保存失败：" << html_escape(error) << "</p>\n";
    }

    page << "<form class=\"form\" method=\"POST\" action=\"" << action << "\">\n";

    // CSRF 令牌走隐藏字段，不进 Cookie：别的网站能让你带上 Cookie，但读不到这个字段（FIX-19）
    page << "<input type=\"hidden\" name=\"csrf_token\" value=\""
         << html_escape(csrf) << "\">\n";

    // 版本号同理：页面打开时读到的值，保存时原样交回去（FIX-21）
    string form_version = field_value(v, "version");
    if (!form_version.empty())
    {
        page << "<input type=\"hidden\" name=\"version\" value=\""
             << html_escape(form_version) << "\">\n";
    }

    page << "<p>标题<br><input name=\"title\" size=\"60\" value=\""
         << html_escape(field_value(v, "title")) << "\"></p>\n";

    page << "<p>题目描述<br><textarea name=\"description\" rows=\"8\" cols=\"80\">"
         << html_escape(field_value(v, "description")) << "</textarea></p>\n";

    page << "<p>输入格式<br><textarea name=\"input_format\" rows=\"3\" cols=\"80\">"
         << html_escape(field_value(v, "input_format")) << "</textarea></p>\n";

    page << "<p>输出格式<br><textarea name=\"output_format\" rows=\"3\" cols=\"80\">"
         << html_escape(field_value(v, "output_format")) << "</textarea></p>\n";

    page << "<p>样例输入<br><textarea name=\"sample_input\" rows=\"4\" cols=\"80\">"
         << html_escape(field_value(v, "sample_input")) << "</textarea></p>\n";

    page << "<p>样例输出<br><textarea name=\"sample_output\" rows=\"4\" cols=\"80\">"
         << html_escape(field_value(v, "sample_output")) << "</textarea></p>\n";

    page << "<p>时间限制（毫秒）<input name=\"time_limit_ms\" size=\"8\" value=\""
         << html_escape(field_value(v, "time_limit_ms")) << "\">　"
                                                            "内存限制（MB）<input name=\"memory_limit_mb\" size=\"8\" value=\""
         << html_escape(field_value(v, "memory_limit_mb")) << "\"></p>\n";

    if (cases != nullptr)
    {
        string checked = (field_value(v, "is_published") == "1") ? " checked" : "";
        page << "<p><label><input type=\"checkbox\" name=\"is_published\" value=\"1\""
             << checked << "> 发布（不勾 = 草稿，学生看不到）</label></p>\n";

        page << "<h3>测试点</h3>\n";
        page << "<p class=\"note\">判题按 1、2、3…… 顺序跑，遇到第一个不通过就停。"
                "<b>序号不用你操心</b>：保存时服务端按顺序统一重新编号。"
                "要删中间的某一组，把它的两个框清空即可，后面的会自动补上来。</p>\n";

        int total = static_cast<int>(cases->size()) + extra_rows + 1; // 末尾常留一个空位
        for (int i = 0; i < total; i++)
        {
            string in_text = (i < static_cast<int>(cases->size())) ? (*cases)[i].first : "";
            string ans_text = (i < static_cast<int>(cases->size())) ? (*cases)[i].second : "";

            page << "<h4>第 " << (i + 1) << " 组</h4>\n";
            page << "<p>输入<br><textarea name=\"tc_in\" rows=\"4\" cols=\"80\">"
                 << html_escape(in_text) << "</textarea></p>\n";
            page << "<p>答案<br><textarea name=\"tc_ans\" rows=\"4\" cols=\"80\">"
                 << html_escape(ans_text) << "</textarea></p>\n";
        }

        page << "<p><a href=\"" << action << "?rows=" << (extra_rows + 1)
             << "\">＋ 再加一个空位</a></p>\n";
    }

    page << "<p><button type=\"submit\">保存</button></p>\n";
    page << "</form>\n";
    page << page_tail();
    return page.str();
}

string render_admin_problems()
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return page_head("题目管理") +
               "<p class=\"empty\">数据库暂时不可用。</p>\n" + page_tail();
    }

    // 管理页要看全部：未发布的、已软删除的都得列出来（学生侧才加那两个 WHERE）
    if (mysql_query(conn, "SELECT problem_id, title, is_published, is_deleted "
                          "FROM problems ORDER BY problem_id") != 0)
    {
        log_write(LOG_ERROR, "db",
                  "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
        return page_head("题目管理") +
               "<p class=\"empty\">题库暂时读不出来。</p>\n" + page_tail();
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL)
    {
        return page_head("题目管理") +
               "<p class=\"empty\">题库暂时读不出来。</p>\n" + page_tail();
    }

    stringstream page;
    page << page_head("题目管理");
    page << "<header class=\"page-head\"><h1>题目管理</h1>"
            "<p class=\"lead\">这里能看见未发布和已删除的题 —— 学生看不到。</p></header>\n";
    page << "<p><a href=\"/admin/problem/new\">+ 新建题目</a> "
            "<a href=\"/admin/users\">用户管理 →</a></p>\n";

    page << "<table class=\"sub-table\">\n";
    page << "<tr><th>题号</th><th>标题</th><th>状态</th><th>测试点</th><th>操作</th></tr>\n";

    int count = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL)
    {
        int pid = stoi(row[0]);
        bool published = (row[2] != NULL && string(row[2]) == "1");
        bool deleted = (row[3] != NULL && string(row[3]) == "1");

        string state;
        string action;
        if (deleted)
        {
            state = "已删除（软删除）";
            action = "—";
        }
        else
        {
            state = published ? "已发布" : "草稿（学生看不到）";
            action = "<a href=\"/admin/problem/" + to_string(pid) + "/edit\">编辑</a>";
        }

        page << "<tr>"
             << "<td>" << pid << "</td>"
             << "<td>" << html_escape(row[1] ? row[1] : "") << "</td>"
             << "<td>" << state << "</td>"
             << "<td>" << count_testcases(pid) << "</td>"
             << "<td>" << action << "</td>"
             << "</tr>\n";
        count++;
    }

    page << "</table>\n";
    if (count == 0)
    {
        page << "<p class=\"empty\">还没有题目。</p>\n";
    }
    page << page_tail();

    mysql_free_result(res);
    return page.str();
}

string render_admin_users(const string &me, const string &csrf)
{
    vector<UserRow> rows = list_users();

    stringstream page;
    page << page_head("用户管理");
    page << "<header class=\"page-head\"><h1>用户管理</h1>"
            "<p class=\"lead\">重置口令、禁用账号、授予或收回管理员。</p></header>\n";
    page << "<p><a href=\"/admin/problems\">← 题目管理</a></p>\n";

    page << "<table class=\"sub-table\">\n";
    page << "<tr><th>用户名</th><th>角色</th><th>状态</th><th>注册时间</th>"
            "<th>重置口令</th><th>操作</th></tr>\n";

    for (const UserRow &u : rows)
    {
        string role = u.is_admin ? "管理员" : "学生";
        string state = u.is_disabled ? "已禁用" : "正常";

        page << "<tr><td>" << html_escape(u.username) << "</td>"
             << "<td>" << role << "</td>";

        if (u.username == me)
        {
            // 不能对自己操作：禁用自己 / 收回自己的管理员 = 把自己的钥匙拆了
            page << "<td>" << state << "（你自己）</td>"
                 << "<td>" << html_escape(u.created_at) << "</td>"
                 << "<td>—</td><td>不能对自己操作</td>";
        }
        else
        {
            string uid = to_string(u.user_id);

            page << "<td>" << state << "</td>"
                 << "<td>" << html_escape(u.created_at) << "</td>";

            string hidden = "<input type=\"hidden\" name=\"csrf_token\" value=\"" +
                            html_escape(csrf) + "\">";

            page << "<td><form method=\"POST\" action=\"/admin/user/" << uid
                 << "/reset_password\" style=\"display:inline\">" << hidden
                 << "<input name=\"new_password\" type=\"password\" size=\"10\" placeholder=\"新口令\">"
                    "<button type=\"submit\">重置</button></form></td>";

            page << "<td>"
                 << "<form method=\"POST\" action=\"/admin/user/" << uid
                 << "/toggle_disabled\" style=\"display:inline\">" << hidden
                 << "<button type=\"submit\">"
                 << (u.is_disabled ? "解禁" : "禁用") << "</button></form>　"
                 << "<form method=\"POST\" action=\"/admin/user/" << uid
                 << "/toggle_admin\" style=\"display:inline\">" << hidden
                 << "<button type=\"submit\">"
                 << (u.is_admin ? "收回管理员" : "设为管理员") << "</button></form>"
                 << "</td>";
        }

        page << "</tr>\n";
    }

    page << "</table>\n";
    page << page_tail();
    return page.str();
}

// 读环境变量 OJ_JUDGE_THREADS；非法值回落到默认值并记一条 WARN。
int judge_thread_count()
{
    const char *env = getenv("OJ_JUDGE_THREADS");
    if (env == nullptr)
    {
        return JUDGE_THREADS_DEFAULT;
    }

    try
    {
        // pos 告诉 stoi 吃到第几个字符就停了 —— 必须整串吃完才算合法，
        // 否则 "4abc" 会被当成 4（stoi 只看前缀，静默给错值）。
        size_t pos = 0;
        int n = stoi(env, &pos);
        if (pos == strlen(env) && n >= 1 && n <= JUDGE_THREADS_MAX)
        {
            return n;
        }
    }
    catch (const exception &)
    {
    }

    log_write(LOG_WARN, "judge",
              "fn=judge_thread_count reason=bad_env env=" + log_sanitize(env) +
                  " fallback=" + to_string(JUDGE_THREADS_DEFAULT));
    return JUDGE_THREADS_DEFAULT;
}

void judge_worker()
{
    while (true)
    {
        JudgeTask t;

        {
            unique_lock<mutex> lock(judge_mtx);
            judge_cv.wait(lock, []
                          { return !judge_queue.empty(); });

            t = judge_queue.front();
            judge_queue.pop();
        }

        set_status(t.job_id, "判题中");

        string data_dir = "/home/ubuntu/online-OJ/judge_demo/testdata/" +
                          to_string(t.problem_id);
        string src_path = t.work_dir + "/submission.cpp";

        string out;
        bool started = run_judge(src_path, data_dir, t.tl_ms, t.ml_mb, out);

        JudgeSummary s = parse_judge(out);
        int total = count_testcases(t.problem_id);

        finish_job(t.job_id, out, s, total, started);

        std::error_code ec;
        std::filesystem::remove_all(t.work_dir, ec);
        if (ec)
        {
            log_write(LOG_ERROR, "fs",
                      "fn=" + string(__func__) + " err=" + ec.message());
        }

        log_write(LOG_INFO, "judge",
                  "job=" + t.job_id +
                      " user=" + log_sanitize(t.username) +
                      " problem=" + to_string(t.problem_id) +
                      " verdict=" + s.verdict +
                      " passed=" + to_string(s.passed) + "/" + to_string(total) +
                      " time_ms=" + to_string(s.time_ms) +
                      " mem_kb=" + to_string(s.memory_kb) +
                      " started=" + (started ? "1" : "0"));

        if (!started)
        {
            log_write(LOG_ERROR, "judge_error",
                      "job=" + t.job_id + " code=127 out=" + log_sanitize(out.substr(0, 200)));
        }
        else if (s.verdict == "NO_DATA")
        {
            log_write(LOG_WARN, "judge_error",
                      "job=" + t.job_id + " problem=" + to_string(t.problem_id) +
                          " reason=no_testdata");
        }
        else if (s.verdict.empty())
        {
            log_write(LOG_WARN, "judge_error",
                      "job=" + t.job_id + " reason=unparsable out=" + log_sanitize(out.substr(0, 200)));
        }
    }
}

void handle_request(int conn_fd, const string &req)
{
    req_start = chrono::steady_clock::now();
    req_method.clear();
    req_path.clear();
    req_user.clear();

    size_t head_end = req.find("\r\n\r\n");
    istringstream iss(req);
    string method, target, version;
    string path, query;
    iss >> method >> target >> version;
    req_method = method;
    req_path = target;

    size_t qpos = target.find('?');
    if (qpos != string::npos)
    {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }
    else
    {
        path = target;
        query = "";
    }
    log_write(LOG_DEBUG, "request_detail",
              "method=" + method + " target=" + log_sanitize(target) +
                  " version=" + version);

    string line;
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
        for (char &ch : name)
        {
            ch = tolower(ch);
        }
        string value = line.substr(pos + 1);
        if (!value.empty() && value.front() == ' ')
        {
            value.erase(0, 1);
        }

        headers[name] = value;
    }

    string session_id;
    if (headers.count("cookie"))
    {
        session_id = cookie_value(headers["cookie"], "session");
    }
    string current_user;
    if (!session_id.empty())
    {
        current_user = get_session_user(session_id);
    }

    nav_user = current_user;
    req_user = current_user;

    size_t body_need = 0;
    int cl_status = content_length_of(req, body_need);
    if (cl_status == 1)
    {
        send_response(conn_fd, 400, "Content-Length 不合法\n");
        return;
    }
    if (cl_status == -1)
    {
        body_need = 0;
    }

    if (body_need > MAX_BODY_BYTES)
    {
        send_response(conn_fd, 413, "请求体太大\n");
        return;
    }

    string body = req.substr(head_end + 4, body_need);
    map<string, string> form = parse_form(body);
    map<string, string> query_params = parse_form(query);

    size_t total_len = head_end + 4 + body_need;
    if (req.size() > total_len)
    {
        log_write(LOG_DEBUG, "request_pipelined",
                  "extra=" + to_string(req.size() - total_len));
    }

    // 首页
    if (path == "/")
    {
        ifstream f("www/index.html");
        stringstream ss;
        ss << f.rdbuf();
        string home = ss.str();
        send_response(conn_fd, 200, home, "text/html; charset=utf-8");
    }
    // 回显页
    else if (path == "/echo")
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
    // 题目详情
    else if (path.compare(0, PROBLEM_PREFIX.size(), PROBLEM_PREFIX) == 0)
    {
        string num = path.substr(PROBLEM_PREFIX.size());
        string html;
        if (!num.empty() && num.find_first_not_of("0123456789") == string::npos)
        {
            try
            {
                if (render_problem(stoi(num), current_user, html))
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
    // 提交代码
    else if (method == "POST" && path.compare(0, SUBMIT_PREFIX.size(), SUBMIT_PREFIX) == 0)
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

        string num = path.substr(SUBMIT_PREFIX.size());
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

        if (code.size() > MAX_CODE_BYTES)
        {
            send_response(conn_fd, 413,
                          "源码超过 " + to_string(MAX_CODE_BYTES / 1024) +
                              " KB，请精简后重试\n");
            return;
        }

        char tmpl[] = "/tmp/oj_XXXXXX";
        char *dir = mkdtemp(tmpl);
        if (dir == nullptr)
        {
            send_response(conn_fd, 500, "服务器开不了临时目录\n");
            return;
        }
        string work_dir = dir;

        string src_path = work_dir + "/submission.cpp";

        ofstream f(src_path);
        f << code;
        f.close();
        if (f.fail())
        {
            send_response(conn_fd, 500, "代码保存失败\n");
            return;
        }

        string job_id = make_job_id();

        MYSQL *conn = get_conn();
        if (conn == nullptr)
        {
            send_response(conn_fd, 500, "数据库暂时不可用\n");
            return;
        }

        string sql = "INSERT INTO submissions "
                     "(job_id, username, problem_id, status, result, source, created_at) "
                     "VALUES ('" +
                     job_id + "', '" + escape_sql(current_user) + "', " +
                     to_string(problem_id) +
                     ", '排队中', NULL, '" + escape_sql(code) + "', NOW(6))";

        if (mysql_query(conn, sql.c_str()) != 0)
        {
            log_write(LOG_ERROR, "db",
                      "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
            send_response(conn_fd, 500, "提交记录保存失败\n");
            return;
        }

        // 队列有界（FIX-12 / OJ-JDG-08）：满了就不收，而不是无限堆积。
        // 容量判断和 push 在同一把锁里，避免两个工人同时看到"还剩一个位"。
        bool queued = false;
        {
            lock_guard<mutex> lock(judge_mtx);
            if (judge_queue.size() < JUDGE_QUEUE_MAX)
            {
                judge_queue.push({job_id, work_dir, problem_id, tl_ms, ml_mb, current_user});
                queued = true;
            }
        }

        if (!queued)
        {
            // 撤掉 Temporary 目录和刚落库的记录 ——
            // 否则库里会留下永远「排队中」的孤儿，/tmp 也会堆满没人清的目录。
            std::error_code ec;
            std::filesystem::remove_all(work_dir, ec);
            if (ec)
            {
                log_write(LOG_ERROR, "fs",
                          "fn=" + string(__func__) + " op=remove work_dir err=" + ec.message());
            }

            string del = "DELETE FROM submissions WHERE job_id = '" +
                         escape_sql(job_id) + "'";
            if (mysql_query(conn, del.c_str()) != 0)
            {
                log_write(LOG_ERROR, "db",
                          "fn=" + string(__func__) + " op=rollback job=" + job_id +
                              " err=" + string(mysql_error(conn)));
            }

            log_write(LOG_WARN, "judge",
                      "fn=" + string(__func__) + " reason=queue_full job=" + job_id +
                          " user=" + log_sanitize(current_user) +
                          " max=" + to_string(JUDGE_QUEUE_MAX));

            // 用户是在题目页提交的，返回整页 HTML 比一行纯文本好收拾：
            // 有导航、有返回题目页的链接，不用自己按后退。
            string page = page_head("判题队列已满");
            page += "<h2>判题队列已满</h2>\n";
            page += "<p>排队上限是 " + to_string(JUDGE_QUEUE_MAX) +
                    " 份提交，现在已满。请稍等几秒后重新提交。</p>\n";
            page += "<p><a href=\"/problem/" + to_string(problem_id) +
                    "\">返回题目页</a></p>\n";
            page += page_tail();

            // Retry-After 是 503 的标准搭档：告诉客户端多久之后再来
            send_response(conn_fd, 503, page, "text/html; charset=utf-8",
                          "Retry-After: 5\r\n");
            return;
        }

        judge_cv.notify_one();

        log_write(LOG_INFO, "submit",
                  "job=" + job_id + " problem=" + to_string(problem_id) +
                      " user=" + log_sanitize(current_user));
        send_redirect(conn_fd, "/result/" + job_id);
    }
    // 提交结果
    else if (path.compare(0, RESULT_PREFIX.size(), RESULT_PREFIX) == 0)
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

        string id = path.substr(RESULT_PREFIX.size());

        string owner;
        if (!valid_job_id(id) || !get_submission_owner(id, owner))
        {
            send_response(conn_fd, 404, "没有这条提交\n");
            return;
        }

        if (owner != current_user && !user_is_admin(current_user))
        {
            send_response(conn_fd, 403, "这不是你的提交\n");
            return;
        }

        string html;
        if (!render_result(id, html))
        {
            send_response(conn_fd, 404, "没有这条提交\n");
            return;
        }
        send_response(conn_fd, 200, html, "text/html; charset=utf-8");
    }
    // 注册
    else if (path == "/register")
    {
        if (method == "POST")
        {
            string username, password, password2;
            if (form.count("username"))
                username = trim(form["username"]);
            if (form.count("password"))
                password = form["password"];
            if (form.count("password2"))
                password2 = form["password2"];

            if (username.empty() || password.empty())
            {
                send_response(conn_fd, 400, "用户名和密码都不能为空\n");
                return;
            }
            if (password != password2)
            {
                send_response(conn_fd, 400, "两次输入的新密码不一样\n");
                return;
            }
            if (username.size() > 32)
            {
                send_response(conn_fd, 400, "用户名最多 32 个字符\n");
                return;
            }

            string hash = make_password_hash(password);
            if (hash.empty())
            {
                send_response(conn_fd, 500, "注册失败，请重试\n");
                return;
            }

            MYSQL *conn = get_conn();
            if (conn == nullptr)
            {
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            string sql = "INSERT INTO users (username, password_hash, created_at) "
                         "VALUES ('" +
                         escape_sql(username) + "', '" +
                         escape_sql(hash) + "', NOW())";

            if (mysql_query(conn, sql.c_str()) != 0)
            {
                // 1062 = 唯一键冲突，也就是用户名已经存在
                if (mysql_errno(conn) == 1062)
                {
                    send_response(conn_fd, 200, "这个用户名已经被注册了\n");
                }
                else
                {
                    log_write(LOG_ERROR, "db",
                              "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
                    send_response(conn_fd, 500, "注册失败，请重试\n");
                }
                return;
            }

            send_redirect(conn_fd, "/login");
            return;
        }
        else
        {
            stringstream page;
            page << page_head("注册")
                 << "<div class=\"form\">\n"
                 << "<form method=\"POST\" action=\"/register\">\n"
                 << "<p>用户名：<input name=\"username\" maxlength=\"32\"></p>\n"
                 << "<p>密码：<input name=\"password\" type=\"password\"></p>\n"
                 << "<p>再输一次：<input name=\"password2\" type=\"password\"></p>\n"
                 << "<button type=\"submit\">注册</button>\n"
                 << "</form>\n"
                 << "<p class=\"note\">已经有账号了？<a href=\"/login\">去登录</a></p>\n"
                 << "</div>\n"
                 << page_tail();
            send_response(conn_fd, 200, page.str(), "text/html; charset=utf-8");
        }
    }
    // 登录
    else if (path == "/login")
    {
        if (method == "POST")
        {
            string username, password;
            if (form.count("username"))
                username = form["username"];
            if (form.count("password"))
                password = form["password"];

            if (username.empty() || password.empty())
            {
                send_response(conn_fd, 400, "用户名和密码都要填\n");
                return;
            }

            MYSQL *conn = get_conn();
            if (conn == nullptr)
            {
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            string sql = "SELECT password_hash, is_disabled FROM users "
                         "WHERE username = '" +
                         escape_sql(username) + "'";

            if (mysql_query(conn, sql.c_str()) != 0)
            {
                log_write(LOG_ERROR, "db",
                          "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            MYSQL_RES *res = mysql_store_result(conn);
            if (res == nullptr)
            {
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            MYSQL_ROW row = mysql_fetch_row(res);

            bool ok = false;
            bool disabled = false;
            if (row != nullptr)
            {
                ok = scrypt_verify(password, row[0]);
                disabled = (row[1] != nullptr && string(row[1]) == "1");
            }

            mysql_free_result(res);

            // 用户不存在 和 密码不对，故意返回同一句话
            if (!ok)
            {
                send_response(conn_fd, 200, "用户名或密码不对\n");
                return;
            }

            // 禁用检查放密码之后：密码答对了才说这句
            if (disabled)
            {
                send_response(conn_fd, 200, "这个账号已被禁用，请联系管理员\n");
                return;
            }

            string sid = create_session(username);
            if (sid.empty())
            {
                send_response(conn_fd, 500, "登录失败，请重试\n");
                return;
            }

            string cookie = "Set-Cookie: session=" + sid +
                            "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                            to_string(SESSION_TTL_SECONDS) + "\r\n";

            send_redirect(conn_fd, "/problems", cookie);
            return;
        }
        else
        {
            stringstream page;
            page << page_head("登录")
                 << "<div class=\"form\">\n"
                 << "<form method=\"POST\" action=\"/login\">\n"
                 << "<p>用户名：<input name=\"username\"></p>\n"
                 << "<p>密码：<input name=\"password\" type=\"password\"></p>\n"
                 << "<button type=\"submit\">登录</button>\n"
                 << "</form>\n"
                 << "</div>\n"
                 << page_tail();
            send_response(conn_fd, 200, page.str(), "text/html; charset=utf-8");
        }
    }
    // 退出登录
    else if (path == "/logout")
    {
        if (!session_id.empty())
        {
            destroy_session(session_id);
        }

        string cookie = "Set-Cookie: session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n";
        send_redirect(conn_fd, "/", cookie);
    }
    // 个人中心
    else if (path == "/profile")
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

        if (method == "POST")
        {
            string old_pw, new_pw, new_pw2;
            if (form.count("old_password"))
                old_pw = form["old_password"];
            if (form.count("new_password"))
                new_pw = form["new_password"];
            if (form.count("new_password2"))
                new_pw2 = form["new_password2"];

            if (old_pw.empty() || new_pw.empty())
            {
                send_response(conn_fd, 400, "三个框都要填\n");
                return;
            }
            if (new_pw != new_pw2)
            {
                send_response(conn_fd, 400, "两次输入的新密码不一样\n");
                return;
            }

            // 先查旧哈希
            MYSQL *conn = get_conn();
            if (conn == nullptr)
            {
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            string sql = "SELECT password_hash FROM users WHERE username = '" +
                         escape_sql(current_user) + "'";
            if (mysql_query(conn, sql.c_str()) != 0)
            {
                send_response(conn_fd, 500, "数据库暂时不可用\n");
                return;
            }

            MYSQL_RES *res = mysql_store_result(conn);
            MYSQL_ROW row = (res != nullptr) ? mysql_fetch_row(res) : nullptr;

            bool old_ok = (row != nullptr && row[0] != nullptr &&
                           scrypt_verify(old_pw, row[0]));
            if (res != nullptr)
            {
                mysql_free_result(res);
            }

            if (!old_ok)
            {
                send_response(conn_fd, 200, "原密码不对\n");
                return;
            }

            // 生成新哈希并写回
            string new_hash = make_password_hash(new_pw);
            if (new_hash.empty())
            {
                send_response(conn_fd, 500, "修改失败，请重试\n");
                return;
            }

            string up = "UPDATE users SET password_hash = '" +
                        escape_sql(new_hash) + "' WHERE username = '" +
                        escape_sql(current_user) + "'";
            if (mysql_query(conn, up.c_str()) != 0)
            {
                log_write(LOG_ERROR, "db",
                          "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
                send_response(conn_fd, 500, "修改失败，请重试\n");
                return;
            }

            // 把这个人其它的会话都踢掉
            string del = "DELETE FROM sessions WHERE username = '" + escape_sql(current_user) +
                         "' AND session_id != '" + escape_sql(session_id) + "'";
            mysql_query(conn, del.c_str());

            send_response(conn_fd, 200, "密码已修改\n");
            return;
        }

        stringstream page;
        page << page_head("个人中心")
             << "<h1>" << html_escape(current_user) << "</h1>\n"
             << "<div class=\"form\">\n"
             << "<form method=\"POST\" action=\"/profile\">\n"
             << "<p>原密码：<input name=\"old_password\" type=\"password\"></p>\n"
             << "<p>新密码：<input name=\"new_password\" type=\"password\"></p>\n"
             << "<p>再输一次：<input name=\"new_password2\" type=\"password\"></p>\n"
             << "<button type=\"submit\">修改密码</button>\n"
             << "</form>\n"
             << "</div>\n"
             << page_tail();
        send_response(conn_fd, 200, page.str(), "text/html; charset=utf-8");
    }
    // 题库列表
    else if (path == "/problems")
    {
        send_response(conn_fd, 200, render_problems(), "text/html; charset=utf-8");
    }
    // 我的提交
    else if (path == "/submissions")
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

        string filter_user;
        if (query_params.count("user"))
        {
            filter_user = trim(query_params["user"]);
        }

        send_response(conn_fd, 200,
                      render_submissions(current_user, filter_user),
                      "text/html; charset=utf-8");
    }
    // 排行榜
    else if (path == "/rank")
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }
        send_response(conn_fd, 200, render_rank(), "text/html; charset=utf-8");
    }
    // 管理后台：整个 /admin/ 子树一个门卫，以后加页面不用再各写一遍权限检查
    else if (path.compare(0, ADMIN_PREFIX.size(), ADMIN_PREFIX) == 0)
    {
        if (!admin_guard(conn_fd, current_user))
        {
            return;
        }

        // CSRF（FIX-19）：管理后台所有写操作进门先验令牌。
        // 令牌只在本页面的隐藏字段里 —— 别的网站能借你的 Cookie 发请求，但读不到页面内容。
        string csrf = get_session_csrf(session_id);
        if (method == "POST" && !csrf_verify(session_id, field_value(form, "csrf_token")))
        {
            log_write(LOG_WARN, "csrf",
                      "path=" + log_sanitize(path) +
                          " user=" + log_sanitize(current_user));
            send_response(conn_fd, 403, "表单已失效，请刷新页面后重试\n");
            return;
        }

        if (path == "/admin/problems")
        {
            send_response(conn_fd, 200, render_admin_problems(),
                          "text/html; charset=utf-8");
        }
        else if (path == "/admin/users")
        {
            send_response(conn_fd, 200, render_admin_users(current_user, csrf),
                          "text/html; charset=utf-8");
        }
        else if (path == "/admin/problem/new")
        {
            if (method == "POST")
            {
                string title = trim(field_value(form, "title"));
                string description = field_value(form, "description");
                int tl = 0;
                int ml = 0;

                string error;
                if (title.empty())
                {
                    error = "标题不能为空";
                }
                else if (description.empty())
                {
                    error = "题面不能为空";
                }
                else if (trim(field_value(form, "input_format")).empty())
                {
                    error = "输入格式不能为空";
                }
                else if (trim(field_value(form, "output_format")).empty())
                {
                    error = "输出格式不能为空";
                }
                else if (!parse_positive_int(field_value(form, "time_limit_ms"), tl))
                {
                    error = "时间限制要填正整数（毫秒）";
                }
                else if (!parse_positive_int(field_value(form, "memory_limit_mb"), ml))
                {
                    error = "内存限制要填正整数（MB）";
                }

                if (!error.empty())
                {
                    // 400 也要把刚填的内容原样放回去 —— 这是表单页的基本礼貌
                    send_response(conn_fd, 400,
                                  render_problem_form("新建题目", "/admin/problem/new", form, error, csrf),
                                  "text/html; charset=utf-8");
                    return;
                }

                MYSQL *conn = get_conn();
                if (conn == nullptr)
                {
                    send_response(conn_fd, 500, "数据库暂时不可用\n");
                    return;
                }

                string sql = "INSERT INTO problems "
                             "(title, description, input_format, output_format, "
                             " sample_input, sample_output, time_limit_ms, memory_limit_mb, "
                             " is_published, is_deleted, created_at) "
                             "VALUES ('" +
                             escape_sql(title) + "', '" +
                             escape_sql(description) + "', '" +
                             escape_sql(field_value(form, "input_format")) + "', '" +
                             escape_sql(field_value(form, "output_format")) + "', '" +
                             escape_sql(field_value(form, "sample_input")) + "', '" +
                             escape_sql(field_value(form, "sample_output")) + "', " +
                             to_string(tl) + ", " + to_string(ml) +
                             ", 0, 0, NOW())"; // ← 发布状态写死 0

                if (mysql_query(conn, sql.c_str()) != 0)
                {
                    log_write(LOG_ERROR, "db",
                              "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
                    send_response(conn_fd, 500, "保存失败，请重试\n");
                    return;
                }

                int new_id = static_cast<int>(mysql_insert_id(conn));
                log_write(LOG_INFO, "admin_problem_new",
                          "id=" + to_string(new_id) + " by=" + log_sanitize(current_user));

                send_redirect(conn_fd, "/admin/problem/" + to_string(new_id) + "/edit");
                return;
            }

            send_response(conn_fd, 200,
                          render_problem_form("新建题目", "/admin/problem/new", {}, "", csrf),
                          "text/html; charset=utf-8");
        }
        else
        {
            int uid = 0;
            string uact;
            if (parse_admin_user_path(path, uid, uact))
            {
                if (method != "POST")
                {
                    send_response(conn_fd, 405, "这个操作必须用 POST\n");
                    return;
                }

                UserRow u;
                if (!get_user_by_id(uid, u))
                {
                    send_response(conn_fd, 404, "没有这个用户\n");
                    return;
                }
                if (u.username == current_user)
                {
                    send_response(conn_fd, 400, "不能对自己操作\n");
                    return;
                }

                if (uact == "reset_password")
                {
                    // 「遗忘口令由管理员重置」—— 这条路不要求原口令
                    string new_pw = field_value(form, "new_password");
                    if (new_pw.empty())
                    {
                        send_response(conn_fd, 400, "新口令不能为空\n");
                        return;
                    }

                    string hash = make_password_hash(new_pw);
                    if (hash.empty() || !set_user_password(uid, hash))
                    {
                        send_response(conn_fd, 500, "重置失败，请重试\n");
                        return;
                    }

                    send_redirect(conn_fd, "/admin/users");
                    return;
                }

                if (uact == "toggle_disabled")
                {
                    // 兜底：至少保留一个可用管理员。
                    // 其实「不能对自己操作」已经把这条兜住了（按当前路径触发不了），留着防将来放开
                    if (!u.is_disabled && u.is_admin && count_active_admins() <= 1)
                    {
                        send_response(conn_fd, 400, "这是最后一个可用的管理员，不能禁用\n");
                        return;
                    }

                    if (!set_user_disabled(uid, !u.is_disabled))
                    {
                        send_response(conn_fd, 500, "操作失败，请重试\n");
                        return;
                    }

                    send_redirect(conn_fd, "/admin/users");
                    return;
                }

                if (uact == "toggle_admin")
                {
                    if (!set_user_admin(uid, !u.is_admin))
                    {
                        send_response(conn_fd, 500, "操作失败，请重试\n");
                        return;
                    }

                    send_redirect(conn_fd, "/admin/users");
                    return;
                }

                send_response(conn_fd, 404, "没有这个管理页面\n");
                return;
            }
            int pid = 0;
            string act;
            if (!parse_admin_problem_path(path, pid, act))
            {
                send_response(conn_fd, 404, "没有这个管理页面\n");
                return;
            }

            // 删除：只认 POST（GET 会被预取/爬虫触发）
            if (act == "delete")
            {
                if (method != "POST")
                {
                    send_response(conn_fd, 405, "删除必须用 POST\n");
                    return;
                }

                MYSQL *conn = get_conn();
                if (conn == nullptr)
                {
                    send_response(conn_fd, 500, "数据库暂时不可用\n");
                    return;
                }

                // 软删除：行留着，历史提交才不会变孤儿；顺手清掉发布状态
                string sql = "UPDATE problems SET is_deleted = 1, is_published = 0 "
                             "WHERE problem_id = " +
                             to_string(pid);
                if (mysql_query(conn, sql.c_str()) != 0)
                {
                    log_write(LOG_ERROR, "db",
                              "fn=" + string(__func__) + " err=" + string(mysql_error(conn)));
                    send_response(conn_fd, 500, "删除失败，请重试\n");
                    return;
                }

                send_redirect(conn_fd, "/admin/problems");
                return;
            }

            if (act != "edit")
            {
                send_response(conn_fd, 404, "没有这个管理页面\n");
                return;
            }

            ProblemRow p;
            if (!get_problem_for_admin(pid, p))
            {
                send_response(conn_fd, 404, "没有这道题\n");
                return;
            }
            if (p.is_deleted)
            {
                send_response(conn_fd, 403, "这道题已删除（软删除），不能再编辑了\n");
                return;
            }

            string action = "/admin/problem/" + to_string(pid) + "/edit";

            if (method == "POST")
            {
                map<string, vector<string>> multi = parse_form_multi(body);

                map<string, string> values;
                for (const auto &kv : multi)
                {
                    if (!kv.second.empty())
                    {
                        values[kv.first] = kv.second.front();
                    }
                }
                values["title"] = trim(field_value(values, "title"));

                bool publish = (field_value(values, "is_published") == "1");
                int tl = 0;
                int ml = 0;
                string error;

                if (values["title"].empty())
                {
                    error = "标题不能为空";
                }
                else if (trim(field_value(values, "description")).empty())
                {
                    error = "题面不能为空";
                }
                else if (trim(field_value(values, "input_format")).empty())
                {
                    error = "输入格式不能为空";
                }
                else if (trim(field_value(values, "output_format")).empty())
                {
                    error = "输出格式不能为空";
                }
                else if (!parse_positive_int(field_value(values, "time_limit_ms"), tl))
                {
                    error = "时间限制要填正整数（毫秒）";
                }
                else if (!parse_positive_int(field_value(values, "memory_limit_mb"), ml))
                {
                    error = "内存限制要填正整数（MB）";
                }

                // 测试点：两个列表天然一一对应，顺序就是表单里的顺序
                vector<string> ins, answers;
                auto it_in = multi.find("tc_in");
                if (it_in != multi.end())
                {
                    ins = it_in->second;
                }
                auto it_ans = multi.find("tc_ans");
                if (it_ans != multi.end())
                {
                    answers = it_ans->second;
                }

                vector<pair<string, string>> cases;
                if (ins.size() != answers.size())
                {
                    error = "表单读到的输入框与答案框对不上，请刷新页面重来";
                }
                else
                {
                    for (size_t i = 0; i < ins.size(); i++)
                    {
                        string in_text = normalize_newlines(ins[i]);
                        string ans_text = normalize_newlines(answers[i]);
                        // 两个都空的块 = 没用上的空位，丢掉
                        if (trim(in_text).empty() && trim(ans_text).empty())
                        {
                            continue;
                        }
                        cases.push_back({in_text, ans_text});
                    }
                }

                if (error.empty() && publish && cases.empty())
                {
                    error = "这道题一个测试点都没有，不能发布 —— 请先填好测试点，再勾「发布」";
                }

                if (!error.empty())
                {
                    send_response(conn_fd, 400,
                                  render_problem_form("编辑题目", action, values, error, csrf, &cases, 0),
                                  "text/html; charset=utf-8");
                    return;
                }

                // FIX-21：版本号要在**动磁盘之前**比。
                // 反过来（先落盘再发现冲突）磁盘已经被覆盖了，想回都回不去。
                int form_version = 0;
                if (!parse_positive_int(field_value(values, "version"), form_version))
                {
                    send_response(conn_fd, 400,
                                  render_problem_form("编辑题目", action, values,
                                                      "表单里没有版本号，请刷新页面重来", csrf, &cases, 0),
                                  "text/html; charset=utf-8");
                    return;
                }
                if (form_version != p.version)
                {
                    log_write(LOG_WARN, "admin",
                              "problem=" + to_string(pid) +
                                  " conflict=version" +
                                  " form=" + to_string(form_version) +
                                  " db=" + to_string(p.version) +
                                  " by=" + log_sanitize(current_user));
                    send_response(conn_fd, 409, render_conflict_page(pid),
                                  "text/html; charset=utf-8");
                    return;
                }

                // 顺序：先落盘、再改库。磁盘是真相、库是账本
                vector<TestcaseMeta> meta;
                if (!save_testcases_to_disk(pid, cases, meta))
                {
                    send_response(conn_fd, 500, "测试点写入磁盘失败\n");
                    return;
                }
                UpdateResult ur = update_problem(pid, values, tl, ml, publish, form_version);
                if (ur != UpdateResult::Ok)
                {
                    // 上面已经比过一次版本，这里还能撞上只有一种可能：
                    // 两次比对之间另一个管理员刚好提交完（窗口是毫秒级）。
                    // 此时磁盘里已经是新测试点、库里还是旧的 —— 如实报冲突，让他刷新重做。
                    if (ur == UpdateResult::Conflict)
                    {
                        log_write(LOG_WARN, "admin",
                                  "problem=" + to_string(pid) +
                                      " conflict=race_after_disk" +
                                      " by=" + log_sanitize(current_user));
                        send_response(conn_fd, 409, render_conflict_page(pid),
                                      "text/html; charset=utf-8");
                        return;
                    }
                    send_response(conn_fd, 500, "保存失败，请重试\n");
                    return;
                }
                if (!replace_testcases(pid, meta))
                {
                    send_response(conn_fd, 500, "测试点元信息保存失败，请重试\n");
                    return;
                }

                send_redirect(conn_fd, "/admin/problems");
                return;
            }

            // GET：把库里的字段和盘上的测试点填回表单
            map<string, string> values;
            values["title"] = p.title;
            values["description"] = p.description;
            values["input_format"] = p.input_format;
            values["output_format"] = p.output_format;
            values["sample_input"] = p.sample_input;
            values["sample_output"] = p.sample_output;
            values["time_limit_ms"] = to_string(p.time_limit_ms);
            values["memory_limit_mb"] = to_string(p.memory_limit_mb);
            values["version"] = to_string(p.version);
            if (p.is_published)
            {
                values["is_published"] = "1";
            }

            int extra_rows = 0;
            auto it_rows = query_params.find("rows");
            if (it_rows != query_params.end())
            {
                long r = 0;
                if (to_long(it_rows->second, r) && r > 0)
                {
                    extra_rows = (r > 20) ? 20 : static_cast<int>(r);
                }
            }

            vector<pair<string, string>> cases = load_testcases(pid);
            send_response(conn_fd, 200,
                          render_problem_form("编辑题目", action, values, "", csrf, &cases, extra_rows),
                          "text/html; charset=utf-8");
        }
    }
    // 当前登录状态（调试用）
    else if (path == "/whoami")
    {
        send_response(conn_fd, 200,
                      current_user.empty() ? "未登录\n" : current_user + "\n");
    }
    // 其余按静态文件处理
    else
    {
        path = urldecode(path, false);
        string road = "www" + path;

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
    if (!log_init("logs/oj.log", log_level_from_env()))
    {
        cerr << "log_init failed，日志降级到 stderr\n";
    }
    int judge_n = judge_thread_count();
    log_write(LOG_INFO, "startup",
              "port=8080 workers=4 judges=" + to_string(judge_n) +
                  " queue_max=" + to_string(JUDGE_QUEUE_MAX));
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


    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    int epfd = epoll_create1(0);

    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[64];
    map<int, string> conn_bufs;

    thread t1(worker), t2(worker), t3(worker), t4(worker);

    // 判题工人数按环境变量可配（FIX-12 / OJ-JDG-08）。
    // vector<thread> 在 main 里一直活着；main 是死循环，不会走到析构那一步。
    vector<thread> judge_pool;
    for (int i = 0; i < judge_n; i++)
    {
        judge_pool.push_back(thread(judge_worker));
    }

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

                    log_write(LOG_DEBUG, "conn", "event=accept fd=" + to_string(conn_fd));
                }
            }
            else
            {
                char buf[4096];
                ssize_t cnt = read(fd, buf, sizeof(buf));

                if (cnt > 0)
                {
                    conn_bufs[fd].append(buf, cnt);
                    log_write(LOG_DEBUG, "conn",
                              "event=read fd=" + to_string(fd) +
                                  " bytes=" + to_string(cnt) +
                                  " buffered=" + to_string(conn_bufs[fd].size()));

                    // 硬上限：攒了 2MB 还凑不出一个完整请求，直接断开。
                    // 语义上限（1MB）走 413，这里只负责“内存有界”
                    if (conn_bufs[fd].size() > MAX_REQUEST_BYTES)
                    {
                        log_write(LOG_WARN, "request_too_large",
                                  "fd=" + to_string(fd) +
                                      " buffered=" + to_string(conn_bufs[fd].size()));
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        conn_bufs.erase(fd);
                        close(fd);
                        continue;
                    }

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
                    log_write(LOG_DEBUG, "conn", "event=close fd=" + to_string(fd));
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
