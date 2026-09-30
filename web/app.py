import os
import queue
import shutil
import subprocess
import tempfile
import threading
import uuid

from flask import Flask, request

app = Flask(__name__)

JUDGE_DIR = "/home/ubuntu/online-OJ/judge_demo"
JUDGE_BIN = os.path.join(JUDGE_DIR, "judge")
TESTDATA_DIR = os.path.join(JUDGE_DIR, "testdata")

# ---------- 任务表：编号 -> 这条提交的状态 ----------
jobs = {}
jobs_lock = threading.Lock()

# ---------- 队列：还没判的提交排在这里 ----------
job_queue = queue.Queue()


def worker():
    """后台工人：不停从队列里取任务，做完把结果写回任务表。"""
    while True:
        job_id, work_dir = job_queue.get()
        src_path = os.path.join(work_dir, "submission.cpp")

        with jobs_lock:
            jobs[job_id]["status"] = "判题中"

        try:
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

        with jobs_lock:
            jobs[job_id]["status"] = "完成"
            jobs[job_id]["result"] = text


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

        with jobs_lock:
            jobs[job_id] = {"status": "排队中", "result": None}

        job_queue.put((job_id, work_dir))

        return (
            "<p>已接收，编号 " + job_id + "</p>"
            "<p><a href='/result/" + job_id + "'>查看结果</a></p>"
        )

    return """
    <form method="POST">
      <textarea name="code" rows="12" cols="70"></textarea><br>
      <button type="submit">提交</button>
    </form>
    """


@app.route("/result/<job_id>")
def result(job_id):
    with jobs_lock:
        job = jobs.get(job_id)
        if job is None:
            return "没有这个编号", 404
        status = job["status"]
        text = job["result"]

    if status == "完成":
        return "<pre>" + status + "\n\n" + text + "</pre>"

    return "<p>当前状态：" + status + "</p>"


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
