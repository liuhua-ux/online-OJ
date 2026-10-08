import datetime
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import uuid

import pymysql
from flask import Flask, redirect, request, session
from markupsafe import escape
from werkzeug.security import generate_password_hash, check_password_hash

import db
import db_config

app = Flask(__name__)
app.secret_key = db_config.SECRET_KEY   # 印章：给 cookie 签名用

JUDGE_DIR = "/home/ubuntu/online-OJ/judge_demo"
JUDGE_BIN = os.path.join(JUDGE_DIR, "judge")
TESTDATA_DIR = os.path.join(JUDGE_DIR, "testdata")

JOB_TIMEOUT = 60  # 秒。等超过这么久还没判完，就认为出事了

# ---------- 队列：还没判的提交排在这里 ----------
job_queue = queue.Queue()

# 题目表单里的字段：(字段名, 中文标签, 文本框行数)。行数 1 = 单行输入框，其余是 textarea
PROBLEM_FIELDS = [
    ("title", "标题", 1),
    ("description", "题面", 10),
    ("input_format", "输入格式", 4),
    ("output_format", "输出格式", 4),
    ("sample_input", "样例输入", 4),
    ("sample_output", "样例输出", 4),
]


def worker():
    """后台工人：不停从队列里取任务，判完把结果写回数据库。"""
    while True:
        # 队列里除了编号和临时目录，还有「这是哪道题」——工人只能从这里知道
        job_id, work_dir, problem_id = job_queue.get()
        src_path = os.path.join(work_dir, "submission.cpp")

        # 每道题的测试点在自己的子目录里：testdata/<题号>/1.in ...
        data_dir = os.path.join(TESTDATA_DIR, str(problem_id))

        try:
            db.update_job(job_id, "判题中", None)
            result = subprocess.run(
                [JUDGE_BIN, src_path, data_dir],
                cwd=work_dir,
                capture_output=True,
                text=True,
            )
            text = "exit code: " + str(result.returncode) + "\n\n" + result.stdout
        except Exception as e:
            text = "判题器出错了：" + repr(e)
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

        try:
            db.update_job(job_id, "完成", text)
        except Exception as e:
            # 不能让工人线程死掉，否则队列会堵死
            print("failed to save result for", job_id, repr(e))


# 开 2 个工人（服务器就 2 核）
for _ in range(2):
    threading.Thread(target=worker, daemon=True).start()


def nav():
    """每个页面顶上那条导航。

    写成一个函数，是为了「只写一次」。否则将来想加一个链接，
    就得把 8 个页面里的 HTML 挨个找出来改一遍，漏一个就断一条路。
    """
    name = session.get("username")   # 有票就显示名字，没票就显示登录/注册
    if name is None:
        right = "<a href='/login'>登录</a>　<a href='/register'>注册</a>"
    else:
        # 用户名是注册时别人自己填的。不转义的话，nav() 被 8 个页面调用
        # = 全站每个页面都成了 XSS 入口
        right = f"已登录：{escape(name)}　<a href='/logout'>退出</a>"

    # 管理员才显示「管理」入口。每一次渲染都现查库（代价：多一次查询）
    admin_link = ""
    if name is not None and db.is_admin(name):
        admin_link = "<a href='/admin/problems'>管理</a>　"

    return (
        "<div style='border-bottom:1px solid #999;padding:6px;margin-bottom:14px'>"
        "<a href='/'>首页</a>　"
        "<a href='/problems'>题库</a>　"
        + admin_link +
        "<a href='/submissions'>提交记录</a>　| " + right +
        "</div>"
    )


def admin_guard():
    """管理页的统一守卫。返回 None = 放行；返回别的 = 直接把这个响应交给浏览器。

    抽出来是因为管理页会越来越多，权限检查只写一次。
    """
    name = session.get("username")
    if name is None:
        return redirect("/login")
    if not db.is_admin(name):
        return nav() + "<p>需要管理员权限。</p>", 403
    return None


def normalize_newlines(text):
    """把浏览器交上来的多行文本统一成 \\n 换行。

    ⚠️ 这一步不做，从管理页配的测试点会**判谁都是 WA**：
    判题器比对答案是「逐行 getline 读、行尾补一个 \\n、再拼成整串比」（judge.cpp）。
    答案文件里若是 "5\\r\\n"，拼出来就是 "5\\r\\n"；而选手程序在 Linux 上
    用 cout 输出的是 "5\\n" —— 差一个看不见的 \\r，字符串就是不相等。
    （HTTP 表单提交的多行文本，换行本来就规定是 \\r\\n，所以这条路上必然会遇到。
      顺带一提：判题器是「按行比」，所以结尾多一个少一个换行不影响结果。）
    """
    return text.replace("\r\n", "\n").replace("\r", "\n")


def parse_positive_int(raw, label):
    """解析一个正整数。返回 (值, 错误信息)，出错时值为 None。

    坑：int("abc") 是**抛异常**，不是返回个什么。不接住的话，
    管理员手滑填个字母，整个页面就是 500。
    文档 OJ-PRB-03 要求「时限/内存填非正数或非数字 → 拒绝保存」，就在这兑现。
    """
    try:
        value = int(raw)
    except ValueError:
        return None, f"{label}要填整数，不能是「{raw}」"
    if value <= 0:
        return None, f"{label}要填正数"
    return value, None


def read_text(path):
    """读一个测试点文件。文件不在了就当空串 —— 页面还能开出来，
    看得到「库里说有这组、盘上却没有」，不用靠猜。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def load_testcases_from_disk(problem_id):
    """把这道题的测试点读回来填进表单。返回 [(输入文本, 答案文本), ...]。

    按库里的元信息找文件（相对路径是相对 testdata/ 的，如 "1/1.in"）——
    库和盘哪边不对，这儿就会露出马脚，而不是安静地显示错内容。
    """
    cases = []
    for _ordinal, in_path, ans_path in db.list_testcases(problem_id):
        cases.append((
            read_text(os.path.join(TESTDATA_DIR, in_path)),
            read_text(os.path.join(TESTDATA_DIR, ans_path)),
        ))
    return cases


def save_testcases_to_disk(problem_id, cases):
    """把测试点写到 testdata/<题号>/，按顺序从 1 重新编号。返回元信息列表。

    ⚠️ 先**把目录整个清掉**再写。只覆盖前 N 个文件是不够的：
    原来 3 组，删掉中间一组变成 2 组，如果只写 1.in / 2.in，
    磁盘上那个旧的 3.in 还躺着 —— 判题器恰恰是靠「探测下一个 .ans 存不存在」
    来决定还有没有数据的，它会把这份**陈年数据**捞起来接着判。
    删掉的文件不会自己消失，这是「删除」这个动作最容易被忘掉的一半。
    """
    pdir = os.path.join(TESTDATA_DIR, str(problem_id))
    if os.path.isdir(pdir):
        shutil.rmtree(pdir)
    os.makedirs(pdir)

    meta = []
    for ordinal, (in_text, ans_text) in enumerate(cases, start=1):
        # 按字节写：不受平台换行处理影响，字节数也算得准
        in_bytes = normalize_newlines(in_text).encode("utf-8")
        ans_bytes = normalize_newlines(ans_text).encode("utf-8")
        with open(os.path.join(pdir, f"{ordinal}.in"), "wb") as f:
            f.write(in_bytes)
        with open(os.path.join(pdir, f"{ordinal}.ans"), "wb") as f:
            f.write(ans_bytes)
        # 库里存相对路径：换服务器、换目录，库里的数据一个字都不用改
        meta.append((
            f"{problem_id}/{ordinal}.in", f"{problem_id}/{ordinal}.ans",
            len(in_bytes), len(ans_bytes),
        ))
    return meta


def read_problem_form(form):
    """从提交的表单里读题目字段和测试点。返回 (values, cases, error)。"""
    values = {name: normalize_newlines(form.get(name, "")) for name, _, _ in PROBLEM_FIELDS}
    values["title"] = values["title"].strip()

    if not values["title"]:
        return values, [], "标题不能为空"
    for name, label, _rows in PROBLEM_FIELDS:
        # 样例允许留空（有的题不展示样例）；其余必填
        if name in ("title", "sample_input", "sample_output"):
            continue
        if not values[name].strip():
            return values, [], f"{label}不能为空"

    time_limit_ms, err = parse_positive_int(form.get("time_limit_ms", ""), "时间限制")
    if err is not None:
        return values, [], err
    memory_limit_mb, err = parse_positive_int(form.get("memory_limit_mb", ""), "内存限制")
    if err is not None:
        return values, [], err
    values["time_limit_ms"] = time_limit_ms
    values["memory_limit_mb"] = memory_limit_mb

    # 测试点：用两个 getlist 一次全取出来。每个块里都是「先 tc_in 后 tc_ans」，
    # 所以两个列表天然一一对应 —— 不用给每组起名字，加组删组也就不用改编号
    ins = form.getlist("tc_in")
    answers = form.getlist("tc_ans")
    if len(ins) != len(answers):
        return values, [], "表单读到的输入框与答案框对不上，请刷新页面重来"

    cases = []
    for in_text, ans_text in zip(ins, answers):
        in_text = normalize_newlines(in_text)
        ans_text = normalize_newlines(ans_text)
        # 两个都空的块 = 没用上的空位，丢掉。「删掉中间一组」也是走的这条路
        if not in_text.strip() and not ans_text.strip():
            continue
        cases.append((in_text, ans_text))

    return values, cases, None


def problem_form_html(action, values, testcases=(), extra_rows=0, error=None,
                      with_testcases=False):
    """题目表单。新建页和编辑页共用它，靠 with_testcases 区分。

    values 是字段字典 —— 报错重画时把**管理员刚填的内容原样放回去**，
    不让他白填一遍（这是表单页的基本礼貌）。
    """
    html = nav()
    if error is not None:
        html += f"<p style='color:#c00;font-weight:bold'>保存失败：{escape(error)}</p>"

    html += f"<form method='POST' action='{escape(action)}'>"

    for name, label, rows in PROBLEM_FIELDS:
        text = escape(values.get(name, ""))
        if rows == 1:
            html += f"<p>{label}<br><input name='{name}' size='60' value=\"{text}\"></p>"
        else:
            html += f"<p>{label}<br><textarea name='{name}' rows='{rows}' cols='80'>{text}</textarea></p>"

    html += (
        "<p>时间限制（毫秒）<input name='time_limit_ms' size='8'"
        f" value=\"{escape(values.get('time_limit_ms', ''))}\">　"
        "内存限制（MB）<input name='memory_limit_mb' size='8'"
        f" value=\"{escape(values.get('memory_limit_mb', ''))}\"></p>"
    )

    if with_testcases:
        checked = " checked" if values.get("is_published") else ""
        html += (
            "<p><label><input type='checkbox' name='is_published' value='1'"
            f"{checked}> 发布（不勾 = 草稿，学生看不到）</label></p>"
        )

        html += "<h3>测试点</h3>"
        html += (
            "<p>判题按 1、2、3…… 顺序跑，遇到第一个不通过就停。<br>"
            "<b>序号不用你操心</b>：保存时服务端按顺序统一重新编号。"
            "要删中间的某一组，把它的两个框清空即可，后面的会自动补上来。</p>"
        )

        # 现有测试点之后再垫一个空位（extra_rows 来自「＋ 再加一个空位」链接）
        blocks = list(testcases) + [("", "")] * (extra_rows + 1)
        for i, (in_text, ans_text) in enumerate(blocks, start=1):
            html += f"<h4>第 {i} 组</h4>"
            html += ("<p>输入<br><textarea name='tc_in' rows='4' cols='80'>"
                     f"{escape(in_text)}</textarea></p>")
            html += ("<p>答案<br><textarea name='tc_ans' rows='4' cols='80'>"
                     f"{escape(ans_text)}</textarea></p>")
        # 纯链接，不需要 JavaScript —— 点一下就再多一个空位（POST 时这个 ?rows= 也在）
        html += f"<p><a href='{escape(action)}?rows={extra_rows + 1}'>＋ 再加一个空位</a></p>"

    html += "<p><button type='submit'>保存</button></p></form>"
    return html


@app.route("/")
def home():
    return nav() + "Hello, OJ!"


@app.route("/problems")
def problems():
    rows = db.list_problems()

    html = nav() + "<h2>题目列表</h2>"

    # 一道题都没有时，显示"暂无题目"，不能报错（文档 OJ-PRB-01 异常流程）
    if not rows:
        return html + "<p>暂无题目。</p>"

    html += "<table border='1' cellpadding='6'>"
    html += "<tr><th>题号</th><th>标题</th><th></th></tr>"
    for problem_id, title in rows:
        html += (
            "<tr>"
            f"<td>{problem_id}</td>"
            # 标题是别人（管理员）输进来的，必须转义
            f"<td>{escape(title)}</td>"
            f"<td><a href='/problem/{problem_id}'>查看</a></td>"
            "</tr>"
        )
    return html + "</table>"


@app.route("/problem/<int:problem_id>")
def problem(problem_id):
    row = db.get_problem(problem_id)
    if row is None:
        return nav() + "没有这道题", 404

    (pid, title, desc, in_fmt, out_fmt, s_in, s_out, tl, ml) = row

    html = nav()
    html += f"<h2>{pid}. {escape(title)}</h2>"
    # white-space:pre-wrap —— 让题面里的换行和缩进原样显示（文档 OJ-PRB-02 要求）
    html += f"<div style='white-space:pre-wrap'>{escape(desc)}</div>"
    html += f"<h3>输入格式</h3><div style='white-space:pre-wrap'>{escape(in_fmt)}</div>"
    html += f"<h3>输出格式</h3><div style='white-space:pre-wrap'>{escape(out_fmt)}</div>"
    html += f"<h3>样例输入</h3><pre>{escape(s_in)}</pre>"
    html += f"<h3>样例输出</h3><pre>{escape(s_out)}</pre>"
    html += f"<p>时间限制：{tl} ms　　内存限制：{ml} MB</p>"
    html += f"<p>共 {db.count_testcases(pid)} 个测试点。</p>"

    # 下半部分：代码编辑框 + 提交按钮（文档 OJ-PRB-02 要求的版面）
    if session.get("username") is None:
        html += "<p><a href='/login'>登录</a>后才能提交本题。</p>"
    else:
        html += (
            "<h3>提交代码</h3>"
            f"<form method='POST' action='/submit/{pid}'>"
            "<textarea name='code' rows='14' cols='70'></textarea><br>"
            "<button type='submit'>提交</button>"
            "</form>"
        )

    html += "<p><a href='/problems'>← 回题目列表</a></p>"
    return html


@app.route("/admin/problems")
def admin_problems():
    deny = admin_guard()
    if deny is not None:
        return deny

    rows = db.list_all_problems()

    html = nav() + "<h2>题目管理</h2>"
    html += "<p><a href='/admin/problem/new'>＋ 新建题目</a></p>"
    if not rows:
        return html + "<p>还没有题目。</p>"

    html += "<table border='1' cellpadding='6'>"
    html += "<tr><th>题号</th><th>标题</th><th>状态</th><th>测试点</th><th></th></tr>"
    for problem_id, title, is_published, is_deleted in rows:
        if is_deleted:
            # 软删除的题连编辑入口都不给：update_problem 里那句
            # `AND is_deleted = 0` 会让改动影响 0 行 —— 界面上干脆别让人点
            state = "已删除（软删除）"
            actions = "<td colspan='2'>—</td>"
        else:
            state = "已发布" if is_published else "草稿（学生看不到）"
            actions = _edit_delete_cells(problem_id)
        html += (
            "<tr>"
            f"<td>{problem_id}</td>"
            f"<td>{escape(title)}</td>"
            f"<td>{escape(state)}</td>"
            f"<td>{db.count_testcases(problem_id)}</td>"
            f"{actions}"
            "</tr>"
        )
    return html + "</table>"


def _edit_delete_cells(problem_id):
    """一行的「编辑 / 删除」两个格子。"""
    return (
        f"<td><a href='/admin/problem/{problem_id}/edit'>编辑</a></td>"
        "<td>"
        # 删除必须走 POST：GET 在 HTTP 里的约定是「安全的」——浏览器会预取、
        # 爬虫会顺着链接爬。把删除做成一个 GET 链接，等于谁的爬虫扫一遍你的站，
        # 题就没了。（改和删同理，所以按钮都放在 <form> 里，不当链接用）
        f"<form method='POST' action='/admin/problem/{problem_id}/delete'"
        " style='display:inline'>"
        "<button type='submit'>删除</button></form>"
        "</td>"
    )


@app.route("/admin/problem/new", methods=["GET", "POST"])
def admin_new_problem():
    deny = admin_guard()
    if deny is not None:
        return deny

    if request.method == "POST":
        values, _cases, error = read_problem_form(request.form)
        if error is not None:
            return problem_form_html("/admin/problem/new", values, error=error), 400

        # 新建一律是草稿（create_problem 里 is_published 写死 0），
        # 所以这条路上根本没有「没有测试点却发布成功」的机会
        new_id = db.create_problem(
            values["title"], values["description"], values["input_format"],
            values["output_format"], values["sample_input"], values["sample_output"],
            values["time_limit_ms"], values["memory_limit_mb"],
        )
        # 存完就跳走（POST 完紧接着 redirect）：不跳的话，管理员一按 F5，
        # 浏览器会把这份表单**再交一遍**，库里就多出一道一模一样的题
        return redirect(f"/admin/problem/{new_id}/edit")

    return problem_form_html("/admin/problem/new",
                             {name: "" for name, _, _ in PROBLEM_FIELDS})


@app.route("/admin/problem/<int:problem_id>/edit", methods=["GET", "POST"])
def admin_edit_problem(problem_id):
    deny = admin_guard()
    if deny is not None:
        return deny

    row = db.get_problem_for_admin(problem_id)
    if row is None:
        return nav() + "没有这道题", 404
    if row[10]:     # is_deleted
        return nav() + ("这道题已删除（软删除），不能再编辑。"
                        "<a href='/admin/problems'>回题目管理</a>"), 403

    action = f"/admin/problem/{problem_id}/edit"

    if request.method == "POST":
        values, cases, error = read_problem_form(request.form)
        publish = request.form.get("is_published") == "1"
        values["is_published"] = publish

        # 文档 OJ-PRB-03：题目必须先配置至少一个测试点才能发布
        # （否则判题器拿不到数据，交上来一律不通过，见 OJ-JDG-06）
        if error is None and publish and not cases:
            error = "这道题一个测试点都没有，不能发布 —— 请先填好测试点，再勾「发布」"
        if error is not None:
            return problem_form_html(action, values, cases, error=error,
                                     with_testcases=True), 400

        # ⚠️ 顺序：先落盘，再改库。
        # 判题器只认磁盘，磁盘才是「真相」，库里的元信息只是账本。
        # 万一写到一半断了：磁盘是对的、账本偏旧 —— 判出来的结果**是准的**，
        # 只是页面上「共 N 个测试点」的数字偏小。
        # 反过来先记账本：页面写着 5 组、判题器手里只有 3 组数据，那是**判错**，
        # 性质严重得多。两边都可能不一致时，挑「错得轻」的那边。
        meta = save_testcases_to_disk(problem_id, cases)
        db.update_problem(
            problem_id, values["title"], values["description"],
            values["input_format"], values["output_format"],
            values["sample_input"], values["sample_output"],
            values["time_limit_ms"], values["memory_limit_mb"], publish,
        )
        db.replace_testcases(problem_id, meta)
        return redirect("/admin/problems")

    values = {
        "title": row[1], "description": row[2], "input_format": row[3],
        "output_format": row[4], "sample_input": row[5], "sample_output": row[6],
        "time_limit_ms": row[7], "memory_limit_mb": row[8], "is_published": row[9],
    }
    extra_rows = request.args.get("rows", type=int) or 0
    return problem_form_html(action, values, load_testcases_from_disk(problem_id),
                             extra_rows=extra_rows, with_testcases=True)


@app.route("/admin/problem/<int:problem_id>/delete", methods=["POST"])
def admin_delete_problem(problem_id):
    deny = admin_guard()
    if deny is not None:
        return deny

    # 只是标记，不是真删（见 db.soft_delete_problem 里的理由）
    db.soft_delete_problem(problem_id)
    return redirect("/admin/problems")


@app.route("/submit")
def submit_bare():
    """没有题号的裸提交页不存在了。交题必须从题目详情页进去 —— 这就是 FIX-09 的药。"""
    return redirect("/problems")


@app.route("/submit/<int:problem_id>", methods=["GET", "POST"])
def submit(problem_id):
    # 第一行就拦：没登录的话，看表单和交代码都不行
    if session.get("username") is None:
        return redirect("/login")

    if db.get_problem(problem_id) is None:
        return nav() + "没有这道题", 404

    if request.method == "POST":
        code = request.form["code"]
        if not code.strip():
            return "代码不能为空"

        work_dir = tempfile.mkdtemp()
        src_path = os.path.join(work_dir, "submission.cpp")
        with open(src_path, "w", encoding="utf-8") as f:
            f.write(code)

        job_id = uuid.uuid4().hex[:8]

        # 三个参数：编号、谁交的、交的哪道题
        db.insert_job(job_id, session.get("username"), problem_id)   # 先落库，再排队
        job_queue.put((job_id, work_dir, problem_id))   # 题号也塞进队列，工人要知道判哪道题

        # 存完就跳走（PRG）—— 不跳的话，学生在这个页面上按 F5，
        # 浏览器会把刚才那个 POST **重发一遍**：同一份代码交两次、白判一次题（FIX-22）。
        # 跳过去之后地址栏是一个普通 GET，F5 只是重新看一眼结果页。
        return redirect(f"/result/{job_id}")

    return nav() + f"""
    <h2>提交到第 {problem_id} 题</h2>
    <form method="POST">
      <textarea name="code" rows="12" cols="70"></textarea><br>
      <button type="submit">提交</button>
    </form>
    """


@app.route("/submissions")
def submissions():
    rows = db.list_jobs()

    html = nav() + "<h2>提交记录</h2><table border='1' cellpadding='6'>"
    html += "<tr><th>编号</th><th>题目</th><th>提交者</th><th>状态</th><th>提交时间</th><th></th></tr>"
    for job_id, username, problem_id, title, status, created_at in rows:
        t = created_at.strftime("%m-%d %H:%M:%S")
        # username 是 None 就显示「游客」——老数据没记过，不知道是谁
        who = username if username is not None else "游客"
        # problem_id 是 NULL = 这一列引入之前的老数据（跟 username 那个 NULL 不是一回事）
        which = f"第 {problem_id} 题 {title}" if problem_id is not None else "（老数据·无题号）"
        # 这几格都来自数据库，一律转义。不逐个判断「这个安不安全」——省得漏
        html += (
            "<tr>"
            f"<td>{escape(job_id)}</td>"
            f"<td>{escape(which)}</td>"
            f"<td>{escape(who)}</td>"
            f"<td>{escape(status)}</td>"
            f"<td>{escape(t)}</td>"
            f"<td><a href='/result/{escape(job_id)}'>查看</a></td>"
            "</tr>"
        )
    html += "</table>"
    html += "<p>一共 " + str(len(rows)) + " 条。</p>"
    return html


@app.route("/result/<job_id>")
def result(job_id):
    job = db.get_job(job_id)
    if job is None:
        return nav() + "没有这个编号", 404

    status, text, created_at = job
    waited = (datetime.datetime.now() - created_at).total_seconds()

    if status == "完成":
        # text 是判题器输出，里面含编译器照抄的用户源码 —— 必须转义（文档 OJ-PRB-02）
        return nav() + f"<pre>{escape(status)}\n\n{escape(text)}</pre>"

    if waited > JOB_TIMEOUT:
        return nav() + (
            "<p>判题异常：已经等了 "
            + str(round(waited))
            + " 秒还没出结果，请联系管理员。</p>"
        )

    return nav() + (
        "<meta http-equiv='refresh' content='2'>"
        "<p>当前状态：" + status + " ……（页面每 2 秒自动刷新）</p>"
    )


@app.route("/register", methods=["GET", "POST"])
def register():
    if request.method == "POST":
        username = request.form["username"].strip()
        password = request.form["password"]

        if not username or not password:
            return "用户名和密码都不能为空"

        pw_hash = generate_password_hash(password)
        try:
            db.create_user(username, pw_hash)
        except pymysql.err.IntegrityError:
            return "这个用户名已经被注册了"

        return nav() + "<p>注册成功，现在可以登录了。</p>"

    return nav() + """
    <form method="POST">
      <p>用户名：<input name="username"></p>
      <p>密码：<input name="password" type="password"></p>
      <button type="submit">注册</button>
    </form>
    """


@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form["username"].strip()
        password = request.form["password"]

        row = db.get_user_by_name(username)

        # 用户不存在 和 密码不对，故意返回同一句话
        if row is None or not check_password_hash(row[2], password):
            return "用户名或密码不对"

        # 发票：把「你是谁」写进 cookie，浏览器会自动存下来
        session["username"] = username
        return nav() + "<p>登录成功。去 <a href='/submit'>提交</a> 一题试试。</p>"

    return nav() + """
    <form method="POST">
      <p>用户名：<input name="username"></p>
      <p>密码：<input name="password" type="password"></p>
      <button type="submit">登录</button>
    </form>
    """


@app.route("/whoami")
def whoami():
    # 取票 + 验签名 + 读票面，Flask 一步做完。注意：没有一行数据库操作
    name = session.get("username")
    if name is None:
        return nav() + "<p>服务器认为你是：陌生人（没带票，或者票是假的）</p>"
    return nav() + f"<p>服务器认为你是：{escape(name)}</p>"


@app.route("/logout")
def logout():
    # 「作废」这张票 —— 其实只是让浏览器把它扔掉
    session.pop("username", None)   # 必须在 nav() 之前——否则导航栏还显示着名字
    return nav() + "<p>已退出登录</p>"


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
