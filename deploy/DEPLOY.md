# 公开部署说明（`feat/public-deploy` 分支）

> 答辩版是 `main` / 标签 `v1.0-course`，内容和这里不同。**本分支不要求合并回 main。**

## 和答辩版的区别

| 方面 | 答辩版（main） | 公开部署版（本分支） |
| --- | --- | --- |
| 判题 | 直接 `fork` 判题器，与服务器同用户 | **每次判题跑一个 Docker 容器**：无网络、根文件系统只读、只有工作目录可写、测试数据只读、CPU/内存/PID 限额 |
| 提交临时目录 | `/tmp/oj_XXXXXX` | `/var/lib/oj/work/oj_XXXXXX`（Docker 挂载解析在 daemon 端，必须是宿主机路径） |
| 注册 | 自助注册 | **关闭**，由管理员在 `/admin/users` 建号 |
| 限流 | 无 | 同一用户名登录失败 10 次 / 10 分钟；同一用户提交冷却 3 秒 |
| 体积上限 | 无 | 请求体 1 MB、源码 256 KB |
| 日志 | 只有 `cout` | 追加写 `oj_server/logs/oj.log`（`INFO` / `WARN`），关闭 `FIX-07` 的主要部分 |
| 进程管理 | 手动 `nohup ./server` | systemd：开机自启、崩溃 3 秒后自动重启 |
| 判题器 | `judge.cpp` 原版 | `fstream` → `ifstream`（只读挂载上 `fstream` 的默认读写模式打不开文件） |

**为什么判题必须进容器**：判题器运行的是**用户提交的任意代码**，而服务器进程和它同用户（`ubuntu`），能读到 `~/.ssh/id_ed25519`、`db_config.h`，而且没有禁网。回环自用无所谓，一公开就等于把服务器交给任何一个能注册的人。

## 首次部署步骤

```bash
cd ~/online-OJ
git checkout feat/public-deploy

# 1) 构建判题镜像（镜像里编译 judge.cpp，避免宿主机/镜像 glibc 不一致）
sudo docker build -f deploy/judge/Dockerfile -t oj-judge:latest .

# 2) 提交临时目录
sudo mkdir -p /var/lib/oj/work && sudo chown -R ubuntu:ubuntu /var/lib/oj

# 3) 编译并交给 systemd
cd oj_server
g++ -Wall -o server server.cpp auth.cpp db.cpp $(mysql_config --cflags --libs) -lcrypto -lhiredis -pthread
sudo cp ~/online-OJ/deploy/oj-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now oj-server

# 4) 验收
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/problems     # 200
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/register     # 200（自助注册开启；关掉时改 ALLOW_REGISTRATION=false）
```

## 对外开放（需要手动做）

1. 腾讯云控制台 → 云服务器 → 安全组 → 入站规则 → 添加 `TCP:8080`；
   **来源尽量填固定 IP**（答辩教室 / 学校出口），不要图省事开 `0.0.0.0/0`。
2. 访问地址：`http://<公网IP>:8080`。
   **用 IP + 端口不需要 ICP 备案**；用域名 + 80/443 才需要。
3. 演示/试用结束，把那条安全组规则删掉。

## 账号怎么开

注册已关闭。管理员登录后进 `/admin/users` →「新建账号」→ 填用户名和初始密码。
新用户第一次登录后可在 `/profile` 自己改密码。

## 日常运维

```bash
systemctl status oj-server                 # 服务状态
journalctl -u oj-server -f                 # stdout/stderr
tail -f ~/online-OJ/oj_server/logs/oj.log  # 业务日志（登录失败、提交、判题结果）
sudo docker ps                             # 正常时不应有 oj-judge 容器残留
```

**备份**（建议加进 crontab）：

```bash
mysqldump --default-character-set=utf8mb4 oj > ~/backup/oj-$(date +%F).sql
tar czf ~/backup/testdata-$(date +%F).tgz -C ~/online-OJ/judge_demo testdata
```

**回滚到答辩版**：

```bash
cd ~/online-OJ && git checkout main
cd oj_server && g++ -Wall -o server server.cpp auth.cpp db.cpp $(mysql_config --cflags --libs) -lcrypto -lhiredis -pthread
sudo systemctl restart oj-server
```

## HTTPS（可选，强烈建议）

- 有域名并且已备案：装 Caddy，反代 `127.0.0.1:8080`，证书自动申请；
- 没有域名：只能 `http://IP:8080`，**密码是明文传输**。至少给同学开独立账号、让他们用一次性口令，别用常用密码。

## 已知缺口（公开部署后仍存在）

| 编号 | 内容 | 影响 |
| --- | --- | --- |
| `FIX-19` | 管理页写操作无 CSRF 令牌 | 管理员被诱导访问恶意页面可被代发操作 |
| `FIX-21` | 编辑题目是「整份替换」，无版本号 | 两个管理员同时编辑会丢更新 |
| `FIX-24` | 禁用账号不踢已有会话 | 被禁用的人手里的票在有效期内仍可用 |
| `FIX-20` | 测试点落盘不是原子操作 | 写到一半失败会让该题测试点全没 |
| — | 密码走 HTTP 明文 | 见上一节 |
