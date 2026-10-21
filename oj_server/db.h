  #pragma once

  #include <string>
  #include <mysql/mysql.h>

  // 每线程一条连接，第一次调用时建
  MYSQL *get_conn();

  // 往 SQL 里拼之前先转义
  std::string escape_sql(const std::string &s);
