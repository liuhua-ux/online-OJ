#include "auth.h"
#include "db.h"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <sstream>
#include <random>
#include <iostream>

using namespace std;

// 生成端用的参数，必须和校验端能认的格式一致
constexpr long SCRYPT_N = 32768;
constexpr long SCRYPT_R = 8;
constexpr long SCRYPT_P = 1;

vector<string> split(const string &s, char delim)
{
    vector<string> out;
    istringstream iss(s);
    string piece;
    while (getline(iss, piece, delim))
    {
        out.push_back(piece);
    }
    return out;
}

bool to_long(const string &s, long &out)
{
    size_t pos = 0;
    try
    {
        out = stol(s, &pos);
    }
    catch (...)
    {
        return false;
    }
    return pos == s.size();
}

bool parse_scrypt(const string &stored,
                  long &N, long &r, long &p,
                  string &salt, string &expected_hex)
{
    vector<string> parts = split(stored, '$');
    if (parts.size() != 3)
    {
        return false;
    }
    vector<string> seg = split(parts[0], ':');
    if (seg.size() != 4 || seg[0] != "scrypt")
    {
        return false;
    }

    salt = parts[1];
    expected_hex = parts[2];

    if (salt.empty() || expected_hex.empty())
    {
        return false;
    }

    if (expected_hex.size() % 2 != 0)
    {
        return false;
    }

    if (expected_hex.find_first_not_of("0123456789abcdef") != string::npos)
    {
        return false;
    }

    if (!to_long(seg[1], N) || !to_long(seg[2], r) || !to_long(seg[3], p))
    {
        return false;
    }

    return true;
}

bool scrypt_verify(const string &password, const string &stored)
{
    long N, r, p;
    string salt, expected_hex;
    if (!parse_scrypt(stored, N, r, p, salt, expected_hex))
    {
        return false;
    }

    vector<unsigned char> out(expected_hex.size() / 2);

    int rc = EVP_PBE_scrypt(password.data(), password.size(),
                            (const unsigned char *)salt.data(), salt.size(),
                            N, r, p,
                            132L * N * r * p,
                            out.data(), out.size());
    if (rc != 1)
    {
        return false;
    }

    string got;
    got.reserve(out.size() * 2);
    for (unsigned char c : out)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        got += buf;
    }

    return CRYPTO_memcmp(got.data(), expected_hex.data(), got.size()) == 0;
}

string make_session_id()
{
    static const char *hex = "0123456789abcdef";
    thread_local random_device rd;

    string id;
    for (int i = 0; i < 32; i++)
    {
        id += hex[rd() % 16];
    }
    return id;
}

string create_session(const string &username)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return "";
    }

    string id = make_session_id();
    string sql = "INSERT INTO sessions (session_id, username, created_at, expires_at) "
                 "VALUES ('" +
                 id + "', '" + escape_sql(username) + "', NOW(), "
                                                      "NOW() + INTERVAL " +
                 to_string(SESSION_TTL_SECONDS) + " SECOND)";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        cerr << "create_session failed: " << mysql_error(conn) << endl;
        return "";
    }
    return id;
}

string get_session_user(const string &session_id)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return "";
    }

    string sql = "SELECT username FROM sessions "
                 "WHERE session_id = '" +
                 escape_sql(session_id) + "' "
                                          "AND expires_at > NOW()";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        cerr << "get_session_user failed: " << mysql_error(conn) << endl;
        return "";
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (res == nullptr)
    {
        return "";
    }

    string user;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row != nullptr && row[0] != nullptr)
    {
        user = row[0];
    }

    mysql_free_result(res);
    return user;
}

bool destroy_session(const string &session_id)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return false;
    }

    string sql = "DELETE FROM sessions WHERE session_id = '" +
                 escape_sql(session_id) + "'";

    if (mysql_query(conn, sql.c_str()) != 0)
    {
        cerr << "destroy_session failed: " << mysql_error(conn) << endl;
        return false;
    }
    return true;
}

string cookie_value(const string &cookie_header, const string &name)
{
    for (const auto &part : split(cookie_header, ';'))
    {
        string p = part;

        // 分隔符后面通常跟一个空格，要去掉
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        {
            p.erase(0, 1);
        }

        size_t eq = p.find('=');
        if (eq == string::npos)
        {
            continue;
        }

        if (p.substr(0, eq) == name)
        {
            return p.substr(eq + 1);
        }
    }
    return "";
}

string make_password_hash(const string &password)
{
    // werkzeug 的盐字符集
    static const char *chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    thread_local random_device rd;

    string salt;
    for (int i = 0; i < 16; i++)
    {
        salt += chars[rd() % 62];
    }

    vector<unsigned char> out(64);
    int rc = EVP_PBE_scrypt(password.data(), password.size(),
                            (const unsigned char *)salt.data(), salt.size(),
                            SCRYPT_N, SCRYPT_R, SCRYPT_P,
                            132L * SCRYPT_N * SCRYPT_R * SCRYPT_P,
                            out.data(), out.size());
    if (rc != 1)
    {
        return "";
    }

    string hex;
    hex.reserve(128);
    for (unsigned char c : out)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        hex += buf;
    }

    return "scrypt:" + to_string(SCRYPT_N) + ":" + to_string(SCRYPT_R) +
           ":" + to_string(SCRYPT_P) + "$" + salt + "$" + hex;
}

string trim(const string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos)
    {
        return "";
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}