"""SELECT / UPDATE / DELETE with expressions, compared against real SQLite.

Random statements run on our database and on Python's sqlite3 module, and
every result has to be the same. The expressions keep numbers and text apart
(SQLite's rules for comparing a number with text are not implemented).
Usage: python test_query.py <path to the database program>"""
import random
import sqlite3

from helpers import Tester, format_row

t = Tester()

SCHEMA = "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b REAL, c TEXT)"
WORDS = ["ana", "marko", "it's", "Zeta", "b", "", "ana marija", "x1"]


def sql_text(word):
    return "'" + word.replace("'", "''") + "'"


class Generator:
    def __init__(self, seed):
        self.rng = random.Random(seed)

    def number(self):
        r = self.rng
        k = r.random()
        if k < 0.6:
            return str(r.randint(-20, 20))
        if k < 0.9:
            return repr(r.randint(-40, 40) / 4)
        return "NULL"

    def num_expr(self, depth=2):
        r = self.rng
        k = r.random()
        if depth == 0 or k < 0.35:
            return r.choice(["a", "b", "id"])
        if k < 0.55:
            return f"({self.number()})"
        if k < 0.65:
            return f"(- {self.num_expr(depth - 1)})"
        op = r.choice(["+", "-", "*", "/", "%"])
        return f"({self.num_expr(depth - 1)} {op} {self.num_expr(depth - 1)})"

    def text_expr(self):
        return "c" if self.rng.random() < 0.5 else sql_text(self.rng.choice(WORDS))

    def condition(self, depth=2):
        r = self.rng
        k = r.random()
        if depth == 0 or k < 0.4:
            kind = r.random()
            op = r.choice(["=", "!=", "<>", "<", "<=", ">", ">=", "=="])
            if kind < 0.55:
                return f"({self.num_expr(1)} {op} {self.num_expr(1)})"
            if kind < 0.8:
                return f"({self.text_expr()} {op} {self.text_expr()})"
            if kind < 0.95:
                return f"({r.choice(['a', 'b', 'c'])} IS {r.choice(['', 'NOT '])}NULL)"
            return self.num_expr(1)     # a number used as true / false
        if k < 0.5:
            return f"(NOT {self.condition(depth - 1)})"
        return f"({self.condition(depth - 1)} {r.choice(['AND', 'OR'])} {self.condition(depth - 1)})"

    def row_values(self, id_):
        r = self.rng
        c = "NULL" if r.random() < 0.15 else sql_text(r.choice(WORDS))
        return f"({id_}, {self.number()}, {self.number()}, {c})"

    def statement(self, next_id):
        r = self.rng
        k = r.random()
        if k < 0.25:
            return f"SELECT * FROM t WHERE {self.condition()}"
        if k < 0.4:
            order = r.choice(["", " DESC"])
            limit = f" LIMIT {r.randint(0, 30)}" + (f" OFFSET {r.randint(0, 10)}" if r.random() < 0.5 else "")
            return (f"SELECT id, {self.num_expr()}, c FROM t WHERE {self.condition(1)} "
                    f"ORDER BY {self.num_expr(1)}{order}, id{limit if r.random() < 0.6 else ''}")
        if k < 0.5:
            return (f"SELECT COUNT(*), COUNT(a), SUM(a), SUM(b), MIN(c), MAX(c), MIN(b), MAX(a), AVG(b), "
                    f"COUNT(*) * 2 + 1 FROM t WHERE {self.condition()}")
        if k < 0.55:
            return f"SELECT c, a FROM t ORDER BY c{r.choice(['', ' DESC'])}, id"
        if k < 0.6:
            key = r.randint(1, next_id)
            return f"SELECT * FROM t WHERE id = {key}" + (f" AND {self.condition(1)}" if r.random() < 0.5 else "")
        if k < 0.72:
            return f"UPDATE t SET a = {self.num_expr(1)}, b = {self.num_expr(1)} WHERE {self.condition()}"
        if k < 0.78:
            return f"UPDATE t SET c = {sql_text(r.choice(WORDS))}, a = b, b = a WHERE {self.condition(1)}"
        # A random condition can match most rows; "id % 5 = n" keeps each
        # delete to about a fifth at most, so the table doesn't run empty
        if k < 0.80:
            return f"DELETE FROM t WHERE {self.condition()} AND (id % 5 = {r.randint(0, 4)})"
        if k < 0.84:
            return f"DELETE FROM t WHERE id = {r.randint(1, next_id)}"
        return f"INSERT INTO t VALUES {self.row_values(next_id)}"


def sqlite_output(connection, statements):
    """What our program should print for these statements"""
    lines = []
    for sql in statements:
        try:
            cursor = connection.execute(sql)
            lines += [format_row(row) for row in cursor.fetchall()]
            lines.append("Executed.")
        except sqlite3.Error as e:
            lines.append(f"<sqlite error: {e}>")
    return lines


def split_output(lines, count):
    """Our output, one list of lines per statement"""
    chunks, current = [], []
    for line in lines:
        current.append(line)
        if not line.startswith("("):
            chunks.append(current)
            current = []
    return chunks + [current] * (count - len(chunks))


print("== random statements, compared with SQLite")
for seed in range(1, 5):
    gen = Generator(seed)
    db = f"rand{seed}.db"
    connection = sqlite3.connect(":memory:")

    setup = [SCHEMA] + [f"INSERT INTO t VALUES {gen.row_values(i)}" for i in range(1, 301)]
    statements = []
    next_id = 301
    for _ in range(700):
        sql = gen.statement(next_id)
        if sql.startswith("INSERT"):
            next_id += 1
        statements.append(sql)
    statements.append("SELECT * FROM t")

    sqlite_output(connection, setup)
    want = split_output(sqlite_output(connection, statements), len(statements))
    t.run(db, setup)
    got = split_output(t.run(db, statements), len(statements))

    def same(ours, theirs):
        # An error on both sides counts as the same result, the messages differ
        if len(theirs) == 1 and theirs[0].startswith("<sqlite error"):
            return len(ours) == 1 and ("rror" in ours[0] or ours[0].startswith("Unrecognized"))
        return ours == theirs

    mismatches = [i for i in range(len(statements)) if not same(got[i], want[i])]
    errors_both = sum(1 for w in want if len(w) == 1 and w[0].startswith("<sqlite error"))
    print(f"   ({errors_both} statements were errors on both sides)")
    for i in mismatches[:3]:
        print("   statement:", statements[i])
        print("   ours:  ", got[i][:5])
        print("   sqlite:", want[i][:5])
    t.expect(f"seed {seed}: {len(statements)} statements give the same results "
             f"({len(want[-1])} rows left at the end)", len(mismatches), 0)
    t.valid(f"seed {seed}", db)

print("== things that should fail")
t.run("e.db", [SCHEMA, "INSERT INTO t VALUES (1, 2, 3.5, 'x')", "INSERT INTO t VALUES (2, 5, NULL, 'y')"])
t.expect("errors", t.run("e.db", [
    "SELECT nope FROM t",
    "SELECT * FROM t WHERE COUNT(*) > 1",
    "SELECT a, COUNT(*) FROM t",
    "SELECT *, COUNT(*) FROM t",
    "SELECT * FROM t ORDER BY 9",
    "SELECT * FROM t LIMIT -1",
    "SELECT * FROM t LIMIT 'x'",
    "UPDATE t SET nope = 1",
    "UPDATE t SET a = SUM(a)",
    "SELECT SUM(COUNT(*)) FROM t",
    "SELECT * FROM t WHERE a IS 5",
    "SELECT (a FROM t",
]), [
    "Error: no such column: nope",
    "Syntax error: misuse of aggregate COUNT().",
    "Syntax error: can't mix aggregates and plain columns without GROUP BY.",
    "Syntax error: can't mix aggregates and plain columns without GROUP BY.",
    "Syntax error: ORDER BY term out of range.",
    "Syntax error: LIMIT needs a whole number >= 0.",
    "Syntax error: LIMIT needs a whole number >= 0.",
    "Error: no such column: nope",
    "Syntax error: misuse of aggregate SUM().",
    "Syntax error: misuse of aggregate COUNT().",
    "Syntax error: expected NULL.",
    "Syntax error: expected ')'.",
])
t.expect("aggregates on an empty result", t.rows("e.db", "SELECT COUNT(*), SUM(a), MIN(c), AVG(b) FROM t WHERE id > 5"),
         ["(0, NULL, NULL, NULL)"])

print("== UPDATE of the primary key")
t.run("pk.db", [SCHEMA] + [f"INSERT INTO t VALUES ({i}, {i}, NULL, 'r{i}')" for i in range(1, 6)])
t.expect("moving rows to new ids", t.run("pk.db", ["UPDATE t SET id = id + 100 WHERE id > 3", "SELECT id, c FROM t"]),
         ["Executed.", "(1, r1)", "(2, r2)", "(3, r3)", "(104, r4)", "(105, r5)", "Executed."])
t.expect("id taken by another row: nothing changes", t.run("pk.db", ["UPDATE t SET id = 2 WHERE id = 1", "SELECT id FROM t"]),
         ["Error: Duplicate key.", "(1)", "(2)", "(3)", "(104)", "(105)", "Executed."])
t.expect("two rows to the same id: nothing changes", t.run("pk.db", ["UPDATE t SET id = 50 WHERE id < 3", "SELECT id FROM t"]),
         ["Error: Duplicate key.", "(1)", "(2)", "(3)", "(104)", "(105)", "Executed."])
t.expect("ids swapped in one statement", t.run("pk.db", ["UPDATE t SET id = 3 - id WHERE id < 3", "SELECT id, c FROM t"]),
         ["Executed.", "(1, r2)", "(2, r1)", "(3, r3)", "(104, r4)", "(105, r5)", "Executed."])
t.expect("id must stay a whole number", t.run("pk.db", ["UPDATE t SET id = 'x' WHERE id = 3"]),
         ["Error: datatype mismatch: id must be an integer."])
t.valid("after primary key updates", "pk.db", {"t": [1, 2, 3, 104, 105]})

t.finish()
