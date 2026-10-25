#pragma once

#include <string>

// 日志级别：数字越大越严重，低于 min_level 的直接丢弃。
enum LogLevel
{
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
};

// 初始化：建目录、打开日志文件、设定最低级别。
// 返回 false = 文件打不开；此时 log_write 应降级写到 stderr，程序仍能跑。
bool log_init(const std::string &path, LogLevel min_level);

// 读环境变量 LOG_LEVEL（DEBUG / INFO / WARN / ERROR），认不出就用 INFO。
LogLevel log_level_from_env();

// 写一行日志，格式：
//   2026-10-25 12:34:56.789 [INFO] [tid=12345] event detail
void log_write(LogLevel level, const std::string &event, const std::string &detail = "");

// 把用户可控内容里的 \n \r \t 转义成字面量，防止一条日志被伪造出第二行。
// 用户名、URL、路径、判题器输出，进日志前都要先过它。
std::string log_sanitize(const std::string &s);
