# 二期服务端笔记（C++ 自研 HTTP 服务）

> 记录核心设计决策与实测结论，可直接用作实验报告素材。
> 对应需求文档 §7 的 `CH-11`：二期 = **socket + epoll + 线程池**，
> **功能需求不变、仅替换实现**，对浏览器端透明。

## 当前能力

```
cd ~/online-OJ/oj_server
g++ -Wall -o server server.cpp
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
 ├── socket → setsockopt(SO_REUSEADDR) → bind → listen        建好门
 └── while (true)                                             外层：一个客人接一个客人
      ├── accept                                              接一位
      ├── setsockopt(SO_RCVTIMEO, 5s)                         ⏱ 只招呼他 5 秒（止血，决策 21）
      ├── while (true) { read }                               内层：攒够一个完整请求
      ├── 解析请求行 / 头部 / body（POST 的 body → parse_form）
      ├── 路由：/ 首页 · /echo 表单+回显 · 其余静态文件（含 🔒 目录穿越校验）
      ├── send_response                                       拼响应 + write
      └── close(conn_fd)                                      送客
```

**两层循环是两件事，别混**：

- 外层 `while(true)` = 客人一个接一个（`accept`）
- 内层 `while(true)` = 同一条连接上把请求攒够（`read`）

把内层的 `n <= 0` 判断放到外层，就会出现「一个客户端断开 → `break` 停掉整个服务器」。

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

- **队头阻塞**（见上）—— **已止血，未根治**。`SO_RCVTIMEO` 5 秒（决策 21）把「**永久**堵死」
  变成「**每次最多**堵 5 秒」，但**仍是一次只招呼一个**。根治要等 epoll + 线程池。
  ⚠️ 它不再是理论问题 —— 2026-10-12 它把搭的人自己挡在门外了。
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
- **没有日志/错误处理**：所有输出都是 `cout` 调试信息，没有分级、没有落盘。
- **导航栏那份 HTML 复制在 5 个地方**（4 个 `www/*.html` + `server.cpp` 里的结果页）—— 改导航要改 5 处。
  静态页那 4 份是静态站的天性；**C++ 那份可以靠模板文件躲掉**（见决策 25）。
- **`Content-Type` 表很小**：只认 html / css / js / png，其余一律 `application/octet-stream`。
  ⚠️ 所以**别用 `.svg` 文件**（会被当 octet-stream 发出去）—— 图标要么内联进 HTML，要么用 `.png`。
- **无任何并发保护**（现在单线程，暂时不需要；第 ④ 步上线程池时要重新审一遍）。

## 下一步

按需求文档 §7 `CH-11` 的四步走：

1. ~~单线程阻塞 HTTP 服务~~ —— **已完成**（`e9cd9dc`）
2. ~~请求解析 + 路由~~ —— **已完成**（`8e23ce6` → `1ad37a7`），半包/粘包都实测过
3. ~~静态文件服务~~ —— **已完成**（`88fb97a` / `5fa3aa2` / `86e3ab3` / **🔒 `f3759c3`**）
4. **epoll + 线程池** —— 🚧 **进行中**（`CH-11` 要的是 epoll **和** 线程池 **两件**）。
   **队头阻塞的现场已经量过，就是它的理由**（决策 21 只止了血，没根治）。
   换完之后跟一期 Flask、以及本步的单线程版**同一浏览器对照压测**。
   - ✅ **子步 1+2 完成**（`b8e7289`）：`handle_request()` 抽取（`req` / `conn_fd` 走参数）、
     `content_types` 提为全局 `const`（于是 `types[ext]` 必须换成 `find()` / `at()`）、
     epoll 骨架（`epoll_create1` + `epoll_ctl(ADD, listen_fd)` + `while(true) epoll_wait`）、
     `accept` 后**不再 `close`** 而是 `fcntl(O_NONBLOCK)` + `epoll_ctl(ADD, conn_fd)`、
     可读事件 `read` 一次攒进 `map<int,string> conn_bufs`、
     `cnt == 0` → 摘牌（`EPOLL_CTL_DEL`）+ 撕页（`erase`）+ 关门；`cnt < 0` → 只有 `EAGAIN` / `EWOULDBLOCK` 算正常。
     实测：9000 字节的请求 → `4096 / 8192 / 9154` 三行（**电平触发**，一次没读完下一轮接着报）；
     连打 3 个请求 fd 数稳定在 6、`ss` 无 `CLOSE-WAIT`
     （**上一版在同一处是 `CLOSE-WAIT` + fd 一路漏** —— 反向验证）。
   - ✅ **子步 3 完成**：新增 `request_total_len(req)` —— 返回「这个请求一共要多少字节」，
     头部还没齐时返回 `string::npos`（= **还不知道**）；【`head_end + 4 + Content-Length`】。
     三道防线：`pos < head_end`（`Content-Length:` 出现在 body 里不算）、`end > start`（值是空的跳过）、
     `try/catch`（值不是数字 → 当它没有 body）。
     攒够 → 抠出 `req` → 摘牌 → 撕页 → `handle_request(fd, req)` → `close(fd)`。
     **网页第一次由二期 C++ 服务端完整服务出来**；`handle_request` 内部**一行没改**。
   - ⏭ **剩：线程池**（`CH-11` 的另一半）。分工要说清：
     **epoll 解决「等待」不再独占**（等一条连接时能招呼别人）；
     **线程池解决「干活」不再独占**（`handle_request` 跑着时，别人只能站着等
     —— 将来接上判题器，`fork` 出去跑 2 秒，这 2 秒全服务器停摆）。

穿插项（顺序待定）：

- ~~a-1 解析 POST body~~ —— **已完成**（`8a7aca5`）
- ~~a-2 `/echo` 回显页 + 🔒 XSS~~ —— **已完成**（`62cfb7d`）
- ~~a-3 路径 URL 解码 + 黑名单/白名单对照实测~~ —— **已完成**（`8af7f30`）
- **接 MySQL** → **接判题器**（`/submit` 那条主干道要走通，就需要这两样）
- 队伍最前面仍然是 **第 ④ 步 epoll + 线程池**（止血只管 5 秒，见决策 21）

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

---

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

**下一步**：**接判题器**（`/submit` 主干道）。
