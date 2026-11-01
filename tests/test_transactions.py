"""BEGIN / COMMIT / ROLLBACK, killing the program, and recovery from a journal.
Usage: python test_transactions.py <path to the database program>"""
import os
import shutil
import struct
import subprocess
import threading
import time

from helpers import Tester

t = Tester()
SCHEMA = "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)"


def insert(i):
    return f"INSERT INTO t VALUES ({i}, 'row {i}')"


def read_bytes(db):
    with open(t.path(db), "rb") as f:
        return f.read()


def journal_exists(db):
    return os.path.exists(t.path(db) + "-journal")


print("== BEGIN / COMMIT / ROLLBACK")
out = t.run("tx.db", [
    SCHEMA, "BEGIN", insert(1), insert(2), "ROLLBACK", "SELECT * FROM t",
    "BEGIN TRANSACTION", insert(3), "COMMIT", "SELECT * FROM t",
    "END", "COMMIT", "ROLLBACK", "BEGIN", "BEGIN", "ROLLBACK TRANSACTION",
])
t.expect("statements", out, [
    "Executed.", "Executed.", "Executed.", "Executed.", "Executed.", "Executed.",
    "Executed.", "Executed.", "Executed.", "(3, row 3)", "Executed.",
    "Error: cannot commit - no transaction is active.",
    "Error: cannot commit - no transaction is active.",
    "Error: cannot rollback - no transaction is active.",
    "Executed.", "Error: cannot start a transaction within a transaction.", "Executed.",
])

out = t.run("tx.db", ["BEGIN", "CREATE TABLE u (x INT)", "INSERT INTO u VALUES (1)", ".tables", "ROLLBACK", ".tables",
                      "BEGIN", "DROP TABLE t", ".tables", "ROLLBACK", ".tables", "SELECT * FROM t"])
t.expect("ROLLBACK undoes CREATE TABLE and DROP TABLE", out, [
    "Executed.", "Executed.", "Executed.", "t", "u", "Executed.", "t",
    "Executed.", "Executed.", "Executed.", "t", "(3, row 3)", "Executed."])
t.valid("after rolled back CREATE / DROP", "tx.db", {"t": [3]})

out = t.run("tx.db", ["BEGIN", insert(10), insert(10), insert(11), "COMMIT", "SELECT id FROM t"])
t.expect("an error inside BEGIN keeps the other statements", out, [
    "Executed.", "Executed.", "Error: Duplicate key.", "Executed.", "Executed.", "(3)", "(10)", "(11)", "Executed."])

out = t.run("tx.db", ["UPDATE t SET id = 3 WHERE id > 3"])
t.expect("a failing statement outside BEGIN changes nothing", out, ["Error: Duplicate key."])
t.expect("... rows are still there", t.rows("tx.db", "SELECT id FROM t"), ["(3)", "(10)", "(11)"])

out = t.run("tx.db", ["BEGIN", insert(20)])
t.expect(".exit with an open transaction", out, [
    "Executed.", "Executed.", "Transaction was not committed, its changes were rolled back."])
t.expect("... and nothing was saved", t.rows("tx.db", "SELECT id FROM t"), ["(3)", "(10)", "(11)"])

print("== a big transaction rolled back never touches the file")
t.run("big.db", [SCHEMA, "BEGIN"] + [insert(i) for i in range(1, 3001)] + ["COMMIT"])
before = read_bytes("big.db")
t.run("big.db", ["BEGIN"] + [insert(i) for i in range(3001, 8001)] +
      [f"DELETE FROM t WHERE id = {i}" for i in range(1, 2000)] + ["UPDATE t SET v = 'changed'", "ROLLBACK"])
t.expect("file is byte for byte the same", read_bytes("big.db") == before, True)
t.valid("after rollback", "big.db", {"t": list(range(1, 3001))})
t.run("big.db", ["BEGIN"] + [f"DELETE FROM t WHERE id = {i}" for i in range(1, 2000)] + ["COMMIT"])
t.valid("after committed deletes", "big.db", {"t": list(range(2000, 3001))})
t.expect("no journal left behind", journal_exists("big.db"), False)

print("== killing the program")


class Running:
    """The program running in the background, fed one line at a time"""

    def __init__(self, db):
        self.process = subprocess.Popen([t.exe, t.path(db)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True, bufsize=1)
        self.output = ""
        self.lock = threading.Lock()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        while True:
            c = self.process.stdout.read(1)
            if not c:
                return
            with self.lock:
                self.output += c

    def send(self, lines):
        """Sends lines and waits until each one printed its result"""
        with self.lock:
            done_before = self.output.count("Executed.")
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        deadline = time.time() + 30
        while time.time() < deadline:
            with self.lock:
                if self.output.count("Executed.") >= done_before + len(lines):
                    return
            time.sleep(0.01)
        raise RuntimeError("program didn't answer: " + self.output[-200:])

    def kill(self):
        self.process.kill()
        self.process.wait()


running = Running("kill.db")
running.send([SCHEMA] + [insert(i) for i in range(1, 51)])
running.kill()
t.expect("rows saved one by one survive a kill", len(t.rows("kill.db", "SELECT * FROM t")), 50)

running = Running("kill.db")
running.send(["BEGIN"] + [insert(i) for i in range(51, 201)] + ["DELETE FROM t WHERE id < 10"])
running.kill()
t.expect("a transaction without COMMIT is gone after a kill", len(t.rows("kill.db", "SELECT * FROM t")), 50)

running = Running("kill.db")
running.send(["BEGIN"] + [insert(i) for i in range(51, 201)] + ["COMMIT", "DELETE FROM t WHERE id = 1"])
running.kill()
t.expect("a committed transaction survives a kill", len(t.rows("kill.db", "SELECT * FROM t")), 199)
t.valid("after kills", "kill.db")
t.expect("no journal left behind", journal_exists("kill.db"), False)

print("== recovery from a journal (as if the program died while committing)")


def make_journal(old, new):
    """The journal a commit from `old` to `new` would write"""
    old_pages = len(old) // 4096
    journal = b"MyDBjrnl" + struct.pack("<II", old_pages, 4096)
    for page in range(old_pages):
        if old[page * 4096:(page + 1) * 4096] != new[page * 4096:(page + 1) * 4096]:
            journal += struct.pack("<I", page) + old[page * 4096:(page + 1) * 4096]
    return journal


t.run("rec.db", [SCHEMA, "CREATE TABLE u (x INTEGER)", "BEGIN"] + [insert(i) for i in range(1, 2001)] + ["COMMIT"])
old = read_bytes("rec.db")
t.run("rec.db", ["BEGIN", "UPDATE t SET v = 'new' WHERE id % 3 = 0", "DELETE FROM t WHERE id > 1500"] +
      [f"INSERT INTO u VALUES ({i})" for i in range(3000)] + ["DROP TABLE t", "COMMIT"])
new = read_bytes("rec.db")
t.expect("the commit changed pages and grew the file", (len(new) > len(old), old != new[:len(old)]), (True, True))
journal = make_journal(old, new)

# Died after writing every new page, before deleting the journal
shutil.copy(t.path("rec.db"), t.path("rec_full.db"))
open(t.path("rec_full.db") + "-journal", "wb").write(journal)
t.expect("tables after recovery", t.run("rec_full.db", [".tables"]), ["t", "u"])
t.expect("file is exactly the old one again", read_bytes("rec_full.db") == old, True)
t.expect("journal deleted", journal_exists("rec_full.db"), False)
t.valid("recovered file", "rec_full.db", {"t": list(range(1, 2001)), "u": []})

# Died halfway through writing the pages: some new, some old, file already longer
half = bytearray(new)
changed = [p for p in range(len(old) // 4096) if old[p * 4096:(p + 1) * 4096] != new[p * 4096:(p + 1) * 4096]]
for p in changed[len(changed) // 2:]:
    half[p * 4096:(p + 1) * 4096] = old[p * 4096:(p + 1) * 4096]
open(t.path("rec_half.db"), "wb").write(bytes(half))
open(t.path("rec_half.db") + "-journal", "wb").write(journal)
t.run("rec_half.db", [])
t.expect("half-written file is restored", read_bytes("rec_half.db") == old, True)

# Died while writing the journal: the database file was never touched
for name, cut in [("header", 10), ("last entry", len(journal) - 100)]:
    db = f"rec_cut_{cut}.db"
    open(t.path(db), "wb").write(old)
    open(t.path(db) + "-journal", "wb").write(journal[:cut])
    t.run(db, [])
    t.expect(f"journal cut in its {name}: file unchanged", read_bytes(db) == old, True)
    t.expect(f"journal cut in its {name}: journal deleted", journal_exists(db), False)

t.finish()
