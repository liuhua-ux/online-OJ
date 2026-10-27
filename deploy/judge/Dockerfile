FROM gcc:13-bookworm

# 判题器在镜像里编译，避免宿主机与镜像的 glibc 版本不一致
COPY judge_demo/judge.cpp /tmp/judge.cpp
RUN mkdir -p /opt/judge && g++ -O2 -o /opt/judge/judge /tmp/judge.cpp && rm /tmp/judge.cpp && chmod 755 /opt/judge/judge

WORKDIR /work
ENTRYPOINT []
