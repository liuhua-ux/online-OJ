import os
import shutil
import subprocess
import tempfile

from flask import Flask, request

app = Flask(__name__)

JUDGE_DIR = "/home/ubuntu/online-OJ/judge_demo"
JUDGE_BIN = os.path.join(JUDGE_DIR, "judge")
TESTDATA_DIR = os.path.join(JUDGE_DIR, "testdata")


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

        result = subprocess.run(
            [JUDGE_BIN, src_path, TESTDATA_DIR],
            cwd=work_dir,
            capture_output=True,
            text=True,
        )

        shutil.rmtree(work_dir, ignore_errors=True)

        return "<pre>exit code: " + str(result.returncode) + "\n\n" + result.stdout + "</pre>"

    return """
    <form method="POST">
      <textarea name="code" rows="12" cols="70"></textarea><br>
      <button type="submit">提交</button>
    </form>
    """


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
