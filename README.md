# MyDB

I had used databases for years without knowing what happens between typing a query and getting rows back, so I wrote one in C++17 to find out. Most of the answer turns out to be shaped by the disk being slow: everything lives in a single file of 4 KB pages, and every table is a B+ tree whose nodes are sized to match one read.

It is a learning project, so the Limitations at the bottom are real ones.

## Features

- `CREATE TABLE`, `DROP TABLE`, `INSERT`, `SELECT`, `UPDATE`, `DELETE`
- `WHERE`, `ORDER BY`, `LIMIT` / `OFFSET`, arithmetic and comparison expressions, `NULL` handling
- aggregates: `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`
- `INTEGER PRIMARY KEY` as the row id, SQLite-style column types (INTEGER, REAL, TEXT, BLOB)
- transactions (`BEGIN` / `COMMIT` / `ROLLBACK`) with a rollback journal, so the file survives a crash mid-commit
- rows stored in SQLite's record format, schema kept in a schema table on page 1
- free page list: pages from dropped tables and deleted rows are reused

## Project structure

```
src/
  main.cpp     REPL loop
  cli/         meta commands (.tables, .schema, .exit)
  sql/         tokenizer, parser, statements, expression evaluation
  engine/      database (tables, schema, transactions), errors
  storage/     pager, B+ tree nodes, tables, cursors
  record/      values, rows in SQLite record format, varints
tests/         Python tests, compared against real SQLite
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Usage

```sh
./build/minisqlite mydb.db
```

```
db > CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
Executed.
db > INSERT INTO users VALUES (1, 'ana', 25);
Executed.
db > SELECT * FROM users WHERE age > 20;
(1, ana, 25)
Executed.
db > .exit
```

Meta commands: `.tables`, `.schema`, `.exit`.

## Tests

The tests need Python 3. They compare query results against real SQLite (Python's `sqlite3`).

```sh
cd build
ctest --output-on-failure
```

## Limitations

- A row (and a table's `CREATE TABLE` text) must fit in 291 bytes, since there are no overflow pages yet
- One table per query (no joins), no indexes


