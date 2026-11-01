#include "Statement.h"
#include "Cursor.h"
#include "Database.h"
#include "ExceptionHandler.h"
#include "Node.h"
#include "Parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>

static std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Row ids are 32-bit keys in the tree
static std::uint32_t to_row_id(const Value& value, const std::string& column) {
    if (value.get_type() != DataType::Integer) {
        throw DatatypeMismatchException(column);
    }
    std::int64_t id = value.as_integer();
    if (id < 0) {
        throw NegativeIdException();
    }
    if (id > std::numeric_limits<std::uint32_t>::max()) {
        throw IdTooLargeException();
    }
    return static_cast<std::uint32_t>(id);
}

static void print_values(const std::vector<Value>& values) {
    std::cout << "(";
    for (std::size_t i = 0; i < values.size(); i++) {
        std::cout << (i > 0 ? ", " : "") << values[i].to_string();
    }
    std::cout << ")\n";
}

Statement prepare_statement(const std::string& line) {
    return Parser(line).parse();
}

// ---- finding rows ----

// One row of a table as statements see it: the id, and a value for every
// column, with the id put back into the INTEGER PRIMARY KEY column
struct TableRow {
    std::uint32_t rowid;
    std::vector<Value> values;
};

static TableRow read_row(Cursor& cursor, const TableSchema& schema) {
    TableRow row{cursor.get_key(), Row::deserialize(cursor.get_key(), cursor.value()).get_values()};
    row.values.resize(schema.columns.size());
    int key_column = schema.primary_key_index();
    if (key_column >= 0) {
        row.values[key_column] = Value::integer(row.rowid);
    }
    return row;
}

// If the WHERE says "id = 5" (or "rowid = 5", maybe AND something else),
// the row can be found directly in the tree instead of reading every row
static std::optional<std::uint32_t> exact_key(const Expr* where, const TableSchema& schema) {
    if (!where || where->kind != ExprKind::Binary) {
        return std::nullopt;
    }
    if (where->op == "AND") {
        std::optional<std::uint32_t> key = exact_key(where->args[0].get(), schema);
        return key ? key : exact_key(where->args[1].get(), schema);
    }
    if (where->op != "=") {
        return std::nullopt;
    }

    for (int side = 0; side < 2; side++) {
        const Expr& column = *where->args[side];
        const Expr& literal = *where->args[1 - side];
        bool is_key = column.kind == ExprKind::Column
                      && (column.column_index == -1 || column.column_index == schema.primary_key_index());
        if (is_key && literal.kind == ExprKind::Literal) {
            Value id = literal.value.with_affinity(DataType::Integer);
            if (id.get_type() == DataType::Integer && id.as_integer() >= 0
                && id.as_integer() <= std::numeric_limits<std::uint32_t>::max()) {
                return static_cast<std::uint32_t>(id.as_integer());
            }
        }
    }
    return std::nullopt;
}

// Every row where `where` is true (every row if there's no WHERE), by id
static std::vector<TableRow> find_rows(Table& table, const TableSchema& schema, const Expr* where) {
    std::vector<TableRow> rows;
    auto matches = [&](const TableRow& row) {
        return !where || is_true(evaluate(*where, row.rowid, row.values));
    };

    std::optional<std::uint32_t> key = exact_key(where, schema);
    if (key) {
        Cursor cursor = Cursor::table_find(table, *key);
        if (cursor.is_at(*key)) {
            TableRow row = read_row(cursor, schema);
            if (matches(row)) {
                rows.push_back(row);
            }
        }
        return rows;
    }

    for (Cursor cursor = Cursor::table_start(table); !cursor.is_end(); cursor.advance()) {
        TableRow row = read_row(cursor, schema);
        if (matches(row)) {
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

// ---- statements ----

static void execute_insert(const Statement& statement, Database& database) {
    const TableSchema& schema = database.get_schema(statement.table_name);
    if (statement.values.size() != schema.columns.size()) {
        throw ColumnCountException(schema.name, schema.columns.size(), statement.values.size());
    }

    std::vector<Value> values;
    for (std::size_t i = 0; i < schema.columns.size(); i++) {
        values.push_back(statement.values[i].with_affinity(schema.columns[i].type));
    }

    Table table = database.get_table(schema.name);
    int key_column = schema.primary_key_index();

    std::uint32_t id;
    if (key_column >= 0 && values[key_column].get_type() != DataType::Null) {
        id = to_row_id(values[key_column], schema.columns[key_column].name);
    } else {
        // No id given: one more than the biggest, like SQLite's rowid
        if (table.is_empty()) {
            id = 1;
        } else if (table.get_max_key() == std::numeric_limits<std::uint32_t>::max()) {
            throw IdTooLargeException();
        } else {
            id = table.get_max_key() + 1;
        }
    }

    // The id is the cell's key, the record keeps NULL in its place
    if (key_column >= 0) {
        values[key_column] = Value();
    }

    table.insert(Row(id, values));
}

static void execute_select(const Statement& statement, Database& database) {
    const TableSchema& schema = database.get_schema(statement.table_name);
    Table table = database.get_table(schema.name);

    bool aggregate_query = false;
    for (const SelectItem& item : statement.select_items) {
        if (item.expr) {
            bind(*item.expr, schema, true);
            aggregate_query = aggregate_query || contains_aggregate(*item.expr);
        }
    }
    if (statement.where) {
        bind(*statement.where, schema, false);
    }

    std::vector<TableRow> rows = find_rows(table, schema, statement.where.get());
    std::vector<std::vector<Value>> results;

    if (aggregate_query) {
        // Without GROUP BY, an aggregate query gives exactly one row
        std::vector<const Expr*> nodes;
        for (const SelectItem& item : statement.select_items) {
            if (item.star || uses_column_outside_aggregate(*item.expr)) {
                throw SyntaxErrorException("can't mix aggregates and plain columns without GROUP BY");
            }
            collect_aggregates(*item.expr, nodes);
        }

        std::vector<Aggregator> aggregators;
        for (const Expr* node : nodes) {
            aggregators.emplace_back(*node);
        }
        for (const TableRow& row : rows) {
            for (Aggregator& aggregator : aggregators) {
                aggregator.add(row.rowid, row.values);
            }
        }

        std::map<const Expr*, Value> totals;
        for (std::size_t i = 0; i < nodes.size(); i++) {
            totals[nodes[i]] = aggregators[i].result();
        }
        std::vector<Value> result;
        for (const SelectItem& item : statement.select_items) {
            result.push_back(evaluate(*item.expr, 0, {}, &totals));
        }
        results.push_back(result);
    } else {
        std::size_t output_width = 0;
        for (const SelectItem& item : statement.select_items) {
            output_width += item.star ? schema.columns.size() : 1;
        }

        // ORDER BY 2 means the second output column
        std::vector<int> order_positions;
        for (const OrderTerm& term : statement.order_by) {
            int position = -1;
            if (term.expr->kind == ExprKind::Literal && term.expr->value.get_type() == DataType::Integer) {
                std::int64_t n = term.expr->value.as_integer();
                if (n < 1 || static_cast<std::uint64_t>(n) > output_width) {
                    throw SyntaxErrorException("ORDER BY term out of range");
                }
                position = static_cast<int>(n) - 1;
            } else {
                bind(*term.expr, schema, false);
            }
            order_positions.push_back(position);
        }

        struct Output {
            std::vector<Value> values;
            std::vector<Value> sort_keys;
        };
        std::vector<Output> outputs;
        for (const TableRow& row : rows) {
            Output output;
            for (const SelectItem& item : statement.select_items) {
                if (item.star) {
                    output.values.insert(output.values.end(), row.values.begin(), row.values.end());
                } else {
                    output.values.push_back(evaluate(*item.expr, row.rowid, row.values));
                }
            }
            for (std::size_t i = 0; i < statement.order_by.size(); i++) {
                int position = order_positions[i];
                if (position >= 0) {
                    output.sort_keys.push_back(output.values[position]);
                } else {
                    output.sort_keys.push_back(evaluate(*statement.order_by[i].expr, row.rowid, row.values));
                }
            }
            outputs.push_back(std::move(output));
        }

        // stable_sort: rows that sort equal stay in id order
        std::stable_sort(outputs.begin(), outputs.end(), [&](const Output& a, const Output& b) {
            for (std::size_t i = 0; i < a.sort_keys.size(); i++) {
                int c = compare_values(a.sort_keys[i], b.sort_keys[i]);
                if (c != 0) {
                    return statement.order_by[i].descending ? c > 0 : c < 0;
                }
            }
            return false;
        });

        for (Output& output : outputs) {
            results.push_back(std::move(output.values));
        }
    }

    // OFFSET skips rows, LIMIT caps how many are printed
    std::size_t start = static_cast<std::size_t>(std::min<std::int64_t>(statement.offset, results.size()));
    std::size_t end = results.size();
    if (statement.limit) {
        end = std::min<std::size_t>(end, start + static_cast<std::size_t>(*statement.limit));
    }
    for (std::size_t i = start; i < end; i++) {
        print_values(results[i]);
    }
}

static void execute_update(const Statement& statement, Database& database) {
    const TableSchema& schema = database.get_schema(statement.table_name);
    Table table = database.get_table(schema.name);
    int key_column = schema.primary_key_index();

    // Which column each assignment changes
    std::vector<std::size_t> targets;
    for (const Assignment& assignment : statement.assignments) {
        std::string name = to_lower(assignment.column);
        auto it = std::find_if(schema.columns.begin(), schema.columns.end(),
                               [&](const Column& column) { return to_lower(column.name) == name; });
        if (it == schema.columns.end()) {
            throw NoSuchColumnException(assignment.column);
        }
        targets.push_back(static_cast<std::size_t>(it - schema.columns.begin()));
        bind(*assignment.expr, schema, false);
    }
    if (statement.where) {
        bind(*statement.where, schema, false);
    }

    // Work out every new row first, so a bad value changes nothing
    std::vector<TableRow> rows = find_rows(table, schema, statement.where.get());
    std::vector<Row> new_rows;
    std::set<std::uint32_t> old_ids;
    std::set<std::uint32_t> new_ids;
    bool ids_change = false;

    for (const TableRow& row : rows) {
        std::vector<Value> values = row.values;
        // Every SET uses the old values: SET a = b, b = a swaps them
        for (std::size_t i = 0; i < targets.size(); i++) {
            values[targets[i]] = evaluate(*statement.assignments[i].expr, row.rowid, row.values)
                                     .with_affinity(schema.columns[targets[i]].type);
        }

        std::uint32_t id = row.rowid;
        if (key_column >= 0) {
            id = to_row_id(values[key_column], schema.columns[key_column].name);
            values[key_column] = Value();
        }
        if (!new_ids.insert(id).second) {
            throw DuplicateKeyException();
        }
        old_ids.insert(row.rowid);
        ids_change = ids_change || id != row.rowid;
        new_rows.emplace_back(id, values);     // checks the size
    }

    if (!ids_change) {
        for (const Row& row : new_rows) {
            table.update(row);
        }
        return;
    }

    // A new id may not belong to a row this UPDATE leaves alone
    for (std::uint32_t id : new_ids) {
        if (old_ids.count(id) == 0) {
            if (Cursor::table_find(table, id).is_at(id)) {
                throw DuplicateKeyException();
            }
        }
    }
    for (std::uint32_t id : old_ids) {
        table.remove(id);
    }
    for (const Row& row : new_rows) {
        table.insert(row);
    }
}

static void execute_delete(const Statement& statement, Database& database) {
    const TableSchema& schema = database.get_schema(statement.table_name);
    Table table = database.get_table(schema.name);

    if (!statement.where) {
        table.clear();
        return;
    }

    bind(*statement.where, schema, false);
    for (const TableRow& row : find_rows(table, schema, statement.where.get())) {
        table.remove(row.rowid);
    }
}

static void run(const Statement& statement, Database& database) {
    switch (statement.type) {
        case StatementType::CreateTable:
            database.create_table(statement.schema);
            break;
        case StatementType::DropTable:
            database.drop_table(statement.table_name);
            break;
        case StatementType::Insert:
            execute_insert(statement, database);
            break;
        case StatementType::Select:
            execute_select(statement, database);
            break;
        case StatementType::Update:
            execute_update(statement, database);
            break;
        case StatementType::Delete:
            execute_delete(statement, database);
            break;
        default:
            break;
    }
}

void execute_statement(const Statement& statement, Database& database) {
    switch (statement.type) {
        case StatementType::Begin:
            database.begin();
            return;
        case StatementType::Commit:
            database.commit();
            return;
        case StatementType::Rollback:
            database.rollback();
            return;
        case StatementType::Select:
            run(statement, database);   // reads only, needs no transaction
            return;
        default:
            break;
    }

    // Outside BEGIN ... COMMIT, the statement is its own transaction
    bool autocommit = !database.in_transaction();
    if (autocommit) {
        database.begin();
    }
    try {
        run(statement, database);
    }
    catch (const DatabaseFileException&) {
        // Something went wrong halfway through: undo everything
        database.rollback();
        throw;
    }
    catch (const ExceptionHandler&) {
        // Other errors are found before anything changes; inside BEGIN the
        // earlier statements stay
        if (autocommit) {
            database.rollback();
        }
        throw;
    }
    if (autocommit) {
        database.commit();
    }
}
