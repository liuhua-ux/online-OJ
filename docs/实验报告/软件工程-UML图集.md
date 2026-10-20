# 软件工程 UML 图集（时序图 / 类图 / 活动图 / 组件图 / 状态图 / E-R）

本文件与《软件工程-实验3-报告内容.md》配套：图1（学生端用例图）、图2（管理员端用例图）在实验3 报告中，这里补齐其余各类图，供实验4（概要设计）、实验5（详细设计）直接取用。

| 图号 | 图类型 | 主题 | 服务的实验 |
|---|---|---|---|
| 图3 | 时序图 | 提交代码与异步判题 | 实验5 详细设计 |
| 图4 | 类图 | 领域实体与服务模块 | 实验5 详细设计 |
| 图5 | 活动图 | 判题器内部流程 | 实验5（补充） |
| 图6 | 组件图 | 系统组成与接口 | 实验4 概要设计 |
| 图7 | 状态图 | 提交记录生命周期 | 实验5（补充） |
| 图8 | 状态图 | 题目生命周期 | 实验5（补充） |
| 图9 | E-R 图 | 数据库设计 | 实验5 详细设计 |

> 所有图均以 PlantUML 源码给出，渲染方法与实验3 相同（见文末附录）。

---

## 图3 时序图：提交代码与异步判题

**用途**：展示一次提交从浏览器到判题完成的全过程，重点体现"**提交立刻返回、判题在后台异步执行**"。

```plantuml
@startuml 时序图-提交与判题
autonumber
actor 学生 as U
participant 浏览器 as B
participant "Web 服务端\n(epoll + 4 个工作线程)" as S
participant "判题队列\n(judge_queue)" as Q
participant "判题工人\n(2 个线程)" as W
participant "判题器\n(judge 进程)" as J
database "MySQL" as DB

U -> B : 编写代码并点击提交
B -> S : POST /submit/{题号}（表单携带代码）
S -> S : 校验登录状态与代码非空
S -> DB : INSERT submissions（status='排队中'）
S -> Q : push(JudgeTask)
S --> B : 302 Location: /result/{job_id}
B -> S : GET /result/{job_id}
S -> DB : SELECT status
S --> B : "排队中"页面（meta refresh 每 2 秒）

Q -> W : 取出任务
W -> DB : UPDATE status='判题中'
W -> J : fork / exec（源码, 测试数据目录, 时限, 内存）
J -> J : g++ 编译
alt 编译失败
    J --> W : VERDICT: CE
else 编译成功
    J -> J : 逐测试点运行并比对
    J --> W : VERDICT / 通过的 AC 行 / time / peak memory
end
W -> W : parse_judge() 解析输出
W -> DB : UPDATE status='完成', verdict, passed, time, memory
W -> W : 删除临时工作目录

B -> S : GET /result/{job_id}（自动刷新）
S -> DB : SELECT status, verdict, ...
S --> B : 结果页（结论 / 通过组数 / 耗时 / 内存）
@enduml
```

![图3 时序图：提交代码与异步判题](图3-时序图.png)

**读图要点**：
1. **两次 HTTP 请求不是一次**：提交只负责"接住并落库"，然后 302 到结果页；判题在另一个线程里进行，所以提交响应时间与判题耗时无关（实测 2.10 s → 0.0032 s）。
2. **两条队列**：`task_queue` 装的是 HTTP 连接（epoll 接收后交工作线程），`judge_queue` 装的是判题任务（提交后交判题工人），两者互不阻塞。
3. **判题器是独立进程**：Web 端通过 `fork/exec` + 管道拿它的 stdout，因此判题器崩溃不会带崩服务端。
4. **alt 分支**对应"编译失败即 CE，不进入运行阶段"。

---

## 图4 类图：领域实体与服务模块

**用途**：详细设计阶段的核心类图，展示实体属性、服务职责与关系。

> 说明：领域实体对应数据库表；`HttpServer`、`JudgeRunner` 是**模块抽象**（实际实现为自由函数 + 全局队列 + 线程池），类图用于表达职责划分，不要求与代码一一对应。

```plantuml
@startuml 类图-领域与服务
skinparam classAttributeIconSize 0

class User {
  - user_id : int
  - username : string
  - password_hash : string
  - is_admin : bool
  - is_disabled : bool
  - created_at : datetime
  + 校验口令(明文) : bool
  + 修改口令(新明文)
  + 禁用() / 启用()
}

class Problem {
  - problem_id : int
  - title : string
  - description : text
  - input_format : text
  - output_format : text
  - sample_input : text
  - sample_output : text
  - time_limit_ms : int
  - memory_limit_mb : int
  - is_published : bool
  - is_deleted : bool
  + 发布() : bool
  + 取消发布()
  + 软删除()
}

class TestCase {
  - testcase_id : int
  - ordinal : int
  - input_path : string
  - answer_path : string
  - input_size : int
  - answer_size : int
}

class Submission {
  - job_id : string
  - username : string
  - problem_id : int
  - status : string
  - result : text
  - verdict : string
  - passed : int
  - total : int
  - time_ms : int
  - memory_kb : int
  - created_at : datetime
  + 标记判题中()
  + 写入结论(结论)
}

class JudgeTask {
  - job_id : string
  - work_dir : string
  - problem_id : int
  - tl_ms : int
  - ml_mb : int
}

class JudgeSummary {
  - verdict : string
  - passed : int
  - time_ms : int
  - memory_kb : int
}

class HttpServer {
  - listen_fd : int
  - epoll_fd : int
  - task_queue : 连接任务队列
  - judge_queue : 判题任务队列
  + accept_loop()
  + worker()
  + judge_worker()
  + handle_request(请求)
  + render_problem(题号)
  + render_result(提交号)
}

class JudgeRunner {
  + run_judge(源码, 数据目录, 时限, 内存) : bool
  + parse_judge(输出) : JudgeSummary
}

User "1" -- "0..*" Submission : 提交
Problem "1" -- "0..*" Submission : 被提交
Problem "1" *-- "1..*" TestCase : 包含

HttpServer --> JudgeTask : 创建并入队
JudgeTask --> Submission : 同一 job_id
HttpServer ..> JudgeRunner : 调用
JudgeRunner --> JudgeSummary : 产出
HttpServer --> Submission : 读 / 写状态与结论
@enduml
```

![图4 类图：领域实体与服务模块](图4-类图.png)

**读图要点**：
1. **组合关系** `Problem *-- TestCase`：测试点脱离题目没有意义，题目删除时一并处理（实心菱形画在题目一端）。
2. **关联** `User -- Submission`、`Problem -- Submission`：一条提交同时指向一个人和一道题；两次都是 1 对多。
3. `JudgeTask` 是队列中的消息，携带 `job_id` 与运行参数；`JudgeSummary` 是判题器输出的解析结果，二者都是传值对象。

---

## 图5 活动图：判题器内部流程

**用途**：把 `judge_demo/judge.cpp` 的控制流画清楚，是"详细设计"里最能体现判题业务的一张图。

```plantuml
@startuml 活动图-判题流程
start
:接收源码路径、测试数据目录、时限、内存上限;
:g++ -std=c++17 编译用户代码;
if (编译成功?) then (否)
  :输出 VERDICT: CE;
  stop
endif
if (存在测试点 1.in ?) then (否)
  :输出 VERDICT: NO_DATA;
  stop
endif
:i = 1;
while (存在测试点 i.in ?) is (是)
  :设置 RLIMIT_AS（内存上限）;
  :fork + exec 运行程序\nstdin 重定向自 i.in，stdout 写入 output.txt;
  if (超时（被定时信号杀死）?) then (是)
    :输出 VERDICT: TLE;
    stop
  elseif (内存超限?) then (是)
    :输出 VERDICT: MLE;
    stop
  elseif (非正常退出?) then (是)
    :输出 VERDICT: RE;
    stop
  endif
  :逐行比对 output.txt 与 i.ans;
  if (输出一致?) then (否)
    :输出 VERDICT: WA;
    stop
  endif
  :本组通过，输出 AC 行与 time / peak memory;
  :i = i + 1;
endwhile (否)
:输出 VERDICT: AC;
stop
@enduml
```

![图5 活动图：判题器内部流程](图5-活动图.png)

**读图要点**：
1. **遇到第一个不通过的测试点就停止**，不再跑后面的测试点（所以 `passed` 只统计已经通过的前缀组数）。
2. **判定优先级**：超时 → 内存 → 异常退出 → 输出不一致，四者是 `elseif` 关系，不会同时成立。
3. **NO_DATA 是系统侧问题**（没有测试数据），与用户代码无关，单独判定。

---

## 图6 组件图：系统组成与接口

**用途**：概要设计阶段的系统结构图 / 模块依赖图，展示组件及其提供的、需要的接口。

```plantuml
@startuml 组件图-系统组成
skinparam componentStyle rectangle

actor 用户 as User
component "浏览器" as Browser
component "Web 服务端\n(oj_server，C++)\n一期为 Flask（web/）" as Web
component "判题器\n(judge_demo/judge)" as Judge
component "g++ 编译器" as GCC
database "MySQL\n(用户/题目/用例/提交)" as DB
database "Redis\n(排行榜缓存，一期)" as Redis
component "文件系统\n(testdata 测试数据 / 临时工作目录)" as FS

User --> Browser
Browser --> Web : HTTP
Web --> DB : SQL（3306）
Web ..> Redis : 读写排行榜缓存
Web --> Judge : fork/exec + 管道收 stdout
Judge --> GCC : 调用编译
Web --> FS : 读写测试数据
Judge --> FS : 读 .in/.ans，写 output.txt
@enduml
```

![图6 组件图：系统组成与接口](图6-组件图.png)

**读图要点**：
1. **Web 层与判题层解耦**：两者之间只有"命令行参数 + 管道 + 退出码"这一条接口，所以一期 Flask 换二期 C++，判题器完全不受影响——这正是分层的价值。
2. **测试数据走文件系统**，数据库只存元信息（序号、路径、大小），避免大文本进出数据库。
3. **Redis 目前只服务于排行榜缓存**（一期实现）；二期 C++ 服务端尚未接入，图中以虚线依赖表示可选。

---

## 图7 状态图：提交记录生命周期

**用途**：展示一条提交记录在数据库中的状态迁移。

```plantuml
@startuml 状态图-提交记录
[*] --> 排队中 : POST /submit 落库
排队中 --> 判题中 : 判题工人取出任务
判题中 --> 完成 : finish_job() 写回结论
完成 --> [*]

note right of 完成
  status 列只有三种取值：
  排队中 / 判题中 / 完成
  verdict 列（结论）：
  AC / WA / TLE / MLE / RE / CE / NO_DATA / SE
end note
@enduml
```

![图7 状态图：提交记录生命周期](图7-状态图-提交记录.png)

**读图要点**：
1. **状态与结论分开存**：`status` 回答"判完了没有"，`verdict` 回答"判成什么"，所以页面用 `status == 完成` 决定还要不要自动刷新。
2. **没有失败终态**：判题器没启动起来也落到"完成"，只是 `verdict = SE`，保证结果页一定能停住（否则页面会永远 2 秒刷新）。
3. 若扩展重判功能，只需从"完成"加一条回到"排队中"的转移。

---

## 图8 状态图：题目生命周期

**用途**：展示题目从草稿到发布、软删除的业务状态。

```plantuml
@startuml 状态图-题目
[*] --> 草稿 : 新建题目（is_published=0）
草稿 --> 已发布 : 勾选发布\n[已配置至少一个测试点]
已发布 --> 草稿 : 取消发布
草稿 --> 已删除 : 软删除
已发布 --> 已删除 : 软删除
已删除 --> [*]

note right of 已删除
  is_deleted = 1，不物理删除，
  历史提交仍能 JOIN 出题目标题
end note
@enduml
```

![图8 状态图：题目生命周期](图8-状态图-题目.png)

**读图要点**：
1. **守卫条件** `[已配置至少一个测试点]` 就是需求里的业务规则，对应服务端"无测试点不允许发布"的校验。
2. **软删除而非物理删除**，避免历史提交变成取不到标题的孤儿记录。

---

## 图9 E-R 图：数据库设计

**用途**：数据库概念结构设计，对应实验5 的"至少 2 张表、第三范式"要求（实际 4 张表）。

```plantuml
@startuml ER-数据库设计
hide circle
skinparam linetype ortho

entity "users 用户" as users {
  * user_id : INT <<PK>>
  --
  username : VARCHAR(32) <<UK>>
  password_hash : VARCHAR(255)
  is_admin : TINYINT
  is_disabled : TINYINT
  created_at : DATETIME
}

entity "problems 题目" as problems {
  * problem_id : INT <<PK>>
  --
  title : VARCHAR(128)
  description : MEDIUMTEXT
  input_format : TEXT
  output_format : TEXT
  sample_input : TEXT
  sample_output : TEXT
  time_limit_ms : INT
  memory_limit_mb : INT
  is_published : TINYINT
  is_deleted : TINYINT
  created_at : DATETIME
}

entity "testcases 测试用例" as testcases {
  * testcase_id : INT <<PK>>
  --
  problem_id : INT <<FK>>
  ordinal : INT
  input_path : VARCHAR(255)
  answer_path : VARCHAR(255)
  input_size : INT
  answer_size : INT
}

entity "submissions 提交记录" as submissions {
  * job_id : CHAR(8) <<PK>>
  --
  username : VARCHAR(32)
  problem_id : INT <<FK>>
  status : VARCHAR(16)
  result : MEDIUMTEXT
  verdict : VARCHAR(8)
  passed : INT
  total : INT
  time_ms : INT
  memory_kb : INT
  created_at : DATETIME
}

users ||--o{ submissions : "1:N（username）"
problems ||--o{ submissions : "1:N"
problems ||--o{ testcases : "1:N"
@enduml
```

![图9 E-R 图：数据库设计](图9-ER图.png)

**读图要点**：
1. **主键**：users / problems / testcases 用自增整型，submissions 用 8 位十六进制 `job_id`（提交时就能生成，落库与跳转不依赖自增回填）。
2. **外键**：`testcases.problem_id`、`submissions.problem_id` 有物理外键；`submissions.username` 与用户是**逻辑关联**（用户名可空，历史数据中没有填写者），图中保留这条关系以体现业务含义。
3. **第三范式**：四个实体各自只存自身属性；测试点的输入输出正文不放进题目表（避免多值导致重复组），只存路径与大小；提交的结论字段直接依赖主键 `job_id`，不存在传递依赖。
4. **唯一约束**：`users.username` 唯一；`testcases (problem_id, ordinal)` 唯一，保证测试点序号不重复。

---

## 附：渲染与插入报告的方法

1. 打开 <https://www.plantuml.com/plantuml/uml/>（或 VS Code 的 PlantUML 插件），粘贴某一图的完整源码（`@startuml` 到 `@enduml`，不要带 Markdown 的代码围栏）；
2. 确认中文正常、关系方向正确；
3. 右键图 → **Save as PNG**（要清晰就在源码第二行加 `skinparam dpi 200`）；
4. 按图号命名为 `图3-时序图.png`、`图4-类图.png`……插入《软件工程实验报告汇总版》对应实验的报告纸；
5. 一页放不下时另起一页。
