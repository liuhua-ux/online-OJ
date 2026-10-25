// 日志模块自测（FIX-07 验收用，不进主程序）
// 编译：g++ -Wall -I. -o /tmp/logtest log_selftest.cpp log.cpp
// 运行：cd oj_server && LOG_LEVEL=INFO /tmp/logtest
#include "log.h"

#include <iostream>
#include <string>
#include <thread>

int main()
{
    if (!log_init("logs/selftest.log", log_level_from_env()))
    {
        std::cerr << "log_init failed\n";
        return 1;
    }

    log_write(LOG_DEBUG, "debug_line", "LOG_LEVEL=INFO 时这行不该出现");
    log_write(LOG_INFO, "startup", "port=8081 workers=4");
    log_write(LOG_WARN, "login_fail", "user=bob");
    log_write(LOG_ERROR, "judge_error", "job=deadbeef code=127");

    // 并发：10 个线程各写 100 行，检查是否丢行 / 内容交错
    std::thread ts[10];
    for (int i = 0; i < 10; i++)
    {
        ts[i] = std::thread([i]() {
            for (int j = 0; j < 100; j++)
            {
                log_write(LOG_INFO, "concurrent",
                          "t=" + std::to_string(i) + " n=" + std::to_string(j));
            }
        });
    }
    for (auto &t : ts)
    {
        t.join();
    }

    // 日志注入：一个带换行的用户名，未转义会伪造出第二行
    log_write(LOG_WARN, "inject_sanitized",
              "user=" + log_sanitize("evil\n[ERROR] forged line"));
    log_write(LOG_WARN, "inject_raw", "user=evil\n[ERROR] forged line");
    return 0;
}
