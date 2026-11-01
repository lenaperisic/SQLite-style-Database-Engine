"""Shared helpers for the database tests.

Every test script gets the path of the database program as its only
argument (CMake passes it, see CMakeLists.txt), works in its own temporary
folder and exits with 1 if anything failed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_db  # noqa: E402


def format_value(value):
    """Formats a Python / sqlite3 value the way the database prints it."""
    if value is None:
        return "NULL"
    if isinstance(value, float):
        text = "%.15g" % value
        if not any(c in text for c in ".eni"):
            text += ".0"
        return text
    if isinstance(value, bytes):
        return "x'" + value.hex().upper() + "'"
    return str(value)


def format_row(values):
    return "(" + ", ".join(format_value(v) for v in values) + ")"


class Tester:
    def __init__(self):
        if len(sys.argv) < 2:
            sys.exit(f"usage: python {sys.argv[0]} <path to the database program>")
        self.exe = sys.argv[1]
        self.dir = tempfile.mkdtemp(prefix="mydb-test-")
        self.passed = 0
        self.failed = 0

    def path(self, db):
        return os.path.join(self.dir, db)

    def run(self, db, lines, exit_command=True):
        """Runs the program on a database file with these input lines.
        Returns the output lines without the "db > " prompts."""
        text = "\n".join(lines + ([".exit"] if exit_command else [])) + "\n"
        out = subprocess.run([self.exe, self.path(db)], input=text,
                             capture_output=True, text=True).stdout
        return [line for line in out.replace("db > ", "\n").split("\n") if line]

    def rows(self, db, sql):
        """Result rows of one SELECT"""
        return [line for line in self.run(db, [sql]) if line.startswith("(")]

    def expect(self, name, got, want):
        if got == want:
            self.passed += 1
            print("PASS ", name)
        else:
            self.failed += 1
            print("FAIL ", name)
            print("   got: ", repr(got)[:600])
            print("   want:", repr(want)[:600])

    def valid(self, name, db, expected_ids=None):
        """Checks the whole file with check_db; expected_ids = {table: [ids]}"""
        errors, summary = check_db.check(self.path(db), expected_ids)
        self.expect(name + " (file valid)", errors, [])
        return summary

    def pages(self, db):
        return os.path.getsize(self.path(db)) // 4096

    def finish(self):
        print(f"---- {self.passed} passed, {self.failed} failed")
        if self.failed:
            print("test files kept in", self.dir)
            sys.exit(1)
        shutil.rmtree(self.dir, ignore_errors=True)
