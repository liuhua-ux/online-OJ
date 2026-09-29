import subprocess

result = subprocess.run(
    ["./judge", "test.cpp", "testdata"],
    cwd="/home/ubuntu/online-OJ/judge_demo",
    capture_output=True,
    text=True,
)

print("exit code:", result.returncode)
print("----")
print(result.stdout)