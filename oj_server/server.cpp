#include "auth.h"
#include "db.h"

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
#include <sys/wait.h>
#include <random>
#include <algorithm>
#include <filesystem>

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
};

queue<Task> task_queue;
mutex mtx;
condition_variable cv;

queue<JudgeTask> judge_queue;
mutex judge_mtx;
condition_variable judge_cv;

const char *JUDGE_BIN = "/home/ubuntu/online-OJ/judge_demo/judge";
const string PROBLEM_PREFIX = "/problem/";
const string SUBMIT_PREFIX = "/submit/";
const string RESULT_PREFIX = "/result/";

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
    else if (code == 403)
    {
        reason = "Forbidden";
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
                  to_string(body.size()) + "\r\n" +
                  extra_headers +
                  "Connection: close\r\n"
                  "\r\n" +
                  body;

    write(conn_fd, resp.c_str(), resp.size());
}

void send_redirect(int conn_fd, const string &location, const string &extra_headers = "")
{
    string resp = "HTTP/1.1 302 Found\r\n"
                  "Location: " +
                  location + "\r\n" +
                  extra_headers +
                  "Content-Length: 0\r\n"
                  "Connection: close\r\n"
                  "\r\n";
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

string nav_right()
{
    if (nav_user.empty())
    {
        return "    <a href=\"/login\">登录</a>\n"
               "    <a href=\"/register\">注册</a>\n";
    }
    return "    <a href=\"/profile\">" + html_escape(nav_user) + "</a>\n"
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

    page << render_submit_form(problem_id);
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

bool render_result(const string &job_id, string &html)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "SELECT status, verdict, passed, total, time_ms, "
                 "memory_kb, result, problem_id, created_at "
                 "FROM submissions WHERE job_id = '" +
                 job_id + "'";

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

    string status = cell(row[0]);
    string verdict = cell(row[1]);
    string passed = num_or_dash(row[2]);
    string total = num_or_dash(row[3]);
    string time_ms = num_or_dash(row[4]);
    string memory = num_or_dash(row[5]);
    string raw = cell(row[6]);
    string prob_id = cell(row[7]);
    string created = cell(row[8]);

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

void handle_request(int conn_fd, const string &req);

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
        cerr << "set_status failed: " << mysql_error(conn) << endl;
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
        cerr << "finish_job failed: " << mysql_error(conn) << endl;
        return false;
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
        cerr << "count_testcases failed: " << mysql_error(conn) << endl;
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
            cerr << "cleanup failed: " << ec.message() << endl;
        }

        cout << "判题完成 job_id=" << t.job_id
             << " started=" << started
             << " out=" << out.size() << " 字节\n"
             << out << endl;

        cout << "解析 job_id=" << t.job_id
             << " verdict=" << s.verdict
             << " passed=" << s.passed
             << " time=" << s.time_ms << "ms"
             << " mem=" << s.memory_kb << "KB"
             << endl;
    }
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
        cout << "[" << line << "]   长度 = " << line.size() << endl;
    }

    for (const auto &kv : headers)
    {
        cout << kv.first << "=" << kv.second << "\n";
    }

    string session_id;
    if (headers.count("cookie"))
    {
        session_id = cookie_value(headers["cookie"], "session");
    }
    cout << "session  = [" << session_id << "]" << endl;

    string current_user;
    if (!session_id.empty())
    {
        current_user = get_session_user(session_id);
    }
    cout << "user     = [" << current_user << "]" << endl;

    nav_user = current_user;

    size_t body_need = 0;
    if (headers.count("content-length"))
    {
        length = headers["content-length"];
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

    // 首页
    if (target == "/")
    {
        ifstream f("www/index.html");
        stringstream ss;
        ss << f.rdbuf();
        string home = ss.str();
        send_response(conn_fd, 200, home, "text/html; charset=utf-8");
    }
    // 回显页
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
    // 题目详情
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
    // 提交代码
    else if (method == "POST" && target.compare(0, SUBMIT_PREFIX.size(), SUBMIT_PREFIX) == 0)
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

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
                     "(job_id, username, problem_id, status, result, created_at) "
                     "VALUES ('" +
                     job_id + "', '" + escape_sql(current_user) + "', " +
                     to_string(problem_id) +
                     ", '排队中', NULL, NOW())";

        if (mysql_query(conn, sql.c_str()) != 0)
        {
            cerr << "insert failed: " << mysql_error(conn) << endl;
            send_response(conn_fd, 500, "提交记录保存失败\n");
            return;
        }

        {
            lock_guard<mutex> lock(judge_mtx);
            judge_queue.push({job_id, work_dir, problem_id, tl_ms, ml_mb});
        }
        judge_cv.notify_one();

        cout << "入队 job_id=" << job_id << " problem=" << problem_id << endl;
        send_redirect(conn_fd, "/result/" + job_id);
    }
    // 提交结果
    else if (target.compare(0, RESULT_PREFIX.size(), RESULT_PREFIX) == 0)
    {
        if (current_user.empty())
        {
            send_redirect(conn_fd, "/login");
            return;
        }

        string id = target.substr(RESULT_PREFIX.size());

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
    else if (target == "/register")
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
                    cerr << "register failed: " << mysql_error(conn) << endl;
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
    else if (target == "/login")
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
                cerr << "login query failed: " << mysql_error(conn) << endl;
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
    else if (target == "/logout")
    {
        if (!session_id.empty())
        {
            destroy_session(session_id);
        }

        string cookie = "Set-Cookie: session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n";
        send_redirect(conn_fd, "/", cookie);
    }
    // 个人中心
    else if (target == "/profile")
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
                cerr << "change password failed: " << mysql_error(conn) << endl;
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
    else if (target == "/problems")
    {
        send_response(conn_fd, 200, render_problems(), "text/html; charset=utf-8");
    }
    // 当前登录状态（调试用）
    else if (target == "/whoami")
    {
        send_response(conn_fd, 200,
                      current_user.empty() ? "未登录\n" : current_user + "\n");
    }
    // 其余按静态文件处理
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

    thread t1(worker), t2(worker), t3(worker), t4(worker);
    thread j1(judge_worker), j2(judge_worker);

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
