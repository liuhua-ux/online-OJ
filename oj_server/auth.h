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
