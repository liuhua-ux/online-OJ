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


def insert_job(job_id, username, problem_id):
    """username 是提交者的用户名（没登录传 None）；problem_id 是交给哪道题。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO submissions (job_id, username, problem_id, status, result, created_at)"
                " VALUES (%s, %s, %s, %s, NULL, NOW())",
                (job_id, username, problem_id, "排队中"),
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
    """最新的排最前面，顺带把题目标题也带出来。

    用 LEFT JOIN 不是 INNER JOIN —— 老提交的 problem_id 是 NULL，
    INNER JOIN 会把它们整行丢掉（记录凭空消失）。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT s.job_id, s.username, s.problem_id, p.title, s.status, s.created_at"
                " FROM submissions s"
                " LEFT JOIN problems p ON s.problem_id = p.problem_id"
                " ORDER BY s.created_at DESC, s.job_id DESC"
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


def list_problems():
    """列出「学生看得见」的题目：没被软删除、且已发布。返回 (problem_id, title) 列表。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT problem_id, title FROM problems"
                " WHERE is_deleted = 0 AND is_published = 1"
                " ORDER BY problem_id"
            )
            return cur.fetchall()
    finally:
        conn.close()


def get_problem(problem_id):
    """查一道题的完整内容。已软删除的题当它不存在（返回 None）。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT problem_id, title, description, input_format, output_format,"
                " sample_input, sample_output, time_limit_ms, memory_limit_mb"
                " FROM problems WHERE problem_id = %s AND is_deleted = 0",
                (problem_id,),
            )
            return cur.fetchone()
    finally:
        conn.close()


def count_testcases(problem_id):
    """这道题有几组测试点，返回一个整数。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT COUNT(*) FROM testcases WHERE problem_id = %s",
                (problem_id,),
            )
            return cur.fetchone()[0]
    finally:
        conn.close()


def is_admin(username):
    """这个人是不是管理员。

    **每次现查数据库，不看 cookie** —— 权限放进 cookie 的话，
    把某人降权后，他手里那张旧票照样是管理员，直到票过期。
    跟 /logout 作废不了票是同一个道理。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT is_admin FROM users WHERE username = %s",
                (username,),
            )
            row = cur.fetchone()
            return row is not None and row[0] == 1
    finally:
        conn.close()


def list_all_problems():
    """管理页用：连**未发布**和**已软删除**的也列出来（学生看不到的那些）。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT problem_id, title, is_published, is_deleted FROM problems"
                " ORDER BY problem_id"
            )
            return cur.fetchall()
    finally:
        conn.close()


def get_problem_for_admin(problem_id):
    """管理页专用：**软删除的也查得到** —— 管理员要看得见自己删过什么。
    （学生侧走 get_problem()，那个会把软删除的当不存在。）

    返回 (题号, 标题, 题面, 输入格式, 输出格式, 样例输入, 样例输出,
          时限ms, 内存MB, 已发布, 已删除)，没有这行返回 None。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT problem_id, title, description, input_format, output_format,"
                " sample_input, sample_output, time_limit_ms, memory_limit_mb,"
                " is_published, is_deleted"
                " FROM problems WHERE problem_id = %s",
                (problem_id,),
            )
            return cur.fetchone()
    finally:
        conn.close()


def create_problem(title, description, input_format, output_format,
                   sample_input, sample_output, time_limit_ms, memory_limit_mb):
    """新建一道题，**一律建成草稿**（is_published 写死 0）。返回新题号。

    「题目必须先配置至少一个测试点才能发布」是硬规则（文档 OJ-PRB-03），
    而刚新建的题必然一个测试点都没有 —— 那就干脆不给它发布的机会：
    要发布，只能先去编辑页把测试点配好、再回来勾选。
    这样「无测试点却已发布」这个非法状态根本无从产生。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO problems (title, description, input_format, output_format,"
                " sample_input, sample_output, time_limit_ms, memory_limit_mb,"
                " is_published, is_deleted, created_at)"
                " VALUES (%s, %s, %s, %s, %s, %s, %s, %s, 0, 0, NOW())",
                (title, description, input_format, output_format,
                 sample_input, sample_output, time_limit_ms, memory_limit_mb),
            )
            new_id = cur.lastrowid
        conn.commit()
        return new_id
    finally:
        conn.close()


def update_problem(problem_id, title, description, input_format, output_format,
                   sample_input, sample_output, time_limit_ms, memory_limit_mb,
                   is_published):
    """改一道题。

    `WHERE ... AND is_deleted = 0`：软删除的题不许再改 —— 它对学生已经不存在了，
    改它只会让人以为「删掉的题还能编辑」。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE problems SET title = %s, description = %s, input_format = %s,"
                " output_format = %s, sample_input = %s, sample_output = %s,"
                " time_limit_ms = %s, memory_limit_mb = %s, is_published = %s"
                " WHERE problem_id = %s AND is_deleted = 0",
                (title, description, input_format, output_format, sample_input,
                 sample_output, time_limit_ms, memory_limit_mb, is_published,
                 problem_id),
            )
        conn.commit()
    finally:
        conn.close()


def soft_delete_problem(problem_id):
    """软删除：把 is_deleted 标成 1，**行本身还在库里**。

    为什么不能直接 DELETE：submissions.problem_id 有外键指过来，题行一没，
    历史提交就再也配不上题目标题，在 /submissions 上变成一堆「孤儿记录」。
    顺手把 is_published 清成 0 —— 删掉的题不该还挂着「已发布」。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE problems SET is_deleted = 1, is_published = 0"
                " WHERE problem_id = %s",
                (problem_id,),
            )
        conn.commit()
    finally:
        conn.close()


def list_testcases(problem_id):
    """这道题的测试点元信息，按序号排。返回 [(序号, 输入相对路径, 答案相对路径), ...]"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT ordinal, input_path, answer_path FROM testcases"
                " WHERE problem_id = %s ORDER BY ordinal",
                (problem_id,),
            )
            return cur.fetchall()
    finally:
        conn.close()


def replace_testcases(problem_id, items):
    """把这道题的测试点元信息**整份换掉**。

    items = [(输入相对路径, 答案相对路径, 输入字节数, 答案字节数), ...]，
    按列表顺序从 1 开始重新编号 —— 序号由服务端统一生成，不靠管理员手工命名。

    先 DELETE 再 INSERT，两步在**同一个事务**里：这条连接是默认不自动提交的，
    中间任何一步失败，异常会冒出去、连接关闭时回滚，**原来的元信息一条没少**。
    不放进一个事务的话，DELETE 完 INSERT 挂掉 = 元信息清空、新的一条没写，
    这道题的测试点在库里就整个消失了。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM testcases WHERE problem_id = %s", (problem_id,))
            for ordinal, paths in enumerate(items, start=1):
                in_path, ans_path, in_size, ans_size = paths
                cur.execute(
                    "INSERT INTO testcases (problem_id, ordinal, input_path, answer_path,"
                    " input_size, answer_size) VALUES (%s, %s, %s, %s, %s, %s)",
                    (problem_id, ordinal, in_path, ans_path, in_size, ans_size),
                )
        conn.commit()
    finally:
        conn.close()
