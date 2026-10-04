"""所有数据库操作的唯一入口。app.py 不直接碰账号密码。"""
import pymysql

from db_config import DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_NAME


def connect():
    """开一条新连接。每次操作完就关掉 —— 连接不是线程安全的，
    这样每个请求/每个工人各用各的，互不干扰。"""
    return pymysql.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASSWORD,
        database=DB_NAME,
        charset="utf8mb4",
    )


def insert_job(job_id, username):
    """username 是提交者的用户名；没登录就传 None。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO submissions (job_id, username, status, result, created_at)"
                " VALUES (%s, %s, %s, NULL, NOW())",
                (job_id, username, "排队中"),
            )
        conn.commit()
    finally:
        conn.close()


def update_job(job_id, status, result):
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE submissions SET status = %s, result = %s WHERE job_id = %s",
                (status, result, job_id),
            )
        conn.commit()
    finally:
        conn.close()


def get_job(job_id):
    """返回 (status, result, created_at)，没有就返回 None。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT status, result, created_at FROM submissions WHERE job_id = %s",
                (job_id,),
            )
            return cur.fetchone()
    finally:
        conn.close()


def list_jobs():
    """最新的排最前面。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT job_id, username, status, created_at FROM submissions"
                " ORDER BY created_at DESC, job_id DESC"
            )
            return cur.fetchall()
    finally:
        conn.close()


def create_user(username, password_hash):
    """新建用户。用户名重复时，MySQL 会拒绝，这里直接让异常冒出去。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO users (username, password_hash, created_at)"
                " VALUES (%s, %s, NOW())",
                (username, password_hash),
            )
        conn.commit()
    finally:
        conn.close()


def get_user_by_name(username):
    """返回 (user_id, username, password_hash)，没这个用户就返回 None。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT user_id, username, password_hash FROM users WHERE username = %s",
                (username,),
            )
            return cur.fetchone()
    finally:
        conn.close()
