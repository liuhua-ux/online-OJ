#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
    std::mutex log_mtx;     // 多个线程同时写，必须串行
    std::ofstream log_file; // 常驻打开，不要每条日志都开关文件
    LogLevel g_min_level = LOG_INFO;
    bool g_ready = false;

    const char *level_name(LogLevel lv)
    {
        switch (lv)
        {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARN:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        }
        return "?";
    }
}

LogLevel log_level_from_env()
{
    const char *v = getenv("LOG_LEVEL");
    if (v == nullptr)
    {
        return LOG_INFO;
    }
    if (strcmp(v, "DEBUG") == 0)
    {
        return LOG_DEBUG;
    }
    if (strcmp(v, "INFO") == 0)
    {
        return LOG_INFO;
    }
    if (strcmp(v, "WARN") == 0)
    {
        return LOG_WARN;
    }
    if (strcmp(v, "ERROR") == 0)
    {
        return LOG_ERROR;
    }
    return LOG_INFO;
}

bool log_init(const std::string &path, LogLevel min_level)
{
    size_t pos = path.rfind('/');
    if (pos != std::string::npos)
    {
        std::string dir = path.substr(0, pos);
        if (!dir.empty())
        {
            mkdir(dir.c_str(), 0755);
        }
    }
    log_file.open(path, std::ios::app);
    g_min_level = min_level;
    g_ready = log_file.is_open();
    return g_ready;
}

void log_write(LogLevel level, const std::string &event, const std::string &detail)
{
    if (level < g_min_level)
    {
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);

    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    char head[96];
    snprintf(head, sizeof(head), "%s.%03ld [%s] [tid=%ld] ",
             ts, (long)(tv.tv_usec / 1000),
             level_name(level), (long)syscall(SYS_gettid));

    std::string msg(head);
    msg += event;
    if (!detail.empty())
    {
        msg += ' ';
        msg += detail;
    }
    msg += '\n';

    std::lock_guard<std::mutex> lock(log_mtx);
    if (g_ready && log_file.is_open())
    {
        log_file << msg;
        log_file.flush();
    }
    else
    {
        std::fwrite(msg.data(), 1, msg.size(), stderr);
        std::fflush(stderr);
    }
}

std::string log_sanitize(const std::string &s)
{
    // TODO: '\n' → "\\n"，'\r' → "\\r"，'\t' → "\\t"，其余原样
    std::string out;
    out.reserve(s.size());
    for (auto c : s)
    {
        if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else if (c == '\t')
        {
            out += "\\t";
        }
        else
        {
            out += c;
        }
    }
    return out;
}
