#pragma once

#include <string>
#include <vector>

// 按分隔符切字符串
std::vector<std::string> split(const std::string &s, char delim);

// 字符串转 long，整串都是数字才算成功
bool to_long(const std::string &s, long &out);

// 拆开 "scrypt:n:r:p$salt$hex"，格式不对返回 false
bool parse_scrypt(const std::string &stored,
                  long &N, long &r, long &p,
                  std::string &salt, std::string &expected_hex);

// 用库里的哈希校验密码
bool scrypt_verify(const std::string &password, const std::string &stored);

// 会话有效期 24 小时，和 cookie 的 Max-Age 对齐
constexpr int SESSION_TTL_SECONDS = 86400;

// 32 位小写十六进制
std::string make_session_id();

// 登录成功后发票，返回票号；失败返回空串
std::string create_session(const std::string &username);

// 票号 → 用户名；无效或过期返回空串
std::string get_session_user(const std::string &session_id);

// 注销，把票从库里删掉
bool destroy_session(const std::string &session_id);

// 从 cookie 头里取某个键的值，没有返回空串
std::string cookie_value(const std::string &cookie_header, const std::string &name); // 从 cookie 头里取某个键的值，没有返回空串