import subprocess
from flask import Flask

app = Flask(__name__)

@app.route("/")
def home():
    return "Hello, OJ!"

@app.route("/submit")
def submit():
    result = subprocess.run(
        ["./judge", "test.cpp", "testdata"],
        cwd="/home/ubuntu/online-OJ/judge_demo",
        capture_output=True,
        text=True,
    )
    return "<pre>" + result.stdout + "</pre>"

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)