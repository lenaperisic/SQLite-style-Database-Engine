"""Tables, schema, types and the B+ tree under load.
Usage: python test_sql.py <path to the database program>"""
import random

from helpers import Tester, check_db, format_value

t = Tester()


def raw_record(db, page, cell=0):
    """Record bytes of a cell in a leaf page"""
    data = open(t.path(db), "rb").read()
    off = page * 4096 + 14 + cell * 297
    size = (data[off + 4] << 8) | data[off + 5]
    return data[off + 6:off + 6 + size]


print("== statements and errors")
out = t.run("a.db", [
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);",
    "create table Orders (id int primary key, user_id integer, item text)",
    "INSERT INTO users VALUES (1, 'ana', 25);",
    "insert into USERS values (null, 'marko', '31')",
    "INSERT INTO orders VALUES (1, 1, 'knjiga');",
    "SELECT * FROM users", "select * from ORDERS;",
    ".tables", ".schema",
])
t.expect("output", out, [
    "Executed."] * 5 + [
    "(1, ana, 25)", "(2, marko, 31)", "Executed.",
    "(1, 1, knjiga)", "Executed.",
    "Orders", "users",
    "create table Orders (id int primary key, user_id integer, item text);",
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);"])
t.expect("record of (1, 'ana', 25) is the todo.md example", raw_record("a.db", 2).hex(" "), "04 00 13 01 61 6e 61 19")
t.valid("after create + insert", "a.db")

errors = t.run("a.db", [
    "SELECT * FROM nema", "INSERT INTO users VALUES (1, 'x', 1)", "INSERT INTO users VALUES (5, 'x')",
    "CREATE TABLE USERS (a INT)", "CREATE TABLE t (a INT, A TEXT)", "CREATE TABLE t (a FOO)",
    "CREATE TABLE t (a TEXT PRIMARY KEY)", "CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)",
    "CREATE TABLE t ()", "CREATE TABLE t (a INT", "INSERT INTO users VALUES (1, ana, 2)",
    "INSERT INTO users VALUES ('x', 'a', 1)", "INSERT INTO users VALUES (3.5, 'a', 1)",
    "INSERT INTO users VALUES (-1, 'a', 1)", "INSERT INTO users VALUES (4294967296, 'a', 1)",
    "INSERT INTO users VALUES (9, 'a', 1) extra", "SELECT FROM users", "TRUNCATE users",
    "DROP TABLE nema", "INSERT INTO users VALUES (9, 'unterminated, 1)",
    "INSERT INTO users VALUES (9, x'ABC', 1)", ".foo",
])
t.expect("error messages", errors, [
    "Error: no such table: nema",
    "Error: Duplicate key.",
    "Error: table users has 3 columns but 2 values were supplied.",
    "Error: table USERS already exists.",
    "Error: duplicate column name: A",
    "Syntax error: unknown type FOO.",
    "Syntax error: only INTEGER PRIMARY KEY is supported.",
    "Syntax error: table has more than one primary key.",
    "Syntax error: expected column name.",
    "Syntax error: expected ')'.",
    "Syntax error: expected a value, got ana (text needs quotes: 'ana').",
    "Error: datatype mismatch: id must be an integer.",
    "Error: datatype mismatch: id must be an integer.",
    "ID must be positive.",
    "Error: ID is too large (max 4294967295).",
    "Syntax error: unexpected 'extra' at the end.",
    "Syntax error: expected FROM.",
    "Unrecognized keyword at start of 'TRUNCATE users'.",
    "Error: no such table: nema",
    "Syntax error: unterminated string.",
    "Syntax error: blob needs an even number of hex digits.",
    "Unrecognized command '.foo'",
])
t.expect("nothing changed by the errors", t.rows("a.db", "SELECT * FROM users"), ["(1, ana, 25)", "(2, marko, 31)"])
t.valid("after errors", "a.db")

print("== affinity and types")
t.run("t.db", ["CREATE TABLE t (id INTEGER PRIMARY KEY, i INTEGER, r REAL, s TEXT, b BLOB)",
               "INSERT INTO t VALUES (1, '42', 5, 42, x'00FF')",
               "INSERT INTO t VALUES (2, '4.5', '2.5', 3.5, 'txt')",
               "INSERT INTO t VALUES (3, 7.0, 'abc', NULL, 12)",
               "INSERT INTO t VALUES (4.0, 'xyz', -0.5, 'it''s', NULL)"])
t.expect("select", t.rows("t.db", "SELECT * FROM t"), [
    "(1, 42, 5.0, 42, x'00FF')",
    "(2, 4.5, 2.5, 3.5, txt)",
    "(3, 7, abc, NULL, 12)",
    "(4, xyz, -0.5, it's, NULL)"])
stored = [check_db.decode_record(raw_record("t.db", 2, c))[0] for c in range(4)]
t.expect("stored types", [[type(v).__name__ for v in r] for r in stored], [
    ["NoneType", "int", "float", "str", "bytes"],     # '42'->int, 5->real, 42->text, blob kept
    ["NoneType", "float", "float", "str", "str"],     # '4.5'->real in INTEGER col, 3.5->text
    ["NoneType", "int", "str", "NoneType", "int"],    # 7.0->7, 'abc' stays text in REAL, BLOB keeps 12
    ["NoneType", "str", "float", "str", "NoneType"]])

print("== table without a primary key")
t.run("np.db", ["CREATE TABLE log (msg TEXT, n INTEGER)",
                "INSERT INTO log VALUES ('a', 1)", "INSERT INTO log VALUES ('b', 2)", "INSERT INTO log VALUES ('c', 3)",
                "DELETE FROM log WHERE rowid = 2", "INSERT INTO log VALUES ('d', 4)"])
t.expect("auto rowid, delete by rowid", t.rows("np.db", "SELECT * FROM log"), ["(a, 1)", "(c, 3)", "(d, 4)"])
t.expect("rowid can be selected", t.rows("np.db", "SELECT rowid, msg FROM log"), ["(1, a)", "(3, c)", "(4, d)"])

print("== header and bad files")
open(t.path("junk.db"), "wb").write(b"x" * 4096)
t.expect("not a database", t.run("junk.db", []), ["Error: File is not a database (or an old format). Delete it and start again."])
open(t.path("half.db"), "wb").write(b"x" * 1000)
t.expect("not whole pages", t.run("half.db", []), ["Error: Db file is not a whole number of pages. Corrupt file."])
t.run("new.db", [])
t.expect("new file = header + schema root", t.pages("new.db"), 2)
t.valid("new file", "new.db")

print("== CREATE TABLE text too long for the schema row")
long_cols = ", ".join(f"column_number_{i} INTEGER" for i in range(12))
out = t.run("long.db", [f"CREATE TABLE t ({long_cols})", ".tables"])
t.expect("rejected, no table", [line.split(" (")[0] for line in out], ["Error: Row is too large"])
t.valid("no page lost", "long.db")

print("== two tables growing at the same time, then deletes and DROP")
rng = random.Random(3)
db = "multi.db"
t.run(db, ["CREATE TABLE a (id INTEGER PRIMARY KEY, v TEXT)", "CREATE TABLE b (id INTEGER PRIMARY KEY, v INTEGER, w REAL)"])
alive = {"a": set(), "b": set()}
for session in range(4):
    lines = ["BEGIN"]
    for _ in range(8000):
        name = rng.choice("ab")
        k = rng.randint(1, 12000)
        if rng.random() < (0.8 if session < 2 else 0.35):
            if k in alive[name]:
                continue
            lines.append(f"INSERT INTO a VALUES ({k}, 'value {k}')" if name == "a"
                         else f"INSERT INTO b VALUES ({k}, {k * 3}, {k / 4})")
            alive[name].add(k)
        elif k in alive[name]:
            lines.append(f"DELETE FROM {name} WHERE id = {k}")
            alive[name].discard(k)
    t.run(db, lines + ["COMMIT"])
    t.valid(f"session {session + 1} ({len(alive['a'])} + {len(alive['b'])} rows)", db,
            {"a": sorted(alive["a"]), "b": sorted(alive["b"])})
t.expect("table a content", t.rows(db, "SELECT * FROM a"), [f"({k}, value {k})" for k in sorted(alive["a"])])
t.expect("table b content", t.rows(db, "SELECT * FROM b"),
         [f"({k}, {k * 3}, {format_value(k / 4)})" for k in sorted(alive["b"])])

t.run(db, ["DROP TABLE a"])
summary = t.valid("after DROP TABLE a", db)
t.expect("only b left", sorted(summary), ["b"])
t.run(db, ["BEGIN", "CREATE TABLE c (id INTEGER PRIMARY KEY, v TEXT)"] +
      [f"INSERT INTO c VALUES ({k}, 'c{k}')" for k in range(1, len(alive["a"]) + 1)] + ["COMMIT"])
t.valid("c filled into a's old pages", db, {"b": sorted(alive["b"]), "c": list(range(1, len(alive["a"]) + 1))})
data = open(t.path(db), "rb").read()
t.expect("every page freed by DROP was reused (free list empty)", int.from_bytes(data[12:16], "little"), 0)

t.run(db, ["DELETE FROM b"])
t.valid("after DELETE FROM b (all rows)", db, {"b": []})
t.expect("b is empty", t.rows(db, "SELECT * FROM b"), [])
t.run(db, ["INSERT INTO b VALUES (1, 2, 3.5)"])
t.expect("b usable again", t.rows(db, "SELECT * FROM b"), ["(1, 2, 3.5)"])

print("== single big table, delete in order (B+ tree splits, borrows, merges)")
for name, order in [("asc", range(1, 15001)), ("desc", range(15000, 0, -1))]:
    db = f"big_{name}.db"
    keys = list(range(1, 15001))
    random.Random(9).shuffle(keys)
    t.run(db, ["CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, email TEXT)", "BEGIN"] +
          [f"INSERT INTO t VALUES ({k}, 'user{k}', 'person{k}@example.com')" for k in keys] + ["COMMIT"])
    t.valid(f"{name}: 15000 rows", db, {"t": list(range(1, 15001))})
    order = list(order)
    t.run(db, ["BEGIN"] + [f"DELETE FROM t WHERE id = {k}" for k in order[:10000]] + ["COMMIT"])
    left = sorted(order[10000:])
    t.valid(f"{name}: 10000 deleted", db, {"t": left})
    t.expect(f"{name}: content", t.rows(db, "SELECT * FROM t"), [f"({k}, user{k}, person{k}@example.com)" for k in left])

t.finish()
