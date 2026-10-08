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
    """返回 (提交者, 状态, 判题结果, 提交时间)，没有就返回 None。

    为什么要带出「提交者」：`/result` 要靠它回答「**这条是不是你的**」
    （需求 OJ-REC-01：用户只能查看自己的提交记录）。老数据的这一列是 NULL。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT username, status, result, created_at"
                " FROM submissions WHERE job_id = %s",
                (job_id,),
            )
            return cur.fetchone()
    finally:
        conn.close()


def list_jobs(owner=None, limit=100):
    """列提交记录，最新的排最前面，顺带把题目标题也带出来。

    owner 传用户名 → 只列那个人的（需求 OJ-REC-02：「学生（仅本人）」）；
    owner 传 None → 列全部（管理员）。
    limit：需求 OJ-REC-02 要求分页，这里先只加个上限，免得数据一多就全表扫描。

    用 LEFT JOIN 不是 INNER JOIN —— 老提交的 problem_id 是 NULL，
    INNER JOIN 会把它们整行丢掉（记录凭空消失）。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            sql = (
                "SELECT s.job_id, s.username, s.problem_id, p.title, s.status, s.created_at"
                " FROM submissions s"
                " LEFT JOIN problems p ON s.problem_id = p.problem_id"
            )
            params = []
            if owner is not None:
                sql += " WHERE s.username = %s"
                params.append(owner)
            sql += " ORDER BY s.created_at DESC, s.job_id DESC LIMIT %s"
            params.append(limit)
            cur.execute(sql, params)
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
    """返回 (user_id, 用户名, 口令哈希, 是否管理员, 是否禁用, 注册时间)，没这人返回 None。

    ⚠️ `password_hash` 必须**留在索引 2** —— `/login` 里写的是 `row[2]`。
    往后追加字段不会破坏它，但**往中间插字段就会**（那正是"接缝裂开"的典型）。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT user_id, username, password_hash, is_admin, is_disabled, created_at"
                " FROM users WHERE username = %s",
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


def get_problem_limits(problem_id):
    """只要这道题的时限和内存上限 —— 判题时要传给判题器（需求 OJ-PRB-02：「不得仅作展示」）。

    返回 (time_limit_ms, memory_limit_mb)；题不存在、或已软删除 → 返回 None。

    单独一个函数、只查两列：`/submit` 每次都要调它，没必要把题面那种大字段（MEDIUMTEXT）
    也拖出来。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT time_limit_ms, memory_limit_mb FROM problems"
                " WHERE problem_id = %s AND is_deleted = 0",
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


# ---------- 用户：改口令 / 管理（OJ-USR-03 / OJ-SYS-01） ----------

def update_password(user_id, password_hash):
    """改口令。**只动这一列** —— 角色、禁用状态都不受影响。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE users SET password_hash = %s WHERE user_id = %s",
                (password_hash, user_id),
            )
        conn.commit()
    finally:
        conn.close()


def list_users():
    """用户管理页用：列出所有人。
    返回 [(user_id, 用户名, 是否管理员, 是否禁用, 注册时间), ...]

    带上 user_id 是为了**当路由参数用**：用户名可能含空格等字符，
    塞进 URL 要额外编码，用数字主键省掉这一层。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT user_id, username, is_admin, is_disabled, created_at FROM users"
                " ORDER BY user_id"
            )
            return cur.fetchall()
    finally:
        conn.close()


def set_disabled(user_id, disabled):
    """禁用 / 解禁账号。

    禁用只是「**不让他登录**」，行本身留着 —— 需求 OJ-SYS-01 明确
    「禁用账号后该用户不能登录，但其**历史提交记录保留**」。
    跟题目软删除是同一个道理：删人就等于让他的提交变成孤儿。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE users SET is_disabled = %s WHERE user_id = %s",
                (1 if disabled else 0, user_id),
            )
        conn.commit()
    finally:
        conn.close()


def get_user_by_id(user_id):
    """按主键查人。返回 (user_id, 用户名, 是否管理员, 是否禁用)，没有返回 None。
    管理页的操作都走主键，避免用户名里的特殊字符进 URL。"""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT user_id, username, is_admin, is_disabled FROM users"
                " WHERE user_id = %s",
                (user_id,),
            )
            return cur.fetchone()
    finally:
        conn.close()


def set_admin(user_id, is_admin):
    """授予 / 收回管理员。

    需求 OJ-USR-01：「管理员角色**不可自助注册**，只能由既有管理员授予」——
    所以这个函数只从管理页调用。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE users SET is_admin = %s WHERE user_id = %s",
                (1 if is_admin else 0, user_id),
            )
        conn.commit()
    finally:
        conn.close()


def count_active_admins():
    """还有几个**能用的**管理员（没被禁用的）。

    需求 OJ-SYS-01：「系统中**至少保留一个可用管理员账号**」——
    禁用或降权之前先问一下这个函数，别把最后一把钥匙扔了。
    """
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT COUNT(*) FROM users WHERE is_admin = 1 AND is_disabled = 0"
            )
            return cur.fetchone()[0]
    finally:
        conn.close()
