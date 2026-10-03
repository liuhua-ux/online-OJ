import datetime
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import uuid

import pymysql
from flask import Flask, request, session
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


def worker():
    """后台工人：不停从队列里取任务，判完把结果写回数据库。"""
    while True:
        job_id, work_dir = job_queue.get()
        src_path = os.path.join(work_dir, "submission.cpp")

        try:
            db.update_job(job_id, "判题中", None)
            result = subprocess.run(
                [JUDGE_BIN, src_path, TESTDATA_DIR],
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


@app.route("/")
def home():
    return "Hello, OJ!"


@app.route("/submit", methods=["GET", "POST"])
def submit():
    if request.method == "POST":
        code = request.form["code"]
        if not code.strip():
            return "代码不能为空"

        work_dir = tempfile.mkdtemp()
        src_path = os.path.join(work_dir, "submission.cpp")
        with open(src_path, "w", encoding="utf-8") as f:
            f.write(code)

        job_id = uuid.uuid4().hex[:8]

        db.insert_job(job_id)          # 先落库，再排队
        job_queue.put((job_id, work_dir))

        return (
            "<p>已接收，编号 " + job_id + "</p>"
            "<p><a href='/result/" + job_id + "'>查看结果</a>　"
            "<a href='/submissions'>提交记录</a></p>"
        )

    return """
    <form method="POST">
      <textarea name="code" rows="12" cols="70"></textarea><br>
      <button type="submit">提交</button>
    </form>
    <p><a href='/submissions'>提交记录</a></p>
    """


@app.route("/submissions")
def submissions():
    rows = db.list_jobs()

    html = "<h2>提交记录</h2><table border='1' cellpadding='6'>"
    html += "<tr><th>编号</th><th>状态</th><th>提交时间</th><th></th></tr>"
    for job_id, status, created_at in rows:
        t = created_at.strftime("%m-%d %H:%M:%S")
        html += (
            "<tr>"
            "<td>" + job_id + "</td>"
            "<td>" + status + "</td>"
            "<td>" + t + "</td>"
            "<td><a href='/result/" + job_id + "'>查看</a></td>"
            "</tr>"
        )
    html += "</table>"
    html += "<p>一共 " + str(len(rows)) + " 条。</p>"
    html += "<p><a href='/submit'>再提交一次</a>　<a href='/'>首页</a></p>"
    return html


@app.route("/result/<job_id>")
def result(job_id):
    job = db.get_job(job_id)
    if job is None:
        return "没有这个编号", 404

    status, text, created_at = job
    waited = (datetime.datetime.now() - created_at).total_seconds()

    if status == "完成":
        return "<pre>" + status + "\n\n" + text + "</pre>"

    if waited > JOB_TIMEOUT:
        return (
            "<p>判题异常：已经等了 "
            + str(round(waited))
            + " 秒还没出结果，请联系管理员。</p>"
        )

    return (
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

        return "<p>注册成功</p><p><a href='/register'>再注册一个</a></p>"

    return """
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
        return "<p>登录成功</p>\n" \
               "<p><a href='/whoami'>看看服务器现在认为你是谁</a></p>"

    return """
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
        return "<p>服务器认为你是：陌生人（没带票，或者票是假的）</p>"
    return "<p>服务器认为你是：" + name + "</p>"


@app.route("/logout")
def logout():
    # 「作废」这张票 —— 其实只是让浏览器把它扔掉
    session.pop("username", None)
    return "<p>已退出登录</p><p><a href='/login'>重新登录</a></p>"


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
