# 二期服务端笔记（C++ 自研 HTTP 服务）

> 记录核心设计决策与实测结论，可直接用作实验报告素材。
> 对应需求文档 §7 的 `CH-11`：二期 = **socket + epoll + 线程池**，
> **功能需求不变、仅替换实现**，对浏览器端透明。

## 当前能力

```
cd ~/online-OJ/oj_server
g++ -Wall -o server server.cpp auth.cpp db.cpp $(mysql_config --cflags --libs) -lcrypto -lhiredis
./server                      # 监听 8080
```

**必须站在 `oj_server/` 里启动** —— 代码里所有路径（`www/index.html`）都是相对路径，
换一个目录跑就全 404 了（见决策 11）。

路由表：

| 请求 | 行为 |
| --- | --- |
| `/` | 读 `www/index.html`，`text/html` |
| `GET /echo` | 读 `www/echo.html`（表单页），`text/html` |
| `POST /echo` | **现场拼**一页 HTML，把用户填的字塞进去 → 🔒 见决策 18–19 |
| `GET /problems` | 从 MySQL 读已发布题目，列表页 |
| `GET /problem/<id>` | 题目详情（含代码提交表单） |
| `POST /submit/<id>` | **需登录** → 建临时目录、写文件、入库（带 `username`）、入判题队列 |
| `GET /result/<job_id>` | **需登录 + 归属校验** → 本人或 admin 可看；含**提交的源码** |
| `GET /submissions` | **需登录**：学生只看本人，admin 看全部、可 `?user=` 筛选，倒序 `LIMIT 100` |
| `GET /rank` | **需登录**：按通过题数降序；Redis 缓存 60s，缓存故障降级查库 |
| `GET/POST /login` | 表单 / 校验 `scrypt` 后发会话 cookie |
| `GET /logout` | 删会话 + 清 cookie |
| `GET/POST /register` | 注册（重名靠唯一键 1062 识别） |
| `GET/POST /profile` | 个人中心 / 改密（验旧密码、踢其它会话） |
| `GET /whoami` | 调试用：返回当前用户名或「未登录」 |
| `/<任意路径>` | **先 URL 解码**（决策 23）→ 读 `www/<该路径>`，按后缀推 `Content-Type`，读不到 → 404 |
| 路径逃出 `www/` 之外 | **404**（🔒 见决策 13–17） |

实测（`curl --path-as-is`，2026-10-11）：

```
/               200  250 bytes  CT=text/html; charset=utf-8
/about.html     200  220 bytes  CT=text/html; charset=utf-8
/hello.html     200  228 bytes  CT=text/html; charset=utf-8
/style.css      200  259 bytes  CT=text/css; charset=utf-8
/nosuchpage     404   25 bytes  CT=text/plain; charset=utf-8
```

a-2 加完之后（2026-10-12）：

```
GET  /                200  290 bytes   ← 首页加了「回显测试」链接（250 → 290）
GET  /echo            200  387 bytes   ← www/echo.html，表单页
POST /echo  name=rikka&msg=hello world   →  名字：rikka / 留言：hello world
POST /echo  name=<b>Y</b>                →  名字：&lt;b&gt;Y&lt;/b&gt;        🔒 决策 18–19
POST /echo  name=<script>alert(1)</script> → 浏览器不弹框，原样显示这行字
```

a-3 加了 URL 解码之后（2026-10-12）：

```
/hello%2Ehtml                 404 25  →  200 228       ← 解码生效
/%2e%2e%2fserver.cpp          404 25  →  404 25        ← 数字没变，但拦住它的换成了白名单
/%2e%2e%2f%2e%2e%2fweb/...    404 25  →  404 25
```

```
/../server.cpp              404   25 bytes      ← 改之前是 200 + 5995 字节源码
/..                         404   25 bytes      ← 改之前是 200
/../../web/db_config.py     404   25 bytes      ← 改之前是 200 + 201 字节数据库密码
/../../../../../etc/passwd  404   25 bytes      ← 改之前是 200 + passwd 内容
```

那 25 字节就是「请输入正确的网址」，**一个字节的文件内容都没漏出去**。

## 整体结构

```
main
 ├── socket → setsockopt(SO_REUSEADDR) → bind → listen      建好门
 ├── fcntl(listen_fd, O_NONBLOCK)                           那扇门不再把门卫堵住
 ├── epoll_create1 + epoll_ctl(ADD, listen_fd)              雇门卫（决策 26）
 ├── thread t1..t4(worker)                                  ⭐ 雇 4 个厨师（决策 27）
 └── while (true)                                           主线程 = 门卫：领名单，挨个处理
      ├── epoll_wait(..., -1)                               没动静就睡，不空转
      ├── fd == listen_fd → accept 循环                      新客人：每条连接 O_NONBLOCK + epoll_ctl(ADD)
      └── 其他 fd（可读）→ read 一次，append 进 conn_bufs[fd]
           ├── request_total_len 攒够一个完整请求了？
           │    ├── 够 → 抠出 req → 摘牌(EPOLL_CTL_DEL) → 撕页(erase)
           │    │        → push{fd, req} + notify_one        ⭐ 挂上单子就走，不等结果
           │    └── 不够 → 留着，等下一轮
           └── cnt == 0 → 摘牌 + 撕页 + close                对方正常关闭

worker()  ← 4 个线程各跑一份                                 厨师：谁空谁取
 ├── 锁内：cv.wait 等到队列不空 → 取一个任务 → 出花括号解锁
 └── 锁外：handle_request(fd, req) + close(fd)              ⭐ 真活不占锁
```

**主线程和工人是两条线，各管各的，别混**：

- **主线程**只干两件事：招呼 epoll（等事件、接连接、收字节）、把攒够的请求**打包丢进队列**
- **工人线程**只干一件事：从队列取任务、跑 `handle_request` + `close`

为什么非拆不可 —— **epoll 治「等」、线程池治「干」**（决策 26 / 27）。
原来那版是「主线程又接客又做菜」，做菜的那 2 秒里没人接客。

## 关键设计决策

### 1. 一个请求的完整生命周期

```
socket → setsockopt(SO_REUSEADDR) → bind → listen → accept → read → write → close
```

- **`SO_REUSEADDR`**：让服务器重启时能立刻重新绑上 8080，不用等内核把旧连接收干净
- **每个连接必须 `close`**。漏掉的代价是可测的：**15 个请求 fd 从 4 涨到 19**；
  补上 `close(conn_fd)` 之后，**65 个请求 fd 仍是 4**

### 2. `send_response`：把重复收成一个函数

```cpp
void send_response(int conn_fd, int code, const string &body,
                   const string &content_type = "text/plain; charset=utf-8")
```

四条分支原来各拼一份响应头，现在收成一份。**默认参数让另外三条调用点一个字不用改。**

- **默认参数的位置规则**：一旦某个参数有默认值，**它后面的全都要有**。
  把带默认值的 `content_type` 放到 `body` 前面 → `error: default argument missing for parameter 4`
- **摆放的道理**：「必须给的」在前，「可以省的」在后 —— 是接口设计通例，不只是语法要求

**要不要写个「响应类」？** 判据：**这个东西要不要「活过这一次调用」？**

- 进来 → 算完 → 扔掉，中间不留记忆 → **函数**
- 有状态要一直带着（先造对象 → 改几个字段 → 再发）→ **类**

`send_response` 一件记忆都没有 → 函数。类留给「一个 HTTP 请求对象」「线程池的任务对象」那两步。

### 3. 请求行解析

```cpp
istringstream iss(req);
iss >> method >> target >> version;
```

`>>` 天然按空白切分，而 `\r` 对 `operator>>` **算空白**（跟空格一个待遇）——
所以 `target` 出来是干净的，**尾巴上没有挂 `\r`**。

（`getline` 就不同了，见下条。）

### 4. 头部解析：`getline` 会把 `\r` 留给你

HTTP 的行尾是 `\r\n` **两个**字符，而 `getline` 只认 `\n` 一个。所以
`Host: 127.0.0.1:8080` 读出来的长度是 **21 不是 20**。

- 逐行 `getline` → 手动 `pop_back()` 掉行尾 `\r`
- 冒号切分：`find(':')` → `substr(0, pos)` / `substr(pos + 1)`
- **冒号后的空格只是"建议"有**（`Content-Length:29` 就没有），要洗一下
- **「请求行的残渣」和「真正的空行」长得一模一样**（strip 完都是空串）——
  分不出来就别分：**循环前先 `getline` 一次把残渣吃掉**

### 5. `map::operator[]` 在"读"的时候会凭空造 key（咬过两次）

查头部时写 `headers["Content-Length"]`，**map 从 2 条变成 3 条** —— 它把那个 key 建出来了。

- **判据：你要写还是要查。** 写 → `[]`；查 → `count()` / `find()`
- 同一个 `operator[]`，在「推 Content-Type」那里又咬了一次
- 类比：**map 是个储物柜，`headers[k]` 的口气是「把那个格子给我」，不是「告诉我里面有什么」**

### 6. 半包：`read()` 不保证给你一个"请求"

```cpp
ssize_t n = read(conn_fd, buf, sizeof(buf));
```

**`read(fd, buf, N)` 的语义是「把此刻内核缓冲区里已经有的字节抄给我，有多少抄多少」**，
**不是**「读一个请求」。TCP 是**字节流**不是消息流，「一个请求」是**应用层（HTTP）**约定的形状。

所以只能自己循环攒。复现脚本（把请求拆成 4 段、每段隔 1 秒发）：

```
half —— 请求拆 4 段发     → 服务器只看到第一行就回响应 + close → 客户端 BrokenPipeError
two  —— 两个完整请求一次性 sendall → 客户端只收到 1 个响应，第二个请求静默消失（不报错）
body —— 头先到、body 分两次到     → 同样 BrokenPipeError
```

⚠️ `string(buf)` 走的是 **C 字符串**那条路（只看 `\0`）：**14 字节的请求被读成 100 字节**。
要用 `req.append(buf, n)`。

### 7. 请求真正的长度 = `head_end + 4 + body_need`

- `\r\n\r\n` 只标记**头部结束**，**不标记请求结束**
- 头部字节数 + `Content-Length` 声明的 body 字节数 = 请求总长
- 实测一例：头部 **67** 字节 + body **20** 字节 → **87**
- 收到的比 `total_len` 多 → **粘包**（属于下一个请求，本步不处理，只报告）

解析必须写在 `if (n > 0)` **里面** —— 否则 `read` 返回 0/-1 时会去解析**未初始化的 `buf`**。

### 8. 路由：`else if` 列已知，`else` 兜未知

最终形态是一条 `if / else if / else` 链 + **链外一次 `write`、一次 `close`**。

- 一开始写成**两颗平级 `if` + 一个 `else`** → `else` 只挂**第二个** `if` → 第一个命中后还会继续往下判。
  **被分支里的 `close` 挡住了**（第二次 `write` 返回 -1），客户端只看见一个 200 ——
  **「对的」是运气，不是逻辑**。（删掉 `close` 实测：收到 2 个响应，90 + 111 字节）
- 加 `301`/`500` 分支时**把最后的 `else` 弄没了** → `reason` 是空串 → `HTTP/1.1 403 `（空短语）。
  **`-Wall` 同样 0 warning。**
- ⚠️ **`find('about')` 用单引号** = **多字符字面量**，编译器算成 `1651471732`（`0x626F7574`），
  截成 char = **116 = `'t'`** → 他实际写的是「路径里有没有字母 `t`」。`/tea`、`/txyz` 全返回 about 页。
  **程序照跑。**

**兜底用 `else` 不用 `else if`** —— 列已知是一根根栏杆，兜未知是一张安全网。

### 9. `Content-Type` 决定浏览器「渲染」还是「显示源码」

浏览器拿到的只有「一串字节 + 响应头」，**字节里没有任何「我是网页」的标记**，
`Content-Type` 是它唯一的线索。

> **一份数据 + 一个类型标签 = 完全不同的行为**（文件后缀、`#!` shebang 都是这个家族）

🔒 **MIME sniffing**：服务器**不写** `Content-Type` 时浏览器会**猜** ——
内容是 HTML 的「图片」会被嗅探成 HTML 渲染 → XSS。防御是 `X-Content-Type-Options: nosniff`。

**「让浏览器猜」本身就是个洞。**

### 10. 静态文件服务的读法：`rdbuf()` 是原样读

```cpp
ifstream f("www/index.html");
stringstream ss;
ss << f.rdbuf();
string home = ss.str();
```

- **`getline` 是「带加工」的读法**：语义是「读到 `\n` 为止、**把 `\n` 扔掉**」
  （判题器里 `a += line + "\n"` 就是在手工补回 getline 丢掉的东西）
- 要**原样**就换 `rdbuf()`
- ⚠️ **文件不存在时 `f.rdbuf()` 不报错**：给空串、`fail()=1` → 会 `send_response(200, "")` **白屏**

**源文件里的 HTML 字面量 vs 搬进文件后**（实测，可写报告）：

| | 字节数 |
| --- | --- |
| 源码里的字面量（12 空格 × 11 行缩进、无末尾换行） | **334** |
| 搬成文件（去缩进 + 末尾加一个换行） | **203** |
| 差 **131** | = 132 个缩进空格 − 1 个换行 |

**铁证**：改 `www/index.html` 一个字 → F5 立刻变，**不碰代码、不重编译**。

### 11. 路径 → 磁盘的通用映射：`/xxx` → `www/xxx`

**不要**拼成 `www/xxx.html`。规则写成后者等于**只认 HTML**，`style.css` / `logo.png` 全没戏。

- 三条分支塌成一条规则（`if / else if / else` → `if / else`）
- 验收时**没写一行 C++ 就能访问 `hello.html`** —— 这才证明规则通用，不是换了种写法又写死两条

⚠️ **`www` 是相对路径** → 服务器必须站在 `oj_server/` 里启动。
这跟决策 13–17 的目录穿越是同一件事的两面。

### 12. 后缀 → `Content-Type`：查表 + 兜底

```cpp
map<string, string> types;          // 建在 while(true) 外面，别每个请求重建一次
types["html"] = "text/html; charset=utf-8";
types["css"]  = "text/css; charset=utf-8";
types["js"]   = "application/javascript; charset=utf-8";
types["png"]  = "image/png";
types[""]     = "application/octet-stream";
```

抠后缀的三种偏移（实测对照）：

```
road = "www/style.css"
           pos 指向 '.'
   pos - 1  →  "e.cs"     ✗
   pos      →  ".css"     ✗（表的键是 "css"，带点的查不到）
   pos + 1  →  "css"      ✓
```

🔴 **「没后缀」和「后缀没见过」是同一件事** —— 两类的正确出口**是同一个兜底**
（`application/octet-stream` = 「我不知道这是什么，你自己看着办」）。

⚠️ **别把兜底塞进 `ext` 变量**（拿「表里的值」当「查表的键」），那会导致
同一句话写两遍，将来改兜底要改两处。

⚠️ `ext` 没初始化**不会**有问题：`string ext;` ≡ `string ext = "";`（默认构造）。
所以「没后缀」根本不用写分支。（对比：`int pos;` 是**垃圾值**，装 `npos` 会截成 `-1`
→ `substr(-1, ...)` 抛 `std::out_of_range`，**整个服务器当场死**。）

### 13. 🔒 目录穿越 —— 先复现

`string road = "www" + target;` 是**字符串拼接**。`..` 对内核是**路径语义** ——
拼接的时候它只是两个点，交给 `open()` 的时候它变成「上一层目录」。

（`..` **不是两个点号的字符**，是**每个目录自带的通向上一层的入口**：
`ls -a` 打出来的 `.` 和 `..` 是真实条目；`ls www/..` 和 `ls`（`oj_server/`）输出完全一样。）

**复现（改之前）**：

```
curl              'http://127.0.0.1:8080/../../web/db_config.py'   → 404
curl --path-as-is 'http://127.0.0.1:8080/../../web/db_config.py'   → 200  201 bytes
curl --path-as-is 'http://127.0.0.1:8080/../../../../../etc/passwd' → 200  passwd 内容
curl --path-as-is 'http://127.0.0.1:8080/../server.cpp'             → 200  5995 bytes 服务器自己的源码
```

**能读到什么**（每条都有证据）：

| 目标 | 证据 |
| --- | --- |
| `web/db_config.py`（数据库密码 + Flask `SECRET_KEY`） | **实测 HTTP 200 / 201 字节** |
| `/etc/passwd` | **实测 HTTP 200**，`root:x:0:0:...` 原样输出 |
| `oj_server/server.cpp`（服务器自己的源码） | **实测 HTTP 200 / 5995 字节** |
| `~/.ssh/id_ed25519`（**登录服务器的私钥**） | **411 字节**；`wc -c www/../../../.ssh/id_ed25519` 与 `~/.ssh/id_ed25519` 的 sha256 **完全相同**（`465b7f1a…`）|

最后一条的验法说明：修完之后 HTTP 一律 404，没法再用 HTTP 验「改之前能不能读」，
所以改用**等价方式** —— 直接验旧代码那行 `ifstream f("www" + target)` 会不会打开它。
旧代码没有任何检查，**`ifstream` 打得开就原样发出去**。

`SECRET_KEY` 被拿到 = **防伪章被偷** → 可以自己签一张 `{"username":"testuser"}` 的
session cookie → **不用密码以管理员身份登进 OJ**。

而且这一切：**不用登录、不用密码、不用有任何账号，一个 GET 请求。**

### 14. 🔒 「折叠」：客户端提前替你走了一遍路径表

带不带 `--path-as-is` 的差别，看**服务器日志里的原始请求行**：

```
curl              →  GET /web/db_config.py HTTP/1.1        ← 两个点没了
curl --path-as-is →  GET /../../web/db_config.py HTTP/1.1  ← 原样送到
```

**`..` 在离开你的电脑之前就被吃掉了** —— 客户端（curl / 浏览器）按 URL 规范
把 `.` 和 `..` 折叠掉：`/..` 一路往上顶到根就停住（根没有上一层）。

用 `urllib.parse.urljoin` 验证同一份 URL 规范：

```
/../server.cpp            →  浏览器真正请求的是  /server.cpp
/../../web/db_config.py   →  浏览器真正请求的是  /web/db_config.py
```

🔴 **两边走的是同一张 `..` 表，起点却不一样**：
**客户端从「网站的根」出发**（`/..` 顶不上去，停住），**服务器从 `oj_server/` 出发**
（`www/../../` 真的拐出去两层）。

> **会折叠的只有客户端。** 攻击者不等你的浏览器，他直接往 socket 里写字：
> `GET /../server.cpp HTTP/1.1`。
>
> **客户端替你做的一切（折叠、校验、`type="password"`、前端表单验证……）统统不是安全边界。**

### 15. 🔒 黑名单 vs 白名单

第一个想到的方案是 `target.find("/../")`。它能过那三条验收，**但它是一张「我想到的坏写法」的清单**：

```
curl --path-as-is 'http://127.0.0.1:8080/..'    →  200（漏掉了！）
```

`target = /..` 里**没有** `/../` 这个子串（结尾没有斜杠）→ 规则放行。

> **黑名单 = 「我想到的坏写法」的清单**，你得为每一种可能的写法负责，**漏一个是常态**。
> **白名单 = 「只允许一种结果」**，你想不到的写法全都无所谓，因为不管怎么写都得过最后这一关。

这就是决策 8 那句的两面：**`else if` 列已知是一根根栏杆，`else` 兜未知是一张安全网。**

⚠️ **2026-10-12（a-3）加了 URL 解码之后又实测了一次 —— 黑名单**多了第二个洞**，这次是「顺序」。见决策 24。**

### 16. 🔒 修法：`realpath` 规范化 + `www` 前缀校验

```cpp
char root_buf[PATH_MAX];                                  // PATH_MAX = 4096, <climits>
char full_buf[PATH_MAX];
char *root_p = realpath("www", root_buf);                 // <cstdlib>
char *full_p = realpath(road.c_str(), full_buf);

string root, real;
if (root_p != NULL) root = root_buf;
if (full_p != NULL) real = full_buf;

bool ok = !root.empty() && real.compare(0, root.size() + 1, root + "/") == 0;
```

**问的问题从「路径里有没有坏东西」换成「算完之后它落在哪」。**

`realpath` 把 `.`、`..`、**符号链接**全部走完，直接给出「最终落在哪」的绝对路径 ——
**不用自己数 `..`**。

**为什么传指针**：`char *realpath(const char *path, char *resolved_path);`

| | 是什么 |
| --- | --- |
| 参数 1 | 要算的路径（只读，`const char *`） |
| 参数 2 | **调用方准备的缓冲区**，函数把结果写进去（可写，`char *`） |
| 返回值 | 成功 → 指向参数 2 那块内存；**失败 → `NULL`** |

- **C 里没有 `std::string`**，"一串字符" = 一块内存 + 指向它开头的指针
- **C 的老规矩：调用方出缓冲区。** 函数自己 malloc 的话，`free` 归谁说不清
  （`realpath` 传 `NULL` 当第二参数的后门用法，就是为这个麻烦准备的 —— 但那时**必须自己 free**）
- `c_str()` 返回的指针**指向 string 内部那块内存，不是副本**，而且是 `const`
  → **不能改、不能存起来，只能当场用掉**

⚠️ **`char *` 转 `string` 之前必须先判 `NULL`** —— `string(NULL)` 是未定义行为。

⚠️⚠️ **返回值必须检查，因为「失败」和「成功」从缓冲区本身分不出来。**
标准说失败时 `resolved_path` 的内容是**未定义**的；glibc 实际会写一个
**格式完美、看着就像正确答案**的半成品。实测：

```
realpath("www/nosuchfile", full_buf) 的返回值 = NULL（失败了）
full_buf 里装的是     = /home/ubuntu/online-OJ/oj_server/www/nosuchfile
前缀检查放行吗？      = 放行！（它看着就在 www 里面）
```

**一个不存在的文件，前缀检查会放行。** 不查返回值 → 走「正常」流程 → `ifstream`
打不开 → 读空串 → **`200` 白屏**（正是决策 10 埋的那个坑）。

> **`full_buf` 是「东西放哪」，返回值是「办成了没有」。** 两件事，缺一不可。
> C 里没有异常，函数办不成事只能靠返回值通知你 —— 形状跟 `read()` 返回 `-1/0/n` 完全一样。

**顺带的好处：代码变短了。** `realpath` 返回 `NULL` 时已经覆盖「文件不存在」，
所以原来 `if (f.is_open())` 的那个 `else` 不需要了。
**好的修复会让代码变短，不是变长**（黑名单是在加栏杆，这个是直接把整条路堵死）。

### 17. 🔒 前缀检查末尾那个 `/` 不能省

```cpp
real.compare(0, root.size() + 1, root + "/") == 0
//     ↑        ↑                 ↑
//   从我第 0 位起  取 root.size()+1 个字符   跟 root + "/" 比，一样吗
```

实测（`root = /home/ubuntu/online-OJ/oj_server/www`）：

| 候选路径 | 只比 `root` | 比 `root + "/"` |
| --- | --- | --- |
| `.../oj_server/www/index.html` | 放行 ✓ | 放行 ✓ |
| `.../oj_server/www2/x` | **放行 ✗ 误判** | 拦下 ✓ |

🔴 **字符串比较只认字符，它不知道 `/` 是「目录的边界」。**
`www` 和 `www2` 是**两个平级目录**（兄弟）不是父子，但 `www2` 的前三个字符就是 `www`
—— 从字符的角度它**确实**"以 `www` 开头"。

加那个 `/` 才是在**划边界线**：`root` 问的是「你开头是不是 `.../www`」，
`root + "/"` 问的是「你开头是不是 `.../www/`」。

> **前缀检查永远要带上那个分隔符。** 少了它，「前缀相等」和「在里面」就是两回事。

（同门牌号：按"门牌以 3 开头"筛，30 号、31 号全进来了；筛"3 号-"才对。）

### 18. 🔒 XSS：换了个「读你字的家伙」

- `/echo` 把用户填的字拼进 **HTML** → **浏览器**读它 → **XSS**（药是**转义**）
- 一期 `/login` 把用户填的字拼进 **SQL** → **MySQL** 读它 → **SQL 注入**（药是**参数化**，更强 —— 根本不让数据进入语句文本）

**同一个病，换了个解释器。**

**先复现（改之前）**：

```
POST name=<b>Y</b>                       → 浏览器里显示【粗体的 Y】      ← <b> 被当指令执行了
POST name=<script>alert(1)</script>      → 弹出一个写着 1 的框          ← 铁证
```

**修完（`html_escape`）**：

```
POST name=<b>Y</b>                       → 名字：&lt;b&gt;Y&lt;/b&gt;
POST name=<script>alert(1)</script>      → 页面上原样显示这行字，不弹框
```

映射：`&`→`&amp;` / `<`→`&lt;` / `>`→`&gt;` / `"`→`&quot;`

**反射型 vs 存储型**：`/echo` 这页的字只在**这一次响应**里 → **反射型**（只有自己中招）；
一期那个注册用户名是**存进数据库**、谁打开 `/whoami` 谁中招 → **存储型**（狠得多）。

### 19. 🔒 转义的是「用户的值」，不是「整页」

`send_response(..., html_escape(page.str()), ...)`（转**整页**）是错的 —— 连自己写的 `<p>` `<h1>`
都会被转成 `&lt;p&gt;`，浏览器把源码当文字显示。

正确位置在 `page <<` **里面**：

```cpp
page << "<p>名字：" << html_escape(form["name"]) << "</p>\n";
```

> **只转「不是我亲手写的字面量」那部分。**
> 一期的 `Markup` 那个坑是同一个错的两头 —— 那次是**把该当标签的也转了**。

### 20. `'...'` 是一个字符（本质是整数），`"..."` 是一串字符

`html_escape` 写错的那几版，值得单独记：

| 写法 | 编译器怎么算 | 结果 |
| --- | --- | --- |
| `'&amp'` | 多字符字面量 → `643919216` | 截成 char = **`'p'`**（112） |
| `'&lt'` / `'&gt'` | `2518132` / `2516852` | 截成 char = **`'t'`**（116） |
| `'&'+'a'+'m'+'p'+';'` | **整数加法**：`38+97+109+112+59` | **415** → 截成 char = **`-97`** |
| `"&amp;"` | 一串字符 | **`&amp;`** ✅ |

🔴 **`'a'` 就是 97。`+` 拼的是字符串，不是字符。**（`find('about')` → `'t'` 是同一个坑的第一次。）

⚠️ **编译器能抓的只是一部分**：`multi-character constant`、`overflow in conversion from 'int' to 'char'`、
`no return statement` 都骂了；但它**抓不到** `else` 只挂最后一个 `if`、抓不到「转整页」、抓不到少写的 `;`。

### 21. ⏱ 止血：`SO_RCVTIMEO` —— 把「永久堵死」降级成「最多 5 秒」

```cpp
struct timeval tv;
tv.tv_sec = 5;
tv.tv_usec = 0;
setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```

- **设在 `conn_fd` 上**，不是 `listen_fd` —— 超时是**每条连接自己的属性**
  （「这位客人不说话，我最多等他多久」）。对比 **`SO_REUSEADDR` 设在 `listen_fd`** —— 它管的是那扇**门**。
- **超时后 `read` 返回 `-1`**（**不是 `0`** —— `0` 的意思是对端正常关闭，两件事）
  → 代码里已有的 `if (n < 0) { ...; break; }` 直接接住，**别处一个字不用改**。

**实测**（空连接挂 10 秒、**1 秒后**打 curl）：`time_total = 4.036`（≈ 5 − 1）。

⚠️ 这是**治标**：那 5 秒里别人照样等。根治要等第 ④ 步。

⚠️ **2026-10-13 已废弃删除** —— `conn_fd` 改成 `O_NONBLOCK` 之后（决策 26），
`SO_RCVTIMEO` 不再起作用了，留着就是死代码，已随 epoll 那步一并删掉。
**保留这一条是为了记住「止血」这个思路**：先量出现象 → 上一副能立刻止住的药 → 再去做根治，
**而且根治完要记得回来把止血药拆掉**（否则代码里留着一副骗人的药）。

### 22. `&&` 串命令的坑：`pkill` 没找到进程时返回 1

```
g++ -Wall -o server server.cpp && pkill -x server && ./server     ← ❌
g++ -Wall -o server server.cpp && pkill -x server ; ./server      ← ✅
```

**服务器已经不在跑**的时候，`pkill` 返回 **1** → `&&` 断链 → **`./server` 根本不执行**；
而编译那一步是**成功**的、屏幕上一个错都没有 → **很容易以为服务器起来了**。

顺带两条实测：

- **必须站在 `oj_server/` 里**：仓库根下既没有 `server.cpp` 也没有 `www`
- **编译时旧进程还在跑不会 `Text file busy`**（`ld` 会先 unlink，实测 `exit=0`）

### 23. URL 解码放在哪（以及 `urldecode` 的第一个正经坑）

**一行**：

```cpp
        else
        {
            target = urldecode(target);      // ← 拼 road 之前、所有路径判断之前
            string road = "www" + target;
```

**验收的关键不是状态码**：`/hello%2Ehtml` 改前 `404 25` → 改后 **`200 228`**（解码生效）；
但 `/%2e%2e%2fserver.cpp` **改前改后都是 404** —— 光看数字，**分不出「没解码所以找不到」和「解码了但被拦住」**。

⭐ **那个 `228` 顺带证明了位置对**：解码要是排在 `string road = "www" + target;` **后面**，这条也照样 404。

⚠️ **`urldecode` 有两个使用者，规矩却不一样**：

| 这段字符在哪 | 谁在读 | 按哪套规矩 | `+` 是 |
| --- | --- | --- | --- |
| POST 的 body | `parse_form` | `application/x-www-form-urlencoded` | **空格** |
| 请求行的路径 | 路由 | URL 规范 | **加号本身** |

**第一次实测**（临时建 `www/a+b.html`，14 字节）：

```
/a+b.html    -> 25 字节（404）   ← 它拿着「a b.html」去磁盘找了
/a%2Bb.html  -> 14 字节（200）   ✅
```

又用**最现实的名字**验了一遍（临时建 `www/c++/index.html`，18 字节）：

```
/c++/index.html      -> 25 字节（404）   ← 去找「c  /index.html」（两个 + 变成两个空格）
/c%2B%2B/index.html  -> 18 字节（200）   ✅
```

`c++` 这名字一点都不假 —— 一个技术站放个 `c++/` 目录太正常了。

**修法：加一个默认参数**（`fc2acdc`）：

```cpp
string urldecode(const string &s, bool plus_is_space = true)
{
    ...
    if (plus_is_space == true && s[i] == '+') { out += ' '; }
    ...
}
```

- **`parse_form` 里那两处一个字都不用改**（走默认值 `true`）—— 又一次决策 2 那个
  「**默认参数让其他调用点不用动**」
- 路径那行：`target = urldecode(target,false);`

⚠️ 小 nit：`plus_is_space == true` 是多余的 —— `bool` 本身就能当条件，`if (plus_is_space && ...)` 更地道。

**回归实测（一次跑完 6 条）**：

```
/c++/index.html          -> 18     ✅ 修好了
/c%2B%2B/index.html      -> 18     ✅ 没坏
/hello%2Ehtml            -> 228    ✅ 没坏
/%2e%2e%2fserver.cpp     -> 25     ✅ 防御还在
form: name=a+b&msg=x%20y ->  名字：a b / 留言：x y    ✅🔴 form 侧的规矩原样保留
GET / 290 · GET /echo 387          ✅ 哪儿都没碰坏
```

⭐ **最后那条才是关键**：只证明「路径那条修好了」不够 —— 得证明**另一个使用者（form）的规矩一个字没变**。

🔴 **一条比 `+` 本身值钱的教训**：

> **「一个函数只对一种上下文正确」这件事，在只有一个调用点的时候看不出来。**
> **多一个不同上下文的调用点，它立刻变成坑。**

这正好是决策 2「把重复收成一个函数」的**反面**：**抽之前要先问一句「这两个调用点，规矩一样吗？」**

### 24. 🔒 加了 URL 解码之后，黑名单**又**多了一个洞 —— 这次是「顺序」

**对照实验**（三个版本并排跑，同一批 payload）：

| payload | 白名单（现役 `8080`） | 黑名单·检查在**解码前** `8081` | 黑名单·检查在**解码后** `8082` |
| --- | --- | --- | --- |
| `/%2e%2e%2fserver.cpp` | **25** | **10067** ← 服务器自己的源码 | **25** |
| `/%2e%2e%2f%2e%2e%2fweb/db_config.py` | **25** | **201** ← 数据库密码 | **25** |
| `/..` | **25** | **0** ← 漏 | **0** ← 漏 |
| `/hello%2Ehtml` | 228 | 228 | 228 |

`10067` 那条吐出来的头三行就是 `#include <sys/socket.h>` —— **服务器把自己的源码发出去了**。

🔴 **两个洞，性质不同**：

- **`/..`（结尾没斜杠）打穿两版黑名单** —— **跟顺序无关**，是「写法清单」本身漏了一条（决策 15）
- **`%2e%2e%2f` 只打穿「检查排在解码前」那版** —— 也就是「**先写防御、后来才加解码**」
  这个再自然不过的顺序。**黑名单的正确性依赖「检查排在所有变形之后」，而这个约定会随代码演进失效**

**白名单为什么不受影响**：

> **黑名单问的是「这一次的写法里有没有坏东西」；白名单问的是「算完之后它落在哪」。**
>
> **写法有无数种，而且每加一个功能就会变多**（URL 解码就是今天新加的）；**落点只有一个。**
> 你往管道里加一道新的「变形」（解码、规范化、去重斜杠……），**排在它前面的所有检查都自动失效**；
> **白名单站在管道最末端**，不管前面接了什么。

### 25. 前端重做（2026-10-12）—— 以及「把 HTML 塞进 C++」的代价

`www/` 五个文件全套重做（**前端不占他的练习时间，我直接写**）：

| 文件 | 旧 | 新 |
| --- | --- | --- |
| `style.css` | 259 | **3784** |
| `index.html` | 290 | **1338** |
| `about.html` | 220 | **1537** |
| `hello.html` | 228 | **1128** |
| `echo.html` | 387 | **1426** |

统一顶栏 + 卡片 + 表单样式 + **暗色模式**（`@media (prefers-color-scheme: dark)` 配 `:root` 变量）+ 内联 SVG favicon。

**零 JS、零外部依赖**（不引 Google Fonts、不引 CDN）。除了离线可用，还有个**本项目的**理由：
**每多一个请求，就多一次撞上队头阻塞的机会** —— 现在一页只有 2 个请求（HTML + CSS）。
那个内联 favicon 也不是纯装饰：它**干掉了浏览器每次都要的 `/favicon.ico` 那个 404**。

🔴 **`/echo` 的结果页是 `server.cpp` 现场拼的**，所以得自己带上 `<link rel="stylesheet">`。
加完之后 —— **导航栏那份 HTML 在 5 个地方各有一份**（4 个静态页 + C++ 里一份）：**改导航要改 5 处**，
**跟一期那个 `nav()` 是同一个病**。

- **静态页那 4 份躲不掉** —— 静态站天生如此（所以才有人做「静态站点生成器」）
- **C++ 这份能躲掉**：让结果页读 `www/echo_result.html` 做占位符替换。**但换进去的值必须已经转义过**，
  否则刚堵上的 XSS 会从模板这条新路爬回来。**先不做，记着。**

### ⚠️ 我自己踩的坑：替换范围没框准 + 提交前不看结果

前端重做时我给的指令是「替换现在那 9 行 `page <<`」。**可自然的选择范围连 `send_response(...)`
一起框进去了** —— 那行被吃掉，`POST /echo` 变成**空回答**（curl 退出码 52）。

**更该说的是**：我**先跑了冒烟、返回是空的，却照样 `git commit` 了**。提交前那一步就是拿来拦这个的。

🔴 **同一行的第二次**：
- **a-2**：他自己写的时候漏了 `send_response`（`page` 拼完没人发）
- **这次**：我给的替换范围把**同一行**吃掉了

**规律：`stringstream` 那一整块（`<< 拼` + `send_response 发`）是个整体，
一旦讲成「替换哪几行」，就一定有人会把发的那行切掉。**

### 26. epoll 事件驱动（2026-10-13/14，`b8e7289` + `1f6d20f`）

**换掉的是什么**：`main` 里「阻塞的 `accept` + 内层 `while(true){ read }`」整段。

```cpp
fcntl(listen_fd, F_SETFL, ... | O_NONBLOCK);
int epfd = epoll_create1(0);
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);      // 只登记一次
epoll_event events[64];
while (true) {
    int n = epoll_wait(epfd, events, 64, -1);         // 没动静就睡
    for (int i = 0; i < n; i++) { ... }
}
```

**三个决定**：

- **只登记一次**（`epoll_ctl(ADD)` 一次），不是每轮都把 fd 集合重传一遍 —— 这正是 epoll 相对 `select` / `poll` 快的根源
- **`conn_fd` 也必须 `O_NONBLOCK`** —— 不然 `read` 会阻塞在那一条连接上，epoll 就白搭了
- **那个 `-1` 是「睡到有事件为止」** —— 忙等（非阻塞 + 循环反复调）会把 CPU 烧光；
  和条件变量里「厨师没单子就睡」是同一个道理（决策 27）

**半包 / 粘包在这里的形态**：`epoll_wait` 只说「**有数据**」，**不说**「有一个完整请求」。
所以按 fd 攒字节（`map<int,string> conn_bufs`），再判够不够。

**`request_total_len()`**：返回「这个请求一共要多少字节」=【`head_end + 4 + Content-Length`】；
头部还没齐 → `string::npos`（= **还不知道**）。**三道防线**：`pos < head_end`（`Content-Length:` 出现在 body 里不算）、
`end > start`（值是空的跳过）、`try/catch`（值不是数字 → 当它没有 body）。

⚠️ **`stoi("")` 是真崩溃**：`Content-Length:` 值为空时抛 `std::invalid_argument` → **整个进程 abort**（退出码 **134**）。

> **凡是解析外部输入的地方，都要问一句「烂输入会怎样」—— 服务器不该因为一个烂请求而死。**

**实测**（数字见「实测数字汇总」）：

- 9000 字节的请求 → `4096 / 8192 / 9154` 三行 —— **电平触发**，一次没读完下一轮接着报
- 连打 3 个请求 fd 数稳定 **6**、`ss` 无 `CLOSE-WAIT`；**上一版在同一处是 `CLOSE-WAIT` + fd 一路漏**
- **半包** POST（body 拆两段发）与 curl 一次发完，`Content-Length` **都是 772、逐字节相同**
- **粘包**（两个 GET 一次发）客户端只收到 **1 个**响应（1438 字节）

### 27. ⭐ 线程池（2026-10-15，`520a3fa`）—— `CH-11` 的另一半

**epoll 治「等」、线程池治「干」** —— 一对孪生兄弟：

| | 什么时候独占主线程 | 谁治的 |
| --- | --- | --- |
| 第一次 | **等**（`read()` 阻塞） | ✅ epoll（决策 26） |
| 这一次 | **干**（`handle_request` 执行中） | ✅ 线程池（本条） |

**现在看不出来**（只解析 + 读个小文件，微秒级）；**接上判题器就咬人**：
判一道 TLE 要 **2.05 秒** → 一个人交超时题，**全站卡 2 秒**。

**4 个零件**（放在 `handle_request` 之后、`main` 之前）：

```cpp
struct Task { int conn_fd; string req; };                    // ① 一张订单
queue<Task> task_queue; mutex mtx; condition_variable cv;    // ② 窗口 + 锁 + 叫醒器
void worker() { ... }                                        // ③ 厨师
// main 里：thread t1(worker), t2(worker), t3(worker), t4(worker);   // ④
```

**主循环里变的只有一样**：`handle_request` + `close` 换成 `push` + `notify_one`（摘牌 / 撕页原样不动）。

**四条讲清的**：

1. ⭐ **锁只圈「碰队列」的行，真活都在锁外** —— `handle_request` 要跑 2 秒，圈进锁里
   ＝ 厨师 1 做菜、2/3/4 全被锁在门外 → **线程池退化成单线程**。`worker()` 里那个花括号就是这个边界
2. ⭐ **`close` 必须跟着 `handle_request` 一起搬** —— 否则主线程丢完任务**立刻**关 fd，
   **fd 号会被下一个 `accept` 复用**，而工人还在往那个号写 → 写串。
   **实测现场**：连续三个请求都是 `fd = 5`（号被复用了三次）。
   搬完之后「工人不 close，号就不还」→ 主线程拿不到同一个号 →
   **竞态在设计上产生不了**（是消掉，不是加检查挡掉）
3. **`Task t;` 在 `worker()` 的栈上 → 线程私有** —— 4 个线程各有一份自己的 `t`，所以它不需要保护；
   锁只保护**共享**的 `task_queue`。一句话：**栈私有，堆和全局共享**
4. **`-pthread` 不能忘** —— `std::thread` 要链接 `libpthread`，g++ 不会自动加。
   不加通常**能编译过**、一跑就 `terminate ... std::system_error`（**代码没问题，是命令少了一个词**）

**同步机制为什么这么写**（对着 `worker()` 读）：

- **锁**：`task_queue` 是 1 主 + 4 工共用的；`push` 要「写内存 + 长度+1」两步，改到一半被别人看见就坏
- **RAII**：`unique_lock<mutex> lock(mtx);` 出生即锁、**出花括号即解** —— 中间 `return` 或抛异常都不会漏解锁。
  同一族：`unique_ptr` / `lock_guard` / **`ifstream`**（他从没写过 `f.close()`，一直是析构帮你关的）
- **条件变量**：厨师没单子时**不能反复开门看** —— 那叫**忙等**，4 个核 100% 烧着而一单没做
  （同决策 26 那个 `-1`）。`cv.wait(lock, pred)` 等价于 `while (队列空) cv.wait(lock);`，
  内部四步：**问 → 原子地解锁+睡 → 被叫醒重新上锁 → 再问**。两个「为什么」：
  **丢通知**（解锁和睡下之间被插队，那声喊就作废了）／**醒来必须再检查**
  （虚假唤醒 + 抢输 → 直接 `front()` 一个空队列是 UB）

**实测（同一个二进制，只改「起几个线程」这一行）**：

| 场景 | 耗时 |
| --- | --- |
| 4 工人 + `worker` 里 `sleep(1)` + 4 个并发请求 | **1.034s** |
| 1 工人 + 同上 | **4.019s** ← 这一版**行为上就等于加线程池之前** |
| 4 工人 + 无慢活 | **0.030s** |

功能回归 4 条**逐字节相同**：`/` 1338 · `/about.html` 1537 · `/style.css` 3784 · `/nosuchpage` 404 25
—— 就是 `CH-11` 要的「**对浏览器端透明**」。`ls /proc/<pid>/task | wc -l` = **5**（4 工人 + 1 主线程）。

### 28. 接 MySQL（2026-10-15 ~ 10-16）—— 从「连上」到「连对」

`CH-11` 队列里「接 MySQL → 接判题器」的前半。**服务端第一次从库里读出真数据、渲染成网页。**
路线照老规矩：`db_test.cpp` **先独立跑通连接**（同当初 `try_judge.py`）→ `render_problems()` ＋ `/problems` 路由 → 再谈复用。

#### 28a. 编译要带库 —— `undefined reference` 是**链接器**说的话

    g++ -Wall -o server server.cpp $(mysql_config --cflags --libs)

`-I` 管**编译**（头文件去哪找），`-L` / `-l` 管**链接**（实现去哪找）。
少 `-l` 的症状永远是 `undefined reference`，而且**只有真的用到那个库里的函数时才报**。

#### 28b. 🔴 SQL 漏了 `WHERE`，被 1 条数据藏住了

    SELECT problem_id, title FROM problems                                     ← 错的
    SELECT problem_id, title FROM problems WHERE is_deleted = 0 AND is_published = 1

**复现**：造一道 `is_published = 0` 的草稿题 → `curl /problems` **把它端端正正列了出来** —— 违反 `OJ-PRB-01`「未发布的题目对学生不可见」。
库里只有 1 道已发布题，所以**看不出来**。→ 同 `42 接雨水` 那条「**样例过了 ≠ 对，因为没走到那条分支**」。

#### 28c. ⭐ 每次请求新建一条连接 = 57ms

**实测对照**（各 20 次 `curl -w '%{time_total}'`）：

| 路由 | 平均 |
| --- | --- |
| `/about.html`（纯静态） | **0.7 ms** |
| `/problems`（只取 **1 行**！） | **57 ms** |

查询本身是微秒级 —— **那 57ms 基本全是 `mysql_real_connect`**：TCP 三次握手 + MySQL 认证握手 + `caching_sha2_password` 的额外往返 + 关闭时的四次挥手。
**为「进一次门」付 57ms，而「拿一样东西」只花 0.1ms。**

#### 28d. ⭐⭐ 修法：`thread_local` 每线程一条 —— 57ms → 1.2ms（**47 倍**）

三个层次，选了中间那个：

| 方案 | 效果 |
| --- | --- |
| ① 一条全局连接 + 互斥锁 | 复用了，但**把线程池的并发锁回串行** |
| ② **每线程一条**（`thread_local`） | **各用各的，零锁零等待** ← 选这个 |
| ③ 连接池（队列 + cv） | 通用，适合「连接数 ≠ 线程数」；**骨架和本项目的线程池一模一样** |

线程数是**死的 4 个**，一人一条永远不用抢 → ② 是正解，③ 属过度设计。

    MYSQL *get_conn()
    {
        thread_local MYSQL *conn = nullptr;        // 每个线程一份，惰性初始化
        if (conn == nullptr)
        {
            conn = mysql_init(nullptr);
            mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, DB_PORT, NULL, 0);
        }
        return conn;
    }

**实测（20 次）**：

    0.123  0.084  0.075  0.111      ← 慢的 4 次
    0.0086                          ← 分界
    0.0010 0.0010 0.0010 ...        ← 之后 16 次

**慢的那 4 次正好是 4 个工人线程的「首单」** —— `thread_local` 是**惰性初始化**：哪个线程先来，哪个才建。**连接「长出来」的过程被完整量到了。**
1.2ms 里有 0.7ms 是 curl ＋ 读文件 ＋ 网络的底噪 → **MySQL 那块只剩 ~0.5ms，贴着理论下限**。

**冷启动**：服务重启后前几个用户白等 100ms。解法 = `worker()` 开头调一次 `get_conn()` 预热（**已加**）。

#### 28e. 🔴 改造时漏掉的孪生兄弟 —— `thread_local` 持有悬垂指针

连接归**线程**了，**一次请求的结束不再是连接的结束**。所以正常路径末尾的 `mysql_close(conn)` 必须删 —— **错误分支里那两个同样要删**。

    if (mysql_query(conn, "...") != 0)
    {
        mysql_close(conn);        // ← 关的是【这个线程的】连接，thread_local 里的指针没跟着清
        return "...";
    }

没删的后果：某次查询失败 → 连接被关 → 那个指针还指着它 → **这个线程下次干活就是 use-after-free**（段错误，或者更糟：不崩但数据乱）。
→ 同 `76` 的 `cnt[out]--` / `satisfied--`、`438` 的「滚动差一位」：**一个动作有一处要改，它就有几处要改。**

#### 28f. ✅ 补上：`get_conn()` 的失败检查（2026-10-16）

原来 `mysql_real_connect` 的返回值**没人看**。失败时 `conn` **不是 `nullptr`**（`mysql_init` 已经给了句柄）
→ 下次进来 `if (conn == nullptr)` 为假 → **这个线程永远抱着一具死连接**。

改法：失败时 `mysql_close(conn); conn = nullptr; return nullptr;`。**`render_problems()` 不用改** ——
它开头那句 `if (conn == nullptr)` 上一轮顺手就写好了，当时进不去（`get_conn` 从不返回 `nullptr`），**这次才真正开始干活**。

> **一个函数开始报错，所有调用它的人就都得接住。** 这次省事是因为接的位置**上一轮留好了** —— 留没留是运气，不是纪律。

##### 🔴🔴 第一版写反了：`if (指针)` 和 `if (错误码 != 0)` 是两套规矩

```cpp
if (mysql_real_connect(conn, DB_HOST, ...))     // ← 反的
{
    mysql_close(conn);                          // ← 关的是一具【刚刚握手成功】的健康连接
    conn = nullptr;
    return nullptr;
}
```

`mysql_real_connect` 返回的是 `MYSQL *`（**指针**）：成功 → 返回那个句柄（**就是 `conn` 本身**）；失败 → `NULL`。
所以 `if (ptr)` 读出来是「**连上了吗**」—— **为真 = 连上了**。

**实测症状（不崩、不报错、日志干净、HTTP 200 —— 它「成功地」返回了一句错话）**：

```
/about.html  200   0.0017      ← 静态页正常
/problems    200   0.0618      ← ←← 61ms！
正文：<p>数据库暂时不可用</p>
```

⭐ **那个 61ms 就是铁证**：它就是 §28c 量到的 57ms **又回来了** ——
每次请求都**连上**（付完整握手钱）、**立刻关掉**、再对外说「不可用」。
**连不上根本不需要 57ms。**

🔴 **同一个 MySQL API 里两个函数用两套约定**，而且就隔着三行：

| 函数 | 返回 | 成功的标志 |
| --- | --- | --- |
| `mysql_query` | `int` 错误码 | `0` |
| `mysql_real_connect` | `MYSQL *` | **非 `NULL`** |

> 判「成功了吗」之前，**先看清返回值是什么类型**。这不是缺知识 —— 上面写「失败时返回 NULL」的是同一个人，
> 是**知道事实、但手按老习惯走**。

**修完实测**（条件改成 `== NULL`）：

```
/problems 正文   <h1>题库</h1><ul><li>1 A+B 问题</li></ul>        ✅
/problems 稳态   0.83 ~ 1.14 ms（连打 10 次）                    ✅ 61ms → ~1ms
/about.html      0.55 ms                                          ✅ 没受影响
```

**顺带复现了 §28d 那个现象**（重启后 0.3 秒就开始连打 8 次）：

```
0.0175  0.0146  0.0361  0.0035  |  0.00098  0.00095  0.00087  0.00089
└──── 4 个慢的 = 4 个工人线程的「首单」 ────┘   └────── 之后全是稳态 ──────┘
```

**慢的正好 4 次** —— `thread_local` 惰性初始化，哪个线程先来哪个才建自己那条。
**等 6 秒再打就一次都看不到**（同一份二进制）。

**还欠着**：错误分支把 `mysql_error(conn)` 原文端给浏览器 = **泄露主机名 / 库名 / 用户名**。

#### 28g. 两个环境坑（都会咬人）

- **`bind: Address already in use`** —— 旧进程还占着 8080（`SO_REUSEADDR` 挡不住「已有活进程在监听」）。
  ⚠️ **千万别 `pkill -f server`**：这台机器上跑着 **`vscode-server`**，会被一起杀掉、编辑器当场断线。
  **规矩：先 `ss -tlnp | grep 8080` 拿 pid，再 `kill <pid>` 点名杀。**
- `.gitignore` 还差一条 `oj_server/db_test`（编译产物）。

#### 本次改动清单（已提交）

`db_config.h`（新，含密码，已进 .gitignore）、`server.cpp`（`get_conn` / `render_problems` / `/problems` 路由 / `worker` 预热 / 两个 `mysql_close` 删除 / **失败检查**）、`.gitignore`（+`oj_server/db_config.h`）。`db_test.cpp` ＋ `db_test` **跑完即删**（同一期 `web/db_test.py` 的先例）。

**下一步**：~~接判题器~~ → 已走完，见 §29。

---

### 29. ⭐ 接判题器（2026-10-16 ~ 10-19，`694f772` → `eaff82d`）

`CH-11` 的前四步都是「把一期搬过来」，这一步是第一块**真正属于二期的新业务**：
浏览器交一份 C++，服务端 fork 判题器、收结果、落库、再把结果页刷出来。

**先拍板的三个决定**（2026-10-16）：

1. **先绕开登录** —— C++ 侧 `grep -ci "cookie|session|login"` = **0**；一期登录才 5 行，留到最后补。
   → 代价：`submissions.username` 恒为 `NULL`（见 29h）。
2. **判题工人另起一组**：**2 个判题线程 + 独立判题队列**，跟 HTTP 的 4 个工人分开。
   理由：判题是**秒级到分钟级**，HTTP 是**毫秒级**；混在一个池子里，一次判题就把 HTTP 堵死。
3. 顺手轮换了 `oj_app` 的数据库密码。

**五个子步：**

| 子步 | 内容 | 提交 |
| --- | --- | --- |
| 1 | `run_judge()` —— fork/exec + 管道收 stdout | `694f772` |
| 2a | `/problem/<题号>` 路由 + `render_problem()` | `0da4c11` |
| 2b | 题面 8 字段 + 提交框（前置：`page_head()` / `page_tail()`） | `4a5d117` / `a447c85` |
| 3a | `POST /submit/<题号>` 接表单 + 空代码校验 | `618ec8f` |
| 3b | `make_job_id()` → 工作目录 → 落库 → 302 | `a105b5c` / `c99a55d` / `3457965` |
| 4a | `JudgeTask` + 判题队列 + 入队 | `203bbb7` |
| 4b | 判题工人取任务、调 `run_judge()` | `131a870` |
| 4c | 解析输出 → 写回数据库 → 清理临时目录 | `2ba0973` / `519658e` |
| 5 | `/result/<job_id>` 结果页（自动刷新 + 转义） | `eaff82d` |

（另有一次前端改版 `80f8ccc`：蓝白判题单风格，首页 / 题库 / 题目页 / 编辑器。）

#### 29a. `run_judge()` —— 我们当的是「可执行文件的客户」

```c
bool run_judge(const string &src_path, const string &data_dir,
               int tl_ms, int ml_mb, string &out);
```

- **第二参数是目录，不是文件。** 他最初写的形状是 `judge_result(in_path, ans_path, ...)` ——
  那是判题器**内部** `run_one_test()`（管**一组**测试点）的形状。
  我们当的是**判题器这个可执行文件**的客户，它自己遍历整道题，所以给的是**题号目录**。
  （他自己 NOTES 里那句「换一道题 = 换 `argv[2]`」正好是答案。）
- **⭐ `cwd` 是关键。** 判题器 `compile()` 里 `g++ -o test`、`run_one_test()` 里 `output.txt` **全是相对路径**，
  落在它**自己的 cwd**。所以子进程必须 `chdir(源码所在目录)`。
  **不 chdir 的后果有两层**：① 产物掉进 `oj_server/`；② **两个判题工人踩同一个 `test`**
  —— 拿 A 的产物跑 B 的测试点，**静默错判**。
  这正是 `judge_demo/NOTES.md`「已知最严重缺陷」（FIX-01）的同一条，接进服务端后以更大形式回来。
- **⭐⭐ 判「判题器起没起来」只能用 `waitpid` 的 `status`：**
  ```c
  WIFEXITED(status) && WEXITSTATUS(status) != 127   // 127 = 子进程没起来的哨兵
  ```
  **不能用 `out == ""`** —— 空串有**两义**：`exec` 失败（`_exit(127)`）**和**
  「跑起来了但一句话没说」（`argc < 3` 时 judge 只往 stderr 打 Usage、退 1）。

**实测三个数**（预期与实测全中）：

| 场景 | raw `status` | 退出码 |
| --- | --- | --- |
| AC | `0` | `0` |
| WA | **`256`** | `1` |
| judge 路径不存在 | **`32512`** | `127` |

`256 = 1<<8`、`32512 = 127<<8` —— 这两组数就是「status 是打包整数」的铁证（低 7 位信号号，高 8 位退出码）。

#### 29b. 路由前缀：`compare` 不是 `find`

```cpp
target.compare(0, PROBLEM_PREFIX.size(), PROBLEM_PREFIX) == 0
```

他先写的 `target.find("/problem/")` —— **`find` 问的是「在不在里面」，不是「在不在开头」**，
`/xxx/problem/1` 也会命中。同决策 15「黑名单 vs 白名单」：**问错了问题**。

题号抠出来之后**三道关卡**：

1. `!num.empty()` —— 挡 `/problem/`
2. `find_first_not_of("0123456789") == npos` —— 挡 `stoi("1abc") == 1` 这种**静默给错值**
3. `try/catch` 接 `out_of_range` —— 挡 `/problem/99999999999999999999`

**⭐ 404 必须放在 `if` 外面（单一出口）。** 他一开始写在 `if` 里面 → 烂输入走不进 `if` →
**一个字节都不发** → curl 拿到 `000` 而不是 `404`。
`404` = 服务器**说了话**；`000` = **一个字没说就关连接**，客户端分不清「崩了」还是「忘了回话」，
浏览器上就是转圈。**光点浏览器发现不了这个。**

#### 29c. 每次提交一个独立的临时工作目录

```cpp
char tmpl[] = "/tmp/oj_XXXXXX";
char *dir = mkdtemp(tmpl);          // 原子地建目录 + 拿到唯一名字
```

→ 写 `work_dir/submission.cpp` → `JudgeTask` 带上这个路径 → 判完 `remove_all`。

**为什么必须是 `mkdtemp`**：并发的两次提交如果共用一个目录，就是 29a 里那个「A 的产物跑 B 的测试点」。
唯一目录名是**正确性**，不是整洁。

#### 29d. 判题队列 + 2 个判题工人

```cpp
thread t1(worker), t2(worker), t3(worker), t4(worker);   // HTTP：4 个
thread j1(judge_worker), j2(judge_worker);               // 判题：2 个
```

- `JudgeTask` 队列由 `judge_mtx` + `judge_cv` 保护，**和 HTTP 的 `task_queue` 是两条独立的线**。
- 状态机：`排队中` →（工人取走）`判题中` →（写回）`完成`。
- **判题工人数 = 2 是个取舍**：每个判题进程会 fork 出一个 `g++` 和一个跑测试点的子进程，
  再加 scrypt 量级的内存/CPU，开多了会把机器压住。这是课程设计尺度下的取值，不是理论最优。

#### 29e. 解析判题器输出：`parse_judge()`

判题器输出是**人读的文本**，所以靠字符串抠（一期 FIX-06 同款做法，原始文本继续存库当现场证据）：

| 行 | 抠出 |
| --- | --- |
| `VERDICT: AC` | `verdict` |
| 单独一行 `AC` | `passed++`（每个通过的测试点一行） |
| `time: 12 ms` | 取**最大值** |
| `peak memory: 2048 KB` | 取**最大值** |

三个值得记的点：

- **取 max 不取 last** —— 多个测试点报的是「最慢那一组 / 峰值最大的那一组」。
- **`has_time` / `has_memory` 两个标志** —— 没出现时写 **`NULL` 而不是 `0`**。
  `0 ms` 是**合法值**，不能用它表示「没有这个数」。同决策里的老规矩：**别让一个值承担两种含义**。
- **`\r` 要手动剥** —— `getline` 把 `\r` 留给你（决策 4），判题器输出在管道里通常是 `\n`，
  但别赌。

#### 29f. 写回数据库 + 清理

一条 `UPDATE` 写完七列：`status / result / verdict / passed / total / time_ms / memory_kb`。

- **`started == false` 时 `verdict` 写 `"SE"`**（System Error）——
  **判题器没起来不是用户的错**，不能混进 WA/RE。
- **`result` 存原文**，不存解析后的结构 —— 出怪问题时这是唯一的现场。
- **`escape_sql()` 包一层** `mysql_real_escape_string` —— `result` 里会含 g++ 的错误信息，
  里面带引号是常态。
- 清理用 `std::filesystem::remove_all(dir, ec)` 的**带 `error_code` 重载** ——
  删不掉只记日志，**不让它抛异常把工人线程带走**。

#### 29g. `/result/<job_id>`

- `valid_job_id()`：**8 位小写十六进制**（和 `make_job_id()` 对齐）。
- 未完成时用 `<meta http-equiv="refresh" content="2">` **每 2 秒自动刷新**，完成就停。
- 原始输出经 `html_escape()` 再进 `<pre>` —— **一期 FIX-17 的教训，二期不能再犯一次**。

#### 29h. 这一步留下的已知缺口

- ⚠️ **`submissions.username` 恒为 `NULL`** —— 没有登录，提交不知道是谁的。
- ⚠️ **`/result/<job_id>` 无归属校验** —— 一期 `FIX-11` 已解决（越权 403），**二期这里是退步**，
  等登录到位必须补回来。
- ⚠️ **`data_dir` 是硬编码绝对路径**（`/home/ubuntu/online-OJ/judge_demo/testdata/<题号>`）——
  换机器就得改代码。
- **结果解析靠字符串抠** —— 判题器改一个字（比如 `time:` 变 `Time:`）整列就静默变 `NULL`。
  一期的 `parse_result()` 有同样的问题，这是同一个「协议靠格式约定」的代价。

**下一步**：~~登录 / 会话~~ → 已走完，见 §30。

### 30. ⭐ 登录 / 会话（2026-10-21 ~ 10-22，`07774a8` + `450d7ef` + 本步）

把 §29h 的「无主提交、无归属校验」两条旧账一起还了，顺手补齐注册与改密。
这一节基本是 CRUD 样板，值得记的只有几处**安全语义**。

#### 30a. 存哈希：认 werkzeug 的格式，不自己另起炉灶

库里原有用户是**一期 Flask** 注册的，`password_hash` 是 werkzeug 的格式：

```
scrypt:32768:8:1$<16 位盐>$<128 位十六进制>
```

所以二期**不引入新格式**，直接照这个格式生成和校验：

- `parse_scrypt()` 把四段（`N:r:p`、盐、期望的 hex）拆出来；
- `EVP_PBE_scrypt()` 现算，`CRYPTO_memcmp()` 做**常数时间比较** —— 普通 `==` 会按第一次不同的字节提前返回，理论上可被计时攻击。
- 生成端 `make_password_hash()` 用同一组参数 `N=32768, r=8, p=1`（werkzeug 默认），盐从 62 个字母数字里取 16 位。

**关键点：两期共用一张 `users` 表、一种哈希格式** —— 换了实现，账号还能登录。
这就是 `CH-11`「功能不变、仅替换实现」在数据层的体现。

#### 30b. 会话：票号存库，不是 JWT

- 票号 = **32 位小写十六进制**（128 位随机），存 `sessions(session_id, username, created_at, expires_at)`。
- 校验只看两件事：**票号在库里** + **`expires_at > NOW()`**。
- 过期行不做后台清理，靠查询条件自然失效（课程尺度够用；量大了再加定时删）。
- **为什么不用 JWT**：服务端 session 能**立刻作废**（`DELETE` 一行就下线），
  改密后踢人、封号、管理员强制下线都靠这一条。JWT 没有黑名单就得等它自己过期。
  Redis 那一步会把这张表搬过去（`SETEX` 天然带 TTL）。

#### 30c. cookie 的三个属性

```
Set-Cookie: session=<32hex>; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400
```

| 属性 | 作用 |
| --- | --- |
| `HttpOnly` | JS 读不到 → XSS 偷不走票 |
| `SameSite=Lax` | 别的站点的 POST 不带票 → **CSRF 的第一道（也是唯一一道）防线** |
| `Max-Age` | 和服务端 `expires_at` **同一个常量** `SESSION_TTL_SECONDS`，两边不许各写各的 |

解析 cookie 时 `split(';')` 之后要**去掉前导空格** —— 浏览器发的是 `a=1; b=2`。

#### 30d. 路由守卫：一个入口，一个 `current_user`

每个请求进来时先算一次：

```cpp
string current_user;
if (!session_id.empty()) current_user = get_session_user(session_id);
```

然后：

- `/submit/*`：`current_user.empty()` → **302 `/login`**。顺带把 `INSERT` 里恒为 `NULL` 的 `username` 填上 —— 提交终于有主了。
- `/result/<job_id>`：先 `get_submission_owner()` 查归属，**本人或 admin** 才放行，否则 **403**。
- `/profile`：未登录直接重定向。

**`404` 和 `403` 的分工**：job_id 不存在 / 格式不对 → **404**（连「有没有这条」都不说）；
存在但不是你的 → **403**（「这不是你的提交」）。一期 `FIX-11` 就是这么分的，二期对齐。

#### 30e. 老数据的坑：`username IS NULL`

守卫上线前提交的记录 `username` 是 `NULL`。`get_submission_owner()` 把它读成空串 ——
于是这些记录的 owner 不等于任何用户名，**普通用户看不了，admin 能看**。这是有意的：
无主数据不给普通用户碰，但管理员得能查。

#### 30f. 注册：重名交给数据库，不先 `SELECT` 再 `INSERT`

```sql
INSERT INTO users (username, password_hash, created_at) VALUES (...)
```

失败时看 `mysql_errno(conn) == 1062`（唯一键冲突）→「这个用户名已经被注册了」。

**「先查再插」有竞态**：两个同名请求同时查，都查不到，然后都去插 —— 一个成功一个报 1062。
让**数据库的唯一键**当裁判，竞态天然消失；代码也少一次往返。

#### 30g. 改密：验旧、换新、踢人

顺序不能反：

1. 先验 `old_password`（拿新哈希去比没意义，等于谁拿到 cookie 谁就能改密）；
2. 写新哈希；
3. `DELETE FROM sessions WHERE username=? AND session_id != ?` —— **踢掉其它设备**，留当前这条。
   旧会话不删的话，偷到旧票的人仍然在线。

#### 30h. `nav_user`：`thread_local` 在这里刚好对

页面头部要显示「谁在登录」。导航由 `page_head()` 拼，它没有 `current_user` 参数，
于是用一个 `thread_local string nav_user`，在 `handle_request()` 开头赋值。

**为什么够用**：一个请求从解析到 `send_response` **始终在同一个工人线程里跑完**，中间不换线程。
所以 `thread_local` 不会串到别的请求 —— 换成全局 `string` 就会（决策 27 的并发审法：
**先问「这东西谁在碰」**）。

#### 30i. 本步留下的缺口

- ⚠️ **没有 CSRF token** —— 只靠 `SameSite=Lax`。它挡得住跨站表单，但挡不住同站 XSS 伪造。
- ⚠️ **登录没有失败次数限制** —— 可以无限撞密码（scrypt 慢，但仍然能撞）。
- ⚠️ **注册不校验密码强度 / 用户名可用字符** —— 只限了长度 ≤ 32。
- ⚠️ **`/login` 已登录仍可访问** —— 没做「已登录就别再登录了」的重定向。
- ⚠️ **每请求一次 `get_session_user()` 查询** —— 认证开销落在 MySQL 上；
  后期要么加缓存，要么把会话搬 Redis（本来就要搬）。

**下一步**：~~`/submissions`~~ → 已走完，见 §31。

### 31. 我的提交列表 + 源码保存（2026-10-22，`66627af` 之后）

需求对应：`OJ-REC-02`（历史提交列表）+ `OJ-REC-01`（结果页展示源码，原先因为源码没落库一直是 ☆）。

#### 31a. 查询串拆分：`path` 和 `query` 必须分开

原来路由是 `target == "/problems"` 这种**精确比较**，一加 `/submissions?user=alice` 直接 404（`target` 里带着 `?user=alice`）。所以请求行解析完先拆：

```cpp
size_t qpos = target.find('?');
string path  = target.substr(0, qpos);                     // npos 时 substr(0,npos) = 整串
string query = (qpos == string::npos) ? "" : target.substr(qpos + 1);
```

- **下游所有路由比较、「`www` + path」、`urldecode` 都改用 `path`**。
- ⚠️ **不能写无条件版本**：`npos + 1` 在 `size_t` 里**回绕成 0**，`substr(0)` 返回**整个 target** ——
  不抛异常、不崩溃，`query` 静悄悄变成 `"/problems"`。**「静默地给错值」比崩溃难查得多。**

#### 31b. 查询串直接复用 `parse_form()`

`parse_form()` 本来就是「按 `&` 切、按 `=` 切、两端 URL 解码」，跟查询串的语法完全一致，
连 `+` 解码成空格的那个开关都正好对（决策 23）。

```cpp
map<string, string> query_params = parse_form(query);
```

#### 31c. 列表 SQL：`LEFT JOIN` + 双排序键 + `LIMIT`

```sql
SELECT s.job_id, s.username, s.problem_id, p.title, s.status, s.created_at
FROM submissions s LEFT JOIN problems p ON s.problem_id = p.problem_id
[WHERE ...]
ORDER BY s.created_at DESC, s.job_id DESC
LIMIT 100
```

- **`LEFT JOIN` 不是 `INNER JOIN`**：17 条提交里有 **9 条 `problem_id IS NULL`**，
  `INNER JOIN` 会**安安静静少给 9 行**（跟一期那次 10 条少 9 条是同一课）。
- **第二排序键**：`created_at` 只有秒级精度，同一秒多条顺序不确定（`FIX-16` 的缓解，不是根治）。
- **`LIMIT 100` 是硬上限**，没做翻页。需求里翻页是「（可选）」。

#### 31d. 权限：`WHERE` 由 `admin` 决定，不认参数

```cpp
if (!admin)                     where = " WHERE s.username = '" + escape_sql(viewer) + "'";
else if (!filter_user.empty())  where = " WHERE s.username = '" + escape_sql(filter_user) + "'";
```

**学生传 `?user=testuser` 也只看得到自己的** —— 分支看的是「你是不是管理员」，**不是「参数里写了谁」**。
实测：学生带该参数 → **0 行**，标题仍是「我的提交」。

#### 31e. 两个 NULL 各说各话

| NULL 在哪 | 页面上 |
| --- | --- |
| `username` 为 NULL | 提交者显示「游客」（这一列引入前的历史数据） |
| `problem_id` 为 NULL | 题目显示「（老数据·无题号）」 |

两者**不是一回事**：有 3 条记录 `username` 有值但 `problem_id` 是 NULL（那一列更晚才加）。
实测共 **6 个「游客」+ 9 条「无题号」**，页面没把这两种 NULL 混成一个。

#### 31f. 源码落库：`submissions.source MEDIUMTEXT`

**动机不是"顺便"** —— 是用户在实际使用中撞出来的：提交完判完，回题目页一看**刚才写的代码全没了**。
根因是题目页的 `<textarea>` 永远是空的（`render_problem()` 根本不查历史），
能不能恢复全看浏览器愿不愿意帮你留表单状态 —— **真实 OJ 不赌浏览器**。

三件事：

1. **提交时存**：`INSERT` 多一列 `source`，值用 `escape_sql(code)`（代码里的引号、反斜杠是常态）。
2. **题目页预填**：`render_problem(problem_id, viewer, html)` 先查
   `WHERE username = viewer AND problem_id = N ORDER BY created_at DESC, job_id DESC LIMIT 1`，
   取**最近一次**（不论判得对不对）填进 textarea，**`html_escape` 之后**再填。
3. **结果页展示**：`/result` 读取 `source` 显示在 `<pre class="result-output">`；
   老数据（NULL）显示「（老数据·无源码）」；**判题没完也显示**（交上去的是哪份，先让人看见）。

**两个实现坑**：

- ⚠️ **同一条 MySQL 连接上，结果集没 `mysql_free_result` 之前不能发下一条查询** ——
  否则报 `Commands out of sync`，而且这条错误**报在下一条命令上**，极难查。
  所以「查上次源码」放在**查题目之前**（那时连接是干净的），而不是图省事塞在后面。
- ⚠️ **函数声明的取舍**：`render_problem` 定义在调用之前 → 不需要前置声明；
  `handle_request` 定义在 `worker()` 之后 → **必须有**。删声明时把两条一起删，编译立刻报
  `'handle_request' was not declared in this scope`。

#### 31g. 实测（2026-10-22）

| 项 | 结果 |
| --- | --- |
| `?` 拆分回归 | `/problems?x=1`、`/problem/1?from=rank`、`/style.css?v=2` 全 **200**（改前 404） |
| 列表（admin） | **17 行**；`?user=alice` → **1 行** |
| 学生越权筛选 | `?user=testuser` → **0 行** |
| 倒序 | 新提交排第一 |
| 空状态 | 200 +「还没有提交记录」 |
| 转义 | `?user=<b>x</b>` → `&lt;b&gt;x&lt;/b&gt;` |
| 源码入库 | 提交 130 字节（含 `'` `"` `\` 中文换行）→ 库里 **130 字节，`cmp` 逐字节相同** |
| 题目页预填 | 回题目页取回同一份 **130 字节**；交第二次后取到**最新**那份；他人/未登录为空 |
| 结果页源码 | 130 字节逐字节相同；老提交显示「（老数据·无源码）」 |

#### 31h. 本步留下的缺口

- ⚠️ **`LIMIT 100` 没实测**（要造 101 条才能看见截断），只核了 SQL。
- ⚠️ **列表没有翻页**；`created_at` 秒级精度导致同秒排序只能靠 `job_id` 兜底（`FIX-16`）。
- ⚠️ **源码无长度上限** —— `MEDIUMTEXT` 能装 16MB，但没有任何提交前检查。
- ⚠️ **需求 `OJ-REC-01` 的「是否对他人可见可配置」没做** —— 目前固定：本人 + 管理员可见。

**下一步**：~~`/rank`~~ → 已走完，见 §32。

### 32. 排行榜 + Redis 缓存（2026-10-22）

需求对应：`OJ-RNK-01`。也是二期代码里 **Redis 第一次上场**（一期 Flask 早就用了）。

#### 32a. 为什么排行榜适合缓存

榜单是**派生数据**：每一个数字都能从 `submissions` 重算出来。所以：

- 丢了**不是灾难**，重算一遍就有；
- 但它要 `GROUP BY` 全表，**每次请求都算一遍很浪费**。

→ 这就是"缓存"最典型的对象：**读多、写少、能重算、偶尔旧一点没关系**。

#### 32b. hiredis 的回复类型（决定缓存逻辑怎么写）

先写了一个一次性探针 `redis_probe.cpp`（跑完已删），实测：

```
PING(空参数)  type=1 str=[]         ← STRING，不是 PONG
SET           type=5 str=[OK]       ← STATUS
GET           type=1 str=[hello]    ← STRING
GET 不存在的键 type=4 str=[(null)]   ← NIL
DEL           type=3 integer=2      ← INTEGER（数字在 integer，不在 str）
SET "" 再 GET  type=1 str=[]         ← 空串也是 STRING
```

**最要命的一条**：`NIL(4)` = 没缓存；`STRING(1) 空串` = 缓存了一个空榜。
**只看 `str` 是不是空会分不出这两件事** → 空榜每次都被当成 miss，缓存等于没做。
代码里判断命中写的是 `r->type == REDIS_REPLY_STRING`，不是 `r->str != nullptr`。

另外两条规矩：

- **每个 `redisReply` 都要 `freeReplyObject`**（同 `mysql_free_result`）；
- **`redisConnect` 默认没有超时**，必须用 `redisConnectWithTimeout`（这里 200ms）——
  否则 Redis 一挂，请求卡在连接上，"降级"根本来不及发生。

#### 32c. 键名冲突：一期已经占了 `oj:rank`

一期 Flask 和二期 C++ **共用同一台 Redis**。一期的键叫 `oj:rank`，存的是 **JSON**；
二期的格式是 `用户名<TAB>题数` 的文本。**同一个键、两种格式** → 双方解析都失败、互相覆盖，
各自的"缓存"变成每请求都白查一次库。

→ 二期用 **`oj:rank:v2`**。**同一台 Redis 是共享命名空间，这是 Redis 没有"库表"的直接后果**
（决策 31 讲过的那个代价）。

#### 32d. 缓存读写：失败一律当"没有"

```
rank_cache_get()   → 连不上/超时/NIL/格式坏 → false（去查库）
rank_query_db()    → 查库失败 → 页面显示「排行榜暂时读不出来」
rank_cache_set()   → 写失败就失败，反正数据已经拿到了
```

- **读写缓存都不许把异常抛给页面** —— 需求原话：「缓存不可用 → 降级为直接查询数据库，
  **不得因缓存故障导致页面不可用**」。
- 页面上有一行「数据来源：Redis 缓存 / 直接查数据库」—— **把降级变成看得见的东西**
  （演示时直接 `systemctl stop redis-server` 就能看见）。

#### 32e. 序列化格式与它的局限

缓存里存的是 `用户名<TAB>题数\n` 的纯文本，不用 JSON 库。

- 读的时候用 `string(r->str, r->len)` —— **带长度**，不靠 `\0` 判断结尾（二进制安全）。
- ⚠️ **用户名里如果有 TAB 或换行，格式会被撑坏**（当前注册只限制长度，没限制字符）。
  这是这份"自制序列化"的已知代价，真要修：要么限制用户名可用字符，要么改用 JSON。

#### 32f. 什么时候让缓存作废

`finish_job()` 写完库之后：

```cpp
if (verdict == "AC") rank_cache_invalidate();
```

**只有 AC 会让榜单变化**；WA/TLE/CE 不改变「谁通过了哪题」，删了纯属浪费。
另外还有 60 秒 TTL 兜底 —— 即使漏了某条失效路径，最多旧 60 秒。

#### 32g. 实测（2026-10-22）

| 场景 | 结果 |
| --- | --- |
| 首次访问 | 「直接查数据库」，`oj:rank:v2` 出现，**TTL=60**，`strlen=0`（空榜） |
| 第二次访问 | 「Redis 缓存」—— **空串缓存也命中**（type=1 不是 NIL） |
| 同一题 AC 两次 | 只算 **1 题**（`COUNT(DISTINCT problem_id)`） |
| 排序 | 题数降序；同分按用户名升序（`<b>hack2` 排在 `rank_b` 前） |
| 转义 | 用户名 `<b>hack2` 显示为 `&lt;b&gt;hack2` |
| AC 之后 | 键**立刻消失**（`EXISTS=0`），下次访问重算 |
| **停掉 Redis** | `/rank` 仍 **200**，2.5ms，显示「直接查数据库」，数据正常 |
| 恢复 Redis | 自动回到「Redis 缓存」 |

#### 32h. 本步留下的缺口

- ⚠️ **缓存击穿**：过期瞬间多个请求同时查库 —— 课程尺度无所谓，生产要加互斥或逻辑过期。
- ⚠️ **单键全量**：榜单一次全读全写，100 名以外看不到（`LIMIT 100`）。
- ⚠️ **没用 ZSET**：真实排行榜用 `ZADD`/`ZREVRANGE` 可以做到「只取前 N、自动排序」；
  这里为了与一期对齐，缓存的是算好的整表。
- ⚠️ **用户名里的 TAB/换行会破坏缓存格式**（见 32e）。

**下一步**：二期还差一期的 **`/admin/*` 管理功能**（题目增删改、用户管理）——
这是 `CH-11`「功能需求不变」剩下的最后一块；此外还欠与一期 Flask 的对照压测。

### 33. 工人数实验：4 个到底少不少（2026-10-22）

#### 33a. 动机

有人问「**4 线程会不会太少**」。查了机器：**`nproc = 2`** —— 4 个 HTTP 工人已经**超过核数**。
但"超过核数"不等于"一定错"：工人大部分时间在**等**（MySQL、Redis、判题子进程），
不是在算。所以问题应该用数据回答：**加到多少线程开始没有收益？**

#### 33b. 方法

- **不改主代码**：复制一个变体 `server_var.cpp`，端口和工人数从 `argv` 读，跑完删除（同 a-3 那套）。
- **三个场景**，分别对应三种负载：
  | 场景 | 负载类型 |
  | --- | --- |
  | `GET /style.css` | 几乎不干活，测调度开销 |
  | `GET /problems` | 等 MySQL，快 I/O |
  | `POST /login`（**故意错密码**） | scrypt 校验，纯 CPU；且失败不写会话表 |
- **负载**：`/style.css`、`/problems` 每轮 40 请求（`/problems` 另跑 200 请求大样本）、`/login` 每轮 40 请求；**并发 8**；每档跑 3 轮取中位数。
- 压测脚本：Python 线程 + `http.client`，每次新建连接。

#### 33c. 结果（中位数）

| 工人数 N | 登录 吞吐 / p50 | `/problems` 吞吐 | `/style.css` 吞吐 |
| --- | --- | --- | --- |
| **1** | **8.2 /s，974 ms** | 1702 /s | 1900 /s |
| **2** | **15.8 /s，494 ms** | 1790 /s | 1860 /s |
| **4** | 16.0 /s，488 ms | 1704 /s | 1775 /s |
| **8** | 15.8 /s，499 ms | 1744 /s | 1904 /s |
| **16** | 15.5 /s，497 ms | 1699 /s | 1900 /s |

#### 33d. 三条结论

1. **CPU 密集负载在 2 个工人时到顶**：1 → 2 吞吐翻倍（8.2 → 15.8），2 之后再增加**零收益**。
   正好等于核数：每个工人一个核，`scrypt`（N=32768）是纯 CPU。
2. **快请求的瓶颈不在工人数**：静态页和查库在 N=1 和 N=16 之间都在 1700~1900 之间晃。
   因为 **accept / 读请求 / 派活全在主线程一个人身上**，主线程的 epoll 循环约 **1800 次/秒** 封顶。
   工人再多也只是在后面排队。
3. **什么时候才需要更多工人**：**单请求会阻塞很久**的时候（同步等判题、大文件上传、慢下游）。
   我们的 `/submit` 是**入队即返回**，判题另有 2 个工人，所以 HTTP 工人不会被长时间占住。

#### 33e. 4 这个数字怎么定

- **2**：吃满 2 核，CPU 密集场景够用；
- **4**：留一点余量（有人登录时还能同时接别的请求），相对 8/16 **没有性能损失**；
- **8/16**：只增加线程切换和 MySQL 连接数（`thread_local` 每工人一条）。

→ **4 是有依据的中间值，不是拍脑袋。**

#### 33f. 实验的诚实边界（写报告要带上）

- 压测程序和服务**在同一台 2 核机器上**，互相抢 CPU —— 绝对吞吐偏低，但**横向比较 N 仍然有效**；
- 变体用 `-O2` 编译（正式版命令没有 `-O2`，但所有 N 用同一份二进制，比较公平）；
- 走回环网络，没有真实网络延迟；
- 数据库只有十几条数据，MySQL 侧接近无压力。

### 34. 代码编辑器 CodeMirror 5（2026-10-22）

**动机**：裸 `<textarea>` 没有行号、没有语法高亮、Tab 键会跳走 —— 自己用了一次就受不了。

**做法**：

- 资源放 **`www/vendor/codemirror/`**（不走 CDN，断网可用）：核心 JS/CSS + `clike` 模式（C++）+ 自动补括号 / 括号匹配 + 深色主题。
- `render_submit_form()` 在 `</form>` 后追加 4 个 `<script>`，用 `CodeMirror.fromTextArea()` 接管。
- **后端零改动**：CodeMirror 提交时会自动把内容写回原 `<textarea>`。
- **失败兜底**：`typeof CodeMirror === 'undefined'` 时直接 return，页面退回裸 textarea，不会白屏。
- 深色模式用 `matchMedia('(prefers-color-scheme: dark)')` 选 `material-darker` 主题。

**实测**：5 个资源全部 **200**，`Content-Type` 正确（js / css）；题目页引用 **6 处**；原有预填代码正常显示在编辑器里；提交判题流程未变。

⚠️ `www/vendor/` 仍在静态文件白名单内（`realpath` 前缀检查覆盖整个 `www/`），没有新开口子。

### 35. `/admin/*` 管理功能（2026-10-23，`25e72ea` / `c9155dd` / `1e12f2a`）

`CH-11`「功能不变」的最后一块，也是二期里**唯一同时碰「数据库」和「磁盘文件」的地方**。

五个子步：① `/admin/problems` 列表 + 守卫；② `/admin/problem/new`；③ `/admin/problem/<题号>/edit`（题面 + 测试点区）；④ 删除（POST 软删除）；⑤ `/admin/users` 用户管理。

#### 35a. 整棵 `/admin/` 子树一个门卫

```cpp
bool admin_guard(int conn_fd, const string &current_user)   // true = 放行
```

`false` 表示**已经替你把响应发了**（302 或 403）。C++ 没有 Flask「`return response` 就交给浏览器」的约定，守卫只能自己 `send`。

路由上只有一个口子 `path.compare(0, ADMIN_PREFIX.size(), ADMIN_PREFIX) == 0`，新页面加在内部派发里 —— **权限检查只写一次**。

#### 35b. 新建：`is_published` 写死 0

「无测试点不许发布」不靠新建页检查，而是**让这条路根本发不出去**（同 6b 第 1 条，表单上也没有勾选框）。发布这个动作只存在于编辑页，那里才有测试点可校验。

#### 35c. 测试点：一期的 `getlist` 在 C++ 没有对应物

表单提交的是 N 个 `tc_in` / N 个 `tc_ans`。一期的 `form.getlist("tc_in")` 一次拿回一个列表；二期的 `parse_form()` 是 `map<string,string>`，**同名 key 只留最后一个 → N 组测试点会变成 1 组**。

新增 `parse_form_multi()` → `map<string, vector<string>>`，两个列表按**出现顺序**天然配对 —— 不用给每组起名字，所以加组删组都不用改编号。

#### 35d. 落盘 + 重编号 + 事务

- 先 `remove_all(testdata/<题号>)` 再写：只覆盖前 N 个的话，旧的 `3.in` 还躺着，判题器把它当「还有下一组」；
- 服务端统一重编号 1..N；「两个框都空」= 删掉这一组；
- 元信息 `DELETE` + `INSERT` 裹 `START TRANSACTION` / `COMMIT` / `ROLLBACK`（不裹的话，DELETE 完 INSERT 挂了 = 这道题的测试点元信息整个消失）；
- **先落盘、再改库** —— 磁盘是真相、库是账本（同 6b 第 4 条）。

#### 35e. 用户管理：一条规则兜住两条需求

「**不能对自己操作**」⇒ 「系统至少保留一个可用管理员」：要降谁，必须由**另一个**管理员点，那一刻场上至少两个活跃管理员。所以不用再写一条「数人数」的规则（代码里留了一道显式兜底，按现有路径触发不了）。

其余三条：路由一律用 `user_id`（用户名可能含空格）；重置口令**不要求原口令**（遗忘口令由管理员重置）；禁用只挡**新登录**、不删行，历史提交全留着 —— 但**不会踢掉已登录的会话**（`FIX-24`）。

#### 35f. 实测（全过）

- 建题 → 配 2 组测试点 → 判题 **`all 2 test passed` / AC 2/2**；
- 勾发布但一个测试点都没有 → **400**「请先填好测试点」；
- 3 组删中间那组 → 盘上只剩 `1.*/2.*`，`2.in` 变成原第 3 组的 `3 3`，**旧的 `3.in` 没残留**；
- 删除：GET **405** / POST 302 / 已删题再编辑 **403** / 学生题库看不到 / **历史提交仍在**；
- 用户管理 10 项：重置口令后**旧口令失败、新口令成功**；禁用后登录被拒、解禁恢复；授予管理员后**下一次请求就能进管理页**（权限每次现查库）；对自己操作 **400**；GET 操作 URL **405**；不存在的用户 **404**；学生访问 **403**。

#### 35g. 过程中三处机械错（我直接在服务器文件上修的）

重复的 `normalize_newlines`、`update_problem` 用到尚未声明的 `field_value`、管理分支外层少一个 `}`。

⚠️ **花括号不配对会一路报错到文件末尾**：IntelliSense 把 `else if (path == "/whoami")` 报成「应输入一个语句」，后面全是级联错误；真正的原因只是前面少了一个 `}`。**看报错要看最靠前的那一条。**

### 36. 对照压测：一期 Flask vs 二期 C++（2026-10-23）

**方法**：同一台 2 核 VPS、回环网络、同一份 Python 线程压测脚本（每次新建连接）。GET 类 400 请求 / 16 并发，登录 80 请求 / 8 并发，各跑 3 轮取中位。C++ 变体用 `-O2` 且与原版同 flags（生产二进制编译时没有 `-O2`），跑完即删 —— 同 a-3 / §33 的做法。

| 场景 | 一期 Flask | 二期 C++（4 工人） | 二期 C++（1 工人） |
| --- | --- | --- | --- |
| `GET /`（轻请求） | 919 QPS / p50 16.8 ms | **2309 / 6.0 ms** | 2268 / 6.1 ms |
| `GET /problems`（查库渲染） | **36.3 / 437 ms** | 约 1900~2500 / 5~7 ms | 约 1900~2500 / 5~7 ms |
| `POST /login`（scrypt，纯 CPU） | 9.4 /s / 689 ms | **15.9 /s / 492 ms** | 8.5 /s / 932 ms |

**三条结论**：

1. **轻请求约 2.3 倍**：少了 Python 解释器和 werkzeug 那一层。
2. **查库页面约 50 倍，根因不是语言，是连接**：一期每个请求新建一条 MySQL 连接（§28c 量到 57 ms），16 并发全卡在握手上；二期 `thread_local` 每工人一条常驻连接，0 次握手。**压测把 §28d 那个决定量出来了。**
3. **只有 CPU 密集的场景吃线程数**：登录 1 工人 8.5/s → 4 工人 15.9/s（1.9 倍，恰好 2 核封顶）。一期 9.4/s 也是被「每请求一次连接」拖住的。

**交叉验证（别把噪声当结论）**：第一次主表里 1 工人 `/problems` 比 4 工人还快，怀疑是噪声 → 又做了 5 轮交替对照（4/1 交替、各 400 请求 8 并发）：单轮在 **1600~3000** 之间乱跳，两边中位都是 **~2500**。结论：**快请求不受工人数影响**（与 §33 一致），主表那个差值是客户端抢 CPU 的抖动。

**诚实边界**：压测程序与服务同机（2 核互抢，绝对吞吐偏低、横向对比有效）；回环无网络延迟；库里只有 1 道题；Flask 是开发服务器不是 gunicorn。

### 37. ⭐ 统一日志（`FIX-07`，2026-10-25）

**背景**：`OJ-SYS-03` 要求「记录事件类型、时间、涉及用户与提交号、结果；输出包括判题耗时、峰值内存」，
`OJ-JDG-09` 要求「系统错误时记录日志供排查」，`OJ-JDG-08` 要求「队列长度与平均等待时间可观测」。
此前只有零散的 `cout` / `cerr`，没有文件、没有级别。

**做法**：独立模块 `log.h` / `log.cpp`，四个接口：

| 接口 | 职责 |
| --- | --- |
| `log_init(path, min_level)` | 建目录、打开文件（追加）、设阈值；文件打不开返回 `false` |
| `log_level_from_env()` | 读 `LOG_LEVEL`，认不出回退 `INFO` |
| `log_write(level, event, detail)` | 一行一条：`时间.毫秒 [级别] [tid=N] event key=value...` |
| `log_sanitize(s)` | 把 `\n` / `\r` / `\t` 转成字面量 |

**五个设计点**：

1. **锁保护的是共享数据，不是代码块**：拼行全用局部变量（`tv` / `tm_buf` / `msg`），互不干扰；
   `lock_guard` 只包住 `log_file` 的写入。如果拼行用全局缓冲区，就必须连拼一起锁。
2. **`localtime_r` 不是 `localtime`**：后者返回一块全局静态缓冲区，多线程同时调会互相覆盖。
3. **`mkdir` 失败不处理**：`EEXIST` 也返回 `-1`。先 `stat` 再建是 TOCTOU；正确姿势是直接建、错了看错误码。
4. **防日志注入**：用户名、URL、判题器输出直接拼进日志时，一个含 `\n` 的用户名能伪造出一整行假日志。
   实测反证：`log_sanitize` 缺席时，`user=evil\n[ERROR] forged line` 真的多出一行裸 `[ERROR]`。
5. **不记敏感内容**：口令、会话票号、请求体（登录表单）、用户源码一律不写。旧代码里 `cout << session` /
   `cout << body` 全部删掉 —— **日志文件是给人看的，不是给攻击者看的**。

**接入点**：`main`（startup）、`send_response`（request：方法/路径/状态码/耗时/用户/IP）、
`worker`（queue：长度/等待毫秒）、`/submit`（submit：提交号/题号/用户）、`judge_worker`（judge：结论/通过组数/
耗时/峰值内存/用户/题号）、`judge_worker` 错误分支（judge_error）、`run_judge`（judge_spawn）、
20 余处 `mysql_error` / `ec.message()` / Redis 错误（`db` / `fs` / `redis`，用 `__func__` 标明出处）。

**⚠️ 一个容易漏的点**：`send_redirect` 原本**自己拼响应直接 `write`**，不经过 `send_response` →
所有 302（登录跳转、提交后跳转）都不会进访问日志。改为复用 `send_response` 后补上。

**实测**：

```
[INFO]  startup port=8081 workers=4 judges=2
[DEBUG] queue   len=0 wait_ms=939
[INFO]  request method=GET path=/problems status=200 ms=10 user= ip=127.0.0.1
[INFO]  request method=GET path=/problems status=200 ms=16 user=logtest ip=127.0.0.1
[INFO]  submit  job=49e8e13e problem=1 user=logtest
[INFO]  judge   job=49e8e13e user=logtest problem=1 verdict=AC passed=3/3 time_ms=3 mem_kb=3584 started=1
[ERROR] judge_error job=108f7565 code=127 out=
[WARN]  judge_error job=2154c852 problem=1 reason=no_testdata
[INFO]  request method=GET path=/rank status=302 ms=0 user= ip=127.0.0.1
```

SE 与 NO_DATA 两个变体分别用「`JUDGE_BIN` 指向不存在的路径」「`TESTDATA_DIR` 指向空目录」造出来（照 a-3 的变体套路）。
日志模块自身用 10 线程 × 100 行并发写验收：1000 行不丢、内容不交错、11 个不同 `tid`。

### 38. 三条收尾缺陷：踢会话 / 请求上限 / 队列有界（`FIX-24` / `FIX-25` / `FIX-12`，2026-10-26）

三条都是**边界**问题 —— 不是功能没做，是「出事的时候没人管」。

#### 38a. `FIX-24` 禁用账号要踢会话

`is_disabled` 只挡**新登录**，手里那张票在有效期内照样能用 —— 禁用等于只关了前门。
修法与改密踢人（§30g）同款：禁用时 `DELETE FROM sessions WHERE username = ?`。

> **凡是「状态变化让某个凭证失效」的地方，都要问一句：已经发出去的那批怎么办。**
> 改密和禁用是同一件事的两次出现。

#### 38b. `FIX-25` 请求体和源码各设一道上限

| 常量 | 值 | 超了 |
| --- | --- | --- |
| `MAX_BODY_BYTES` | 1 MB | 413 |
| `MAX_CODE_BYTES` | 256 KB | 413 |
| `MAX_REQUEST_BYTES` | 2 MB | 断开连接 |

前两个是**语义上限**（告诉用户「太大了」），第三个是**内存上限**（连收都不收）。
分成两层是因为：量得出 body 大小之前，它已经在内存里了 —— 真正的防线是接收缓冲那道。

#### 38c. `FIX-12` 判题队列有界

需求 `OJ-JDG-08` 原文：「队列**有界**：超出容量时拒绝新提交并明确提示，而不是无限堆积耗尽内存」。
原来那句 `queue<JudgeTask> judge_queue;` 没有任何上限。

**三个决定**：

1. **容量判断和 `push` 必须在同一把锁里。**
   写成「锁外判容量、锁内 push」会有检查-使用竞态：两个 HTTP 工人同时看到 `size() == 63`，
   各自判定「还有一位」，然后一前一后 push，队列变成 65。
   > 决策 27「先问这东西谁在碰」的又一次应用 —— 只不过这次抢的不是内存，是**最后那个位子**。

2. **满了要回滚，而且回滚有两样东西。**
   现有顺序是「先 INSERT（`status='排队中'`）→ 再 push」。队列满直接 return 会留下：
   - 库里一条**永远排队中**的孤儿记录（没人判、没人更新）
   - `/tmp/oj_XXXXXX` 一个**没人删**的目录

   所以满了要 `remove_all(work_dir)` + `DELETE FROM submissions WHERE job_id = ...`。
   > **一个动作有一处要撤销，它就有几处要撤销。**（同 §28e 那两个 `mysql_close`）

3. **503 要自己补 reason。** `send_response` 那条 if-else 链不加分支就是 `HTTP/1.1 503 Unknown`
   —— 决策 8 记过完全相同的现场。顺带补 `Retry-After: 5`，这是 503 的标准搭档。

**线程数可配**（同一条需求的第二句）：读环境变量 `OJ_JUDGE_THREADS`，范围 1–32，默认 2。

- **默认仍是 2 而不是需求建议的 8～16** —— §29d 那个取舍（每个判题进程要 fork g++ + 测试点子进程）。
  需求与现实有差距时：**代码支持配到 16，默认保守取 2，把理由写进注释**。
- ⚠️ **`stoi("4abc") == 4`，不抛异常** —— 它只看前缀。必须用 `stoi(s, &pos)` 再查 `pos == strlen(s)`
  才叫「整串合法」。这正是 §29b `/problem/1abc` 那个「静默给错值」的同款，只是换了个入口。

**实测**（`/home/ubuntu/oj-main` worktree，端口临时改 8090，不碰线上 8080）：

| 场景 | 结果 |
| --- | --- |
| `OJ_JUDGE_THREADS=4` | `judges=4` |
| `OJ_JUDGE_THREADS=4abc` | `reason=bad_env` + `judges=2` |
| `OJ_JUDGE_THREADS=0` | `reason=bad_env` + `judges=2` |
| 未设置 | `judges=2` |
| 上限临时设 1，并发 4 份死循环 | **302 / 302 / 302 / 503**，被拒记录入库 **0** 条，`/tmp` 无残留 |
| 503 响应头 | `HTTP/1.1 503 Service Unavailable`、`Retry-After: 5`、`text/html` |

### 39. 判定顺序：TLE 优先于 MLE（`FIX-02`，2026-10-26）—— 以及一个部署环境的坑

#### 39a. 为什么 TLE 要排在 MLE 前面

原实现的判定顺序是「先量内存、再看信号」：

```cpp
if (ru.ru_maxrss > ml_mb * 1024) return "MLE";     // ← 先判内存
if (WIFSIGNALED(status)) { ... }                   // ← 后看信号
```

于是「既死循环又吃内存」的程序会被报成 **MLE**。可它其实是被 `SIGALRM` 打断的 ——
**没跑完的程序，`ru_maxrss` 只是「跑到一半时的峰值」；拿这个不完整的数下结论，是在用半截数据判案。**

改成三档：

```cpp
if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) return "TLE";   // ① 被闹钟打断
if (ru.ru_maxrss > ml_mb * 1024)                        return "MLE";   // ② 峰值超了
if (WIFSIGNALED(status))                                return "RE";    // ③ 其它信号
```

**复现（改之前）**：`while(true) { malloc(1MB); memset(...); }`，`tl=1000ms / ml=64MB`：

```
peak memory: 1046656 KB
time: 1077 ms            ← 已经超时了
Memory Limit Exceeded    ← 却报超内存
VERDICT: MLE
```

**改后同一条**：

```
time: 1066 ms
Time Limit Exceeded
VERDICT: TLE
```

**四类回归**（确认没弄坏别的）：纯超时 → TLE、纯超内存（549ms 内吃 822MB）→ MLE、
纯 RE → RE、A+B → AC。

> **判定顺序不是随便排的。** 每条结论都依赖「程序跑到了哪一步」，
> 而只有**正常退出**的程序，它的资源账本才是完整的。

#### 39b. ⚠️ apport：把 RE 的耗时凭空拉长 1.2 秒

修完回归时发现：`re.cpp` 只有 `int *p = nullptr; *p = 1;` —— **一行就崩**，
可判题器量出的 `time: 1158 ms`，而且稳定复现。

直接量：

```
正常退出的程序   real 0.002s
段错误的程序     real 1.190s     ← 差近 600 倍
```

**根因**：

```
/proc/sys/kernel/core_pattern
→ |/usr/share/apport/apport -p%p -s%s -c%c -d%d ...
```

Ubuntu 把 core dump 设成**管道模式**：进程崩溃时内核不写 core 文件，而是把现场
递给 `apport`（崩溃报告服务）。这个调用是**同步**的，父进程的 `wait4` 要等 apport
收工才返回 —— 那 1.18 秒全在等它。

**试过但无效的两条路**：

| 尝试 | 结果 |
| --- | --- |
| `ulimit -c 0` / `setrlimit(RLIMIT_CORE, 0)` | ❌ 管道模式下内核**不检查** `RLIMIT_CORE`（不写文件，就不受文件大小限制） |
| 子进程里 `exec` 之前 `prctl(PR_SET_DUMPABLE, 0)` | ❌ **`execve` 会把它重置回默认值** —— 内核加载新程序时重新设一次 dumpable，提前打的标记被冲掉 |

⭐ **第二条值得单独记**：`PR_SET_DUMPABLE` 是**进程属性**，不是 `exec` 保留的属性。
「在 `exec` 之前设置、`exec` 之后就失效」—— 与 §21 那个 `SO_RCVTIMEO` 是同一类：
**改了一个东西，却没确认它作用在哪一段生命周期上。**

在**程序内部**调才有效（实测 3 ms）：

```cpp
int main() { prctl(PR_SET_DUMPABLE, 0); int *p = nullptr; *p = 1; }   // 3 ms
```

但判题器没法替用户程序做这件事 —— 它只能在 `exec` 之前设，而 `exec` 会冲掉。

**结论：代码层面改不了，这是宿主机配置问题。**

- 判定结论**不受影响**：杀它的是 `SIGSEGV` 不是 `SIGALRM`，§39a 改完顺序后依然是 RE
- 受影响的是**结果页的耗时数字**（显示 ~1150ms）与**每次 RE 白等 1.2 秒**
- 修法：把 `core_pattern` 改回普通文件模式（`sysctl -w kernel.core_pattern=core`），
  此时 `setrlimit(RLIMIT_CORE, 0)` 才管用；或直接关掉 apport
- **当前未改**，如实标注在需求文档 `OJ-JDG-03`

⭐ **但这条只在「宿主机直接跑 judge」时成立。** 部署分支把判题放进容器，
容器里根本没有 `apport` 这个程序，同一份 `re.cpp` 在容器内只量到 **201 ms**
（对比宿主机的 1158 ms）。所以：

| 运行方式 | RE 的 time |
| --- | --- |
| 宿主机直接跑（`main` 分支开发 / 对照实验） | **1158 ms** |
| 容器内跑（`feat/public-deploy`） | **201 ms** |

> **同一个二进制、同一份源码，换个运行环境结论就差 5 倍。**
> 量到异常数时，先问「这个数是在哪跑出来的」。

### 40. 管理页三条缺陷 + 两个「小尾巴」：CSRF / 原子落盘 / 乐观锁 / 时间精度 / 缓存格式（`FIX-19` / `20` / `21` / `16` / `23`，2026-10-27）

这五条都落在同一条路上（管理员编辑题目 → 落盘 → 改库），一起改、一起验。

#### `FIX-19` CSRF：令牌为什么不能放 Cookie

- `sessions` 表加 `csrf_token CHAR(32)`，**登录时和票号一起签发**。
- 所有管理表单带隐藏字段 `<input type="hidden" name="csrf_token">`；`/admin/*` 的 **POST** 进门先比令牌，不匹配 **403**。
- **为什么不是「再发一个 Cookie」**：CSRF 的前提就是「浏览器会自动带上你的 Cookie」。别的站点让浏览器向本站发请求，票会自动带上 —— 你再发一个令牌 Cookie 也一样会被带上，等于没防。**令牌必须放在页面内容里**：跨站请求发得出去，但**读不到你页面的 HTML**。
- 比对用 `CRYPTO_memcmp`（常数时间）—— 和口令哈希那边同一个理由：不让攻击者靠响应时间逐位试。
- **为什么先只覆盖 `/admin/*`**：`/profile` 改口令要求原口令（攻击者没有）；`/submit` 最坏是被冒名交几份代码；`/logout` 只是登出。**管理操作才是「不喊一声就能删题」的那类**。登录本身的 CSRF（把你登进攻击者的账号）留作已知边界，写进文档。
- **部署时的坑（已修）**：线上部署分支是旧代码，它登录时不会写 `csrf_token`。新代码读到空令牌 → 管理页表单全空、怎么刷新都 403。**`get_session_csrf()` 遇到空令牌时当场补一个并写回**（自愈），不需要把所有人踢下线。

#### `FIX-20` 落盘原子化：先写临时目录，再 `rename`

- 旧写法 `remove_all(dir)` → 逐个写：写到一半失败，这道题**一个测试点都不剩**，已发布的题变成「交上来一律不通过」。
- 新写法：写 `testdata/.tmp-<题号>-<pid>-<序号>` → 旧目录挪成 `.old-*` → **`rename(tmp, dir)`** → 删旧目录。
- **`rename` 在同一文件系统内是原子的** —— 判题器要么看到旧的整套、要么看到新的整套，不存在「看到一半」。
- 失败路径：写临时目录失败 → 删临时目录、旧目录**原封不动**；`rename` 失败 → 把 `.old-*` **放回去**再清理。
- **实测**：正常保存 2 组 → 目录只有 `1.*/2.*`；把测试数据根目录 `chmod 555` 再保存 → **500，旧目录内容原样还在**（`9 9`）。临时目录名带 `pid + 原子序号`，同进程两个管理员同时保存也不会撞名。

#### `FIX-21` 丢失更新：版本号要在**动磁盘之前**比

- `problems` 加 `version INT NOT NULL DEFAULT 1`；编辑页把它放进隐藏字段；保存时 `UPDATE ... AND version = ?`，成功后 `version = version + 1`。
- **`mysql_affected_rows() == 0` = 冲突**。因为版本号每次必变，成功时不可能报 0。
- **顺序是这条的关键**：现有设计是「先落盘、再改库」（磁盘是真相）。如果先落盘再发现冲突，**磁盘已经被覆盖了**。所以：**打开事务之前**先用表单里的版本号和刚读出来的 `p.version` 比一次 → 写盘 → 再带 `WHERE version` 兜一次。
- **残留窗口（已写日志）**：两次比对之间（毫秒级）另一个管理员刚好提交 → `UPDATE` 影响 0 行，但磁盘已是新内容。如实返回 409 并记 `conflict=race_after_disk`，不假装成功。
- **实测**：版本 1 首次保存 → 302、版本变 2；拿旧版本 1 再保存 → **409**；乱填版本 3 → **409**；版本号不变。

#### `FIX-16` 时间精度：`DATETIME(6)` + 显示只到秒

- `submissions.created_at` 改 `DATETIME(6)`，写入用 `NOW(6)`。
- **排序用完整精度、显示用 `DATE_FORMAT(..., '%Y-%m-%d %H:%i:%s')`** —— 微秒只是用来定序的，人不用看。
- **实测**：塞两条同一秒、微秒相差 1 的记录 → `ORDER BY created_at DESC` 顺序正确；列表页显示仍到秒。

#### `FIX-23` 榜单缓存：用长度前缀，不用 TAB

- 旧格式 `用户名<TAB>题数\n`：注册只限用户名长度、不限字符，**用户名里有 TAB 或换行，整份缓存错位**。
- 新格式（键升到 `oj:rank:v3`）：`<字节数>:<用户名>:<题数>\n`。解析时**按长度精确取字节**，用户名里有什么字符都无所谓。
- **这条的普适教训**：**长度是事实，分隔符只是约定。** 拼文本协议时，只要其中一段来自用户，优先用长度前缀而不是「找个他大概不会用的字符」。
- **实测**：往 Redis 塞 `3:a<TAB>b:7\n` → 页面正确显示 `a·b / 7`（缓存命中）；塞 `garbage` → **降级查库**，页面标注「直接查数据库」。

#### 三个推演（写报告 / 面试可直接用）

**① CSRF 为什么令牌必须放页面、不能放 Cookie**

攻击者的能力边界：

| 能 | 不能 |
|---|---|
| 让浏览器向本站发请求（表单、图片） | 读本站页面的 HTML / 响应体（同源策略） |
| 请求会自动带上本站 Cookie | 猜出 128 bit 令牌 |
| — | 读 `HttpOnly` Cookie |

→ **Cookie 证明「你的浏览器来过」，令牌证明「请求是从我自己的页面发出的」。** 前者能借，后者借不走。
**病根是「Cookie 自动附带」** —— 再发一个令牌 Cookie 等于把同一把钥匙又配一把挂在门上。
两个边界：**XSS 会让 CSRF 失效**（能读页面就能读令牌）；`SameSite=Lax` 已挡掉大部分跨站 POST，令牌是第二道防线（老浏览器、同站子域、行为差异）。

**② `rename` 原子性：原子操作拼起来 ≠ 原子整体**

- `rename` 只改**目录项**（名字 → inode），不搬数据块 → 物理上没有「搬一半」。
- POSIX 保证**同一文件系统内**对任何观察者「要么旧名、要么新名」。跨文件系统直接 `EXDEV`。
- **实测**（2026-10-27，服务器上）：
  - `python3 os.rename('/dev/shm/xtest', '/home/ubuntu/xtest_moved')` → `Errno 18 Invalid cross-device link`
  - `mv` 同一条命令**退出码 0** —— 因为它检测到 `EXDEV` 后自动降级成「复制 + 删除」，而这正是不原子、会写一半的动作。
- **现状的诚实边界**：目录不能被 `rename` 直接覆盖（`ENOTEMPTY`），所以用两步「旧挪走 → 新放回」，**两步之间有几微秒 `testdata/<题号>` 不存在的空窗**，判题器恰好此时来读会 `NO_DATA`。彻底解法是 `renameat2(..., RENAME_EXCHANGE)`（原子交换两个目录，Linux 3.15+）。课设未采用，如实记下。
- ⚠️ **这台机器上试不出 `/tmp` 的坑**：`findmnt` 显示 `/tmp` 与 `testdata` **同在 `/dev/vda2` ext4**，临时目录放 `/tmp` 会「碰巧正常」；换一台 `/tmp` 是 tmpfs 的机器立刻 `EXDEV`。**正确性不能依赖「/tmp 恰好和代码同盘」。**

**③ `FIX-23` 的注入场景（不是「缓存坏了」那么温和）**

注册只限用户名长度、不限字符。用户名填 `bob<TAB>99`：

```
缓存行:  bob	99	3
解析:    name = line.substr(0, 第一个TAB) = "bob"
         count = stoi("99\t3") = 99        ← stoi 遇到 TAB 就停
```

→ 排行榜显示 **bob：99 题**，一道题没多做就能刷到第一。用户名填 `x\nadmin\t999` 还能**凭空注出一条假记录**。
**修法二选一**：限制用户名字符（产品决策，且文档明确允许空格等字符）或改格式（只动内部）→ 选了后者。
**普适教训：长度是事实，分隔符只是约定。** 文本协议里只要有一段来自用户，优先用长度前缀（JSON 转义、`Content-Length`、Protobuf varint 都是同一个思路）。

#### 回归与清理

- 编译 `-Wall -Wextra` **零警告**；管理页 GET 正常、POST 无令牌/错令牌 **403**、正常令牌 302；用户管理三个操作同样受保护；删除接口无令牌 403 且**没有落库**。
- 测试用临时题目 `problem_id=7`、两条假提交、测试会话、Redis 测试键、`/tmp/oj-fix-testdata` **全部清干净**；线上 8080 与 production testdata **未触碰**（测试实例跑 8090、`TESTDATA_DIR` 指到 `/tmp`）。

### 41. 公开部署分支：判题进容器（2026-10-23，`feat/public-deploy`）

从「回环自用」走到「别人能用」时先摸出两个事实：

- **外网打不通**（`http://公网IP:8080` 超时）= 腾讯云安全组没放行，不是代码；
- **判题子进程和服务器同一个 `ubuntu` 用户、没有网络隔离** → 任何能注册的人交一段 C++，就能在服务器上以 `ubuntu` 身份读到 `~/.ssh/id_ed25519` 和 `db_config.h`，还能联网发走。

所以先给答辩版钉标签 `v1.0-course`，公开部署的改动全部放在分支 **`feat/public-deploy`**（不要求合并回 main）。

#### 41a. 为什么用 Docker，而不是 setuid / chroot

自建隔离要 `unshare(CLONE_NEWNET)`（要 CAP_SYS_ADMIN）、自己维护 chroot、还得防漏；容器一条命令给齐：

```
--network=none --read-only --cap-drop=ALL
--pids-limit=128 --memory=1g --cpus=1
-u 1000:1000 --security-opt no-new-privileges
```

**实测（容器内，5 条）**：

| 尝试 | 结果 |
| --- | --- |
| 读 `/etc/shadow` | Permission denied |
| 读宿主机 `~/.ssh/id_ed25519` | No such file or directory（根本没挂进去） |
| 写 `/etc/hostname` | Read-only file system |
| 连 `1.1.1.1:80` | Network is unreachable |
| 写 `/work/ok.txt` | 成功 |

#### 41b. 三个坑

1. **glibc 不匹配**：宿主机（Ubuntu 24.04）编出来的 judge 拿到 Debian 镜像里跑，报 `GLIBC_2.38 not found` → 改成**镜像里现编**（Dockerfile 里 `COPY judge_demo/judge.cpp` + `g++`）。
2. **`fstream` 默认是「读写」模式**：`std::fstream check(ans_path)` 在**只读挂载**上 `is_open()` 直接 false → 判题器以为没有测试点，报 `NO_DATA`。改 `ifstream` → AC。**这就是「同一个函数在读/写两种上下文的区别」的老课**。
3. **挂载路径必须由宿主机解析**：Docker 是 **daemon** 去解析 `-v` 的路径，服务进程眼里的 `/tmp/oj_XXXXXX` 不等于 daemon 眼里的同一个目录（尤其开了 `PrivateTmp`）→ 提交目录从 `/tmp` 挪到 `/var/lib/oj/work`。

#### 41c. 应用层加固（都在这个分支）

- **关闭自助注册**，改成管理员在 `/admin/users` 里「新建账号」（`/admin/user/new`）；
- **登录失败限流**：同一用户名 10 次 / 10 分钟 → 429；
- **提交冷却**：同一用户 3 秒内第二次 → 429；
- **体积上限**：请求体 1 MB、源码 256 KB → 413；
- **日志**：`logs/oj.log`，一行一件事（登录失败、提交、判题结果）；
- **systemd**：`deploy/oj-server.service`，开机自启、崩溃 3 秒重启，`SupplementaryGroups=docker` 让服务有 docker 权限。

#### 41d. 实测

- 端到端：提交 A+B → 容器判题 **AC 3/3**，`submissions.verdict = AC`；
- **容器启动 ~0.45 s**；判一道 3 组测试的题 **~0.85 s**（需求 ≤ 3 s）；
- 提交冷却：第一次 302、第二次 **429**；
- 登录失败：第 11 次 **429**；
- 请求体 1.1 MB → **413**；源码 300 KB → **413**；
- 管理员建号 → 能登录；重名 400；GET 405；学生 403；
- `systemctl restart` 后 `/problems` 200、提交正常。

#### 41e. 仍未做

- **HTTPS**（没有域名/备案；见 `deploy/DEPLOY.md` 的说明）；
- `FIX-19`（CSRF）/ `FIX-20`（落盘原子性）/ `FIX-21`（丢失更新）/ `FIX-24`（禁用不踢会话）；
- 安全组放行 8080 **必须手动在腾讯云控制台做**。

### 42. 视觉重做：从「AI 生成的落地页」回到「工具」（2026-10-23）

同学看完站点的原话是「AI 味道太重」。回看当时的 `style.css`，确实每一条都是生成式界面的默认值：
hero 大标题 + 假的代码窗口 + 顶部网格渐变、所有内容切成**同样的圆角卡片 + 同样的软阴影**、
首页三块大数字统计、悬停上浮 + `→` 箭头、Bahnschrift 显示字体 + 字距收紧。

这一版把参照系换成**洛谷 / 力扣**（同学日常真在用的工具），原则只有一句：
**靠字号、密度和边框分层，不靠卡片、阴影和动效。**

#### 42a. 具体动了什么

| 原来 | 现在 |
| --- | --- |
| hero + 假代码窗 + 网格渐变 + `rise` 入场动画 | 一行标题 + 一行说明 + 一条数字，页面直接进内容 |
| 三块「大数字 + 小标签」统计卡 | 一行 `7 道题 · 42 次提交 · 5 位选手`（`·` 也去掉了，改成空格分栏） |
| 题库：圆角卡片 + 悬停上浮 + 箭头 | 表头 + 行，`题号 / 题目 / 时间限制 / 内存限制 / 状态` 五列，悬停只换底色 |
| 所有面板 14px 圆角 + `0 14px 36px` 阴影 | 1px 边框 + 4px 圆角，全站**零阴影** |
| 84×84 判题结果色块 | 结果条：左侧 3px 色条 + `AC / WA` 等宽字 + 行内统计 |
| 胶囊 chip 显示时间/内存 | 标题下一行 `时间限制 1000 ms` 纯文字 |
| 16px 正文 / 34–46px 标题 | 14px 正文 / 22px 标题（表格 12.5–13px） |
| 蓝色主按钮 + 蓝底导航 pill | 近黑主按钮；导航当前页用**下划线**，不用底色 |

#### 42b. 留下的取舍

- **编辑器保持深色**（`#161b22`），不跟随页面明暗 —— 页面上唯一一块深色，也是唯一的记忆点；
  CodeMirror 的 theme 从「亮色用 default」改成**始终 `material-darker`**，和兜底 textarea 一致。
- **暗色模式保留**，只是把蓝紫调换成中性灰（`#0d1117` / `#161b22`）。
- 首页不再是营销页，改成**入口目录**（题库 / 排行榜 / 我的提交 / 回显 / 关于），
  `{{problems}}` / `{{submissions}}` / `{{users}}` 三个占位符照旧由 `handle_request` 现查替换。
- 移动端顶栏折成两行：品牌一行，导航一行**横向滚动**（9 个入口在 390px 下放不开）。

#### 42c. 改到的文件

- `www/style.css` 整份重写（设计令牌 → 组件，无 `--shadow`、无 `keyframes` 入场动画）；
- `server.cpp`：`render_problems` 换表格骨架、`render_problem` / `render_result` 的 chip 改 `meta-line`、
  结果页把「提交的源码」挪到判题输出之后、管理页标题输入框去掉 `size=60`；
- `www/index.html` 重写；`about/echo/hello` 只改了图标和导航里的「登录 / 注册」拆成两个链接。

#### 42d. 怎么验的

Linux 侧才能编译，本地没有浏览器可跑真服务 —— 所以做法是：
**把 `render_*` 吐出来的 HTML 原样抄成静态页**（`%TEMP%/oj-preview`），再打本地服务用
Playwright（`--channel msedge`，不额外下载浏览器）截 1280 / 390 两档、亮暗两色对照。
CSS 与真实页面共用同一份 `style.css`，所以布局问题能提前看见 —— 移动端导航挤成竖排就是这一步发现的。
`server.cpp` 只做了 `g++ -fsyntax-only -Wall -Wextra`（在 VPS 上，带 `mysql_config --cflags`），**没做端到端**。

## 实测数字汇总

| 项 | 数字 |
| --- | --- |
| 请求总长（一例） | 头部 67 + body 20 = **87** 字节 |
| `string(buf)` 走 C 字符串路的后果 | 14 字节的请求被读成 **100** 字节 |
| `headers["Content-Length"]` 读一次 | map 从 2 条变 **3** 条 |
| `find('about')` 多字符字面量 | 算成 `1651471732` → 截成 char = **116 = `'t'`** |
| HTML 字面量 → 文件 | **334** → **203** 字节（差 131 = 132 缩进 − 1 换行） |
| 双 `write` 暴露 `else` 绑定错误 | 收到 2 个响应：**90 + 111** 字节 |
| 漏 `close` 的 fd 泄漏 | 15 个请求 → fd **4 → 19**；补上后 65 个请求仍是 **4** |
| `www/` 各文件 | index.html **250** / about.html **220** / hello.html **228** / style.css **259** |
| 404 正文 | **25** 字节（「请输入正确的网址」+ 换行） |
| `/..` 在黑名单方案下 | **200**（漏网） |
| 前缀检查漏 `/` 的后果 | `.../www2/x` 被**放行** |
| `/echo` 表单页 | **387** 字节 |
| 首页加一个链接 | 250 → **290**（+40） |
| `html_escape` 前后（`name=<b>Y</b>`） | 裸 `<b>Y</b>` → **`&lt;b&gt;Y&lt;/b&gt;`** |
| `'&amp'` / `'&lt'` / `'&gt'`（多字符字面量） | `643919216`→**`'p'`** / `2518132`→**`'t'`** / `2516852`→**`'t'`** |
| `'&'+'a'+'m'+'p'+';'`（整数加法） | **415** → 截成 char = **-97** |
| 止血后：空连接挂 10s、1s 后打 curl | `time_total = ` **4.036**（≈ 5 − 1） |
| URL 解码前 / 后：`/hello%2Ehtml` | **404 25** → **200 228** |
| 黑名单（检查在解码前）：`/%2e%2e%2fserver.cpp` | **10067** 字节 = 服务器源码 |
| 黑名单（检查在解码前）：`/%2e%2e%2f%2e%2e%2fweb/db_config.py` | **201** 字节 = DB 密码 |
| 白名单：上面两条 | **25 / 25**（全拦） |
| `/..` 打两版黑名单 | **0**（漏） |
| 路径里的 `+`：`/a+b.html` vs `/a%2Bb.html` | **404** vs **200**（文件本身 14 字节） |
| 同上，换最现实的名字：`/c++/index.html` | **404 25** → 修完 **200 18** |
| `plus_is_space` 默认参数的回归 | `/c++/index.html` **18** · `/hello%2Ehtml` **228** · `%2e%2e%2f` **25** · form `a+b` → **`a b`** |
| **前端重做后**（`wc -c`） | index **1338** · about **1537** · hello **1128** · echo **1426** · style.css **3784** |
| 前端重做后冒烟 | `/ 1338` · `/about.html 1537` · `/hello.html 1128` · `/echo 1426` · `/style.css 3784`（全 200） |
| **epoll 子步 2**：9000 字节的请求 | `4096 / 8192 / 9154` 三行（**电平触发**，一次没读完下一轮接着报） |
| 子步 2：连打 3 个请求 | fd 数稳定 **6**、`ss` 无 `CLOSE-WAIT`（改前同处是 `CLOSE-WAIT` + fd 一路漏） |
| 子步 2：fd 号复用 | 每次都是 **5**；累积数每次回到 **77**（不是 154）→ `erase` 生效的铁证 |
| **子步 3：半包 POST vs curl 一次发完** | 两边 `Content-Length` **都是 772** —— 逐字节相同 |
| 子步 3：粘包两个 GET | 客户端只收到 **1 个**响应（**1438** 字节） |
| `stoi("")` 不兜底的后果 | `terminate ... std::invalid_argument`，退出码 **134**（整个进程死） |
| `find` 返回 `npos` 存进 `int` | `18446744073709551615` → **-1**；`end − start` → 负数 |
| **线程池：4 工人 + `sleep(1)` + 4 个并发请求** | **1.034s** |
| 线程池：1 工人 + 同一条命令 | **4.019s**（= 加线程池之前的行为） |
| 线程池：4 工人 + 无慢活 | **0.030s** |
| 线程池：`ls /proc/<pid>/task \| wc -l` | **5**（4 工人 + 1 主线程） |
| 登录 POST（scrypt `N=32768`） | **0.128 / 0.140 / 0.150 s**；同期 `/problems` 只有 **1~9 ms** |
| 认证冒烟（2026-10-22） | 未登录 `/submit`、`/result` → **302 `/login`**；注册成功 → **302 `/login`**；重名 → **200**「已被注册」 |
| 越权三者 | 本人 `/result` **200** / 他人 **403** / 不存在 **404** |
| **查询串拆分后回归** | `/problems?x=1`、`/problem/1?from=rank`、`/style.css?v=2` 全 **200**（改前 **404**） |
| `/submissions`（admin） | **17 行**；`?user=alice` → **1 行**；学生带 `?user=testuser` → **0 行** |
| 列表里的 NULL | **6 个「游客」** + **9 条「（老数据·无题号）」**（两种 NULL 各显示各的） |
| 源码入库 | 提交 **130 字节**（含 `'` `"` `\` 中文）→ 库里 **130 字节，`cmp` 逐字节相同** |
| 题目页预填 | 回题目页取回 **130 字节**逐字节相同；交第二次取到**最新**；他人/未登录为空 |
| 结果页源码 | **130 字节**逐字节相同；老提交 `03eec3f0` 显示「（老数据·无源码）」 |
| 排行榜缓存 | 首次「直接查数据库」→ **TTL=60** → 第二次「Redis 缓存」；空榜 `strlen=0` **也命中** |
| AC 后失效 | `EXISTS oj:rank:v2` 从 **1 → 0**，下次访问重算 |
| **Redis 停掉** | `/rank` 仍 **200**、**2.5ms**、「直接查数据库」；恢复后回到缓存 |
| 工人数：登录（scrypt） | **1 工人 8.2/s（p50 974ms）→ 2 工人 15.8/s（494ms）→ 4/8/16 不再涨** |
| 工人数：`/problems`、静态页 | 1~16 工人都在 **1700~1900 /s** —— 瓶颈是主线程，不是工人 |
| CodeMirror 资源 | 5 个文件 **200**，共 **~208KB** |

**队头阻塞复现**（第 ③ 步的动机，一次成功）：

```
0. 起跑线                           curl = 200
1. 开一条空连接挂 8 秒             fd=3 连上
2. ss -tnp | grep :8080            ESTAB  server fd=4        ←←← 铁证
3. 空连接挂着时 curl               000 / 退出码 28           ← 整个服务器装死
4. 空连接关掉后 curl               200 / 退出码 0            ← 立刻复活
```

机制：`while(true){ accept(); while(true){ read(); } }` —— **单线程 + 阻塞**，
`read()` **没有超时**。只要有一条连接**连着不发数据**（浏览器 keep-alive / 预连 / favicon），
就永远卡在 `read()`，`accept()` 轮不到 → 后续全堵在 backlog。

> **这就是第 ④ 步（epoll + 线程池）存在的全部理由。**

**2026-10-12（a-2 期间）：它把搭的人自己挡在门外了。**

现象：浏览器点「提交」→ **页面一直卡死**，一度被误当成「空白表单提交的 bug」。
实测否掉了这个归因 —— 服务器上打三种 POST（`name=&msg=` / `-d ""`(CL:0) / `name=rikka&msg=`）**全超时**，
**跟填不填内容无关**。

```
server(pid 2445554) 手上只有一条连接：127.0.0.1:43456   Recv-Q 0     ← 卡在它的 read() 上
另外三条堆在门外没人 accept：   Recv-Q 720 / 720 / 78               ← 请求到了，服务器没接
```

触发者是 **VS Code 的端口转发**（它会留一条到 8080 的连接不放；浏览器自己还会**预连接**）。
**只要走浏览器 + VS Code 转发，基本必堵。**

→ 已上止血药（决策 21）。**根治仍等第 ④ 步。**

## 已知问题 / 待改进

- ~~**队头阻塞**~~ —— ✅ **已根治**（决策 26 + 27，**两半都治了**）：
  epoll 让「**等**」不再独占，线程池让「**干**」不再独占。
  ⚠️ 顺带：原来那副止血药 `SO_RCVTIMEO`（决策 21）**已经删掉了** ——
  `conn_fd` 改成 `O_NONBLOCK` 之后它不起作用了，留着就是死代码。
  ⚠️ 它曾经不是理论问题 —— 2026-10-12 它把搭的人自己挡在门外了。
- **粘包不处理**：一条连接上来了两个请求，第二个**静默丢弃**（只打印一行提示）。
- **不支持 keep-alive**：每个请求都 `close`。**这是有意的，而且不算退步** ——
  一期 Flask 也主动禁用了它：`werkzeug/serving.py` 里写着
  `# Always close the connection. This disables HTTP/1.1 keep-alive connections. They aren't handled well
  by Python's http.server because it doesn't know how to drain the stream before the next request line.`
  —— **理由和我们一模一样**。`CH-11` 要求「功能需求不变，仅替换实现」，所以**不做 = 与一期对齐**。
- ~~**响应头缺 `Connection: close`**~~ —— **已补**（`send_response` 里加了一行）。
  `send_response` 声明自己是 `HTTP/1.1`，而 1.1 **默认 keep-alive**；
  原来是「发完就 `close` 却一个字不说」，**一期是明说的**。
  这一处**曾是二期相对一期唯一的真退步**，现在平了。
- ~~**没有 URL 解码**~~ —— **a-3 做掉了**（`8af7f30`）。原来那句预言的「一旦加了 URL 解码，黑名单方案会立刻被打穿」
  **已实测坐实**（决策 24）—— 而且打穿它的那条 payload，**白名单稳稳拦下**。
- ~~**路径里的 `+` 被当成空格**~~ —— **已修**（`fc2acdc`）：`urldecode` 加了 `plus_is_space` 开关，
  `parse_form` 走默认值、一个字没改。见决策 23。
- **只支持 GET / POST**：没有 PUT / DELETE / HEAD，也不支持 `multipart/form-data` 上传。
- **`/echo` 用 `form["name"]` 读值（`operator[]`）** —— 键不存在时会**凭空往 map 里塞一个空串**。
  这里**没有可见症状**（空串照样渲染成空），但决策 5 的规则仍然成立：**读要用 `count` / `find`**。
  接 MySQL 那一步之前回头看一眼。
- **`realpath` 对目录也会成功**：`www/sub` 这种目录会被放行，`ifstream` 读出空串返回 `200` 空 body。
  极端情况才需要额外判 `is_directory`。
- ~~**没有日志/错误处理**~~ —— ✅ **已解决**（决策 37，`FIX-07`）：独立日志模块 `log.h` / `log.cpp`，分级 + 落盘 + 线程安全 + 防注入。
- **导航栏那份 HTML 复制在 5 个地方**（4 个 `www/*.html` + `server.cpp` 里的结果页）—— 改导航要改 5 处。
  静态页那 4 份是静态站的天性；**C++ 那份可以靠模板文件躲掉**（见决策 25）。
- **`Content-Type` 表很小**：只认 html / css / js / png，其余一律 `application/octet-stream`。
  ⚠️ 所以**别用 `.svg` 文件**（会被当 octet-stream 发出去）—— 图标要么内联进 HTML，要么用 `.png`。
- ~~**无任何并发保护**~~ —— ✅ **已审过**（决策 27）。**共享的只有 `task_queue`**（有 `mtx` + `cv` 保护）；
  `epfd` / `conn_bufs` **只有主线程碰**，工人不碰；`Task t` 是各线程**自己的栈**。
  **规矩：先问「这东西谁在碰」，只有被多方碰的才需要锁。**
- ~~**日志会交错**（决策 27 带来的）~~ —— ✅ **已解决**（决策 37）：所有输出改走 `log_write()`，
  由 `mutex` 串行化整行写入，不再有 4 个线程交错写 `cout` 的问题。
- ⚠️ **`worker()` 里没有 `try/catch`**：`handle_request` 抛异常（如 `stoi`）→ `std::terminate` → **整个进程死**。
  和线程池之前**结果一样**（不是本步引入），但值得记 —— 一期 Flask 那个「没 except，工人线程静默死掉」
  是同一课的 Python 版。
- ~~**`submissions.username` 恒为 `NULL`**~~ —— ✅ **已修**（§30d）：登录后提交写入 `current_user`。
  守卫上线前的**老记录仍是 `NULL`**，按「无主数据」处理（普通用户看不了，admin 能看，§30e）。
- ~~**`/result/<job_id>` 无归属校验**~~ —— ✅ **已补**（§30d）：本人或 admin 才放行，越权 **403**，
  和一期 `FIX-11` 对齐。
- ⚠️ **判题数据目录硬编码**：`/home/ubuntu/online-OJ/judge_demo/testdata/<题号>`，换机器要改代码。

## 下一步

按需求文档 §7 `CH-11` 的四步走：

1. ~~单线程阻塞 HTTP 服务~~ —— **已完成**（`e9cd9dc`）
2. ~~请求解析 + 路由~~ —— **已完成**（`8e23ce6` → `1ad37a7`），半包/粘包都实测过
3. ~~静态文件服务~~ —— **已完成**（`88fb97a` / `5fa3aa2` / `86e3ab3` / **🔒 `f3759c3`**）
4. ~~**epoll + 线程池**~~ —— ✅ **两半全齐**（`CH-11` 要的两件都做完了）。**队头阻塞已根治**。
   - ✅ **epoll 那一半**（`b8e7289` + `1f6d20f`）—— 详见**决策 26**。
     子步：① `handle_request()` 抽取（`req` / `conn_fd` 走参数、`content_types` 提为全局 `const`
     → `types[ext]` 必须换成 `find()` / `at()`）② epoll 骨架 + 管 `conn_fd`
     ③ `request_total_len()` 攒够一个完整请求再处理。
     **网页第一次由二期 C++ 服务端完整服务出来**，`handle_request` 内部**一行没改**。
   - ✅ **线程池那一半**（`520a3fa`，2026-10-15）—— 详见**决策 27**。
     主线程只接客派活，`handle_request` + `close` 交给 4 个工人线程；
     **实测 1.034s（4 工人） vs 4.019s（1 工人）**。
   - ⏭ **还欠一条**：换完之后**跟一期 Flask、以及单线程版做同一浏览器对照压测**。

穿插项（顺序待定）：

- ~~a-1 解析 POST body~~ —— **已完成**（`8a7aca5`）
- ~~a-2 `/echo` 回显页 + 🔒 XSS~~ —— **已完成**（`62cfb7d`）
- ~~a-3 路径 URL 解码 + 黑名单/白名单对照实测~~ —— **已完成**（`8af7f30`）
- ~~**接 MySQL**~~ —— ✅ **已完成**（**决策 28**）：`/problems` 从库读题渲染、`thread_local` 每线程一条连接（57ms → 1ms）
- ~~**接判题器**~~ —— ✅ **已完成**（**决策 29**，`694f772` → `eaff82d`）。
  提交 → 入队 → 判题 → 落库 → `/result/<job_id>` 自动刷新，**主干道打通**。
- ~~**登录 / 会话**~~ —— ✅ **已完成**（**决策 30**，`07774a8` → `450d7ef` + 本步）。
  ① scrypt 校验 ② `sessions` 表 + `POST /login` ③ 路由守卫 + 提交归属 + `/result` 越权 403
  ④ `/register` / `/logout` / 改密 —— **四个子步全齐**。
- ~~**`/submissions`（我的提交列表）**~~ —— ✅ **已完成**（**决策 31**）。
  顺带把 `submissions.source` 落库，题目页预填 + 结果页展示 —— **`OJ-REC-01` 的「源码展示」缺口在二期补上了**。
- ~~**`/rank`（排行榜）**~~ —— ✅ **已完成**（**决策 32**）：Redis 缓存 60s、AC 后失效、缓存故障降级查库。
- ~~队列最前面 = `/admin/*` 管理功能~~ —— ✅ **已完成**（**决策 35**，`25e72ea` / `c9155dd` / `1e12f2a`）。**`CH-11` 的功能对齐到此结束**，二期与一期功能已一致。
- ~~与一期 Flask / 单线程版的对照压测~~ —— ✅ **已完成**（**决策 36**，2026-10-23）。
- ~~**文档收尾**~~ —— ✅ 需求文档已到 **v1.5**（2026-10-26）：`FIX-12` / `FIX-24` / `FIX-25` 标为已解决，`OJ-JDG-08` / `OJ-SUB-01` / `OJ-SYS-01` 的实现状态同步更新；本条决策见 **§38**。
- 剩下：**2 条 FIX**（`FIX-04` / `08`）与答辩准备。
  - ✅ `FIX-19` / `20` / `21` / `16` / `23` 已于 2026-10-27 修完（见 **§40**）。
  - `FIX-08`（无 cgroup / namespace）按需求文档定性是「课程设计可接受，须如实标注」，**不改代码，写进设计文档的边界说明**。
  - `FIX-04`（内存只能事后算账）：可选做法是判题进程里加监视线程轮询 `/proc/<pid>/status` 的 `VmHWM`，超阈值直接 kill；改的是 `judge_demo/judge.cpp`（C++）。

## 待做的对照实验

套路照 `a-3`：**从现役 `server.cpp` 复制变体 → 改端口 → 并排跑 → 跑完即删。主代码一行不动。**
（a-3 那次就是这么做黑名单 / 白名单三版对照的。）

- **keep-alive 有 / 无 的 QPS 对比**（2026-10-14 记）
  动机：keep-alive **不是需求** —— 需求文档只字未提，`CH-01`~`CH-11` 十一条「预留扩展空间」里也没有；
  而且**一期 Flask 也主动禁用了它**（`werkzeug/serving.py`：`Always close the connection`）。
  **结论：不进主代码，只做对照。**
  做法：两个变体（一个带 keep-alive、一个不带）并排跑，用同一份压测脚本量 QPS。
  产出：一条对比数字，写报告 / 面试用。

---

相关：`judge_demo/NOTES.md`（判题器）、`web/NOTES.md`（一期 Flask 层）、
`软件需求规格说明书-OJ.md` §7 `CH-11`、§8 功能列表。
