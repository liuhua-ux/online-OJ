#include "db.h"
#include "db_config.h"
#include <random>
#include <iostream>
using namespace std;

MYSQL *get_conn()
{
    thread_local MYSQL *conn = nullptr;

    if (conn != nullptr)
    {
        if (mysql_ping(conn) == 0) 
        {
            return conn;
        }
        mysql_close(conn); 
        conn = nullptr;
    }

    conn = mysql_init(nullptr);
    if (conn == nullptr)
    {
        return nullptr;
    }
    if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME,
                           DB_PORT, NULL, 0) == NULL)
    {
        mysql_close(conn);
        conn = nullptr;
        return nullptr;
    }
    return conn;
}

string escape_sql(const string &s)
{
    MYSQL *conn = get_conn();
    if (conn == nullptr)
    {
        return "";
    }

    string buf(s.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(
        conn, &buf[0], s.c_str(), s.size());
    buf.resize(n);
    return buf;
}
