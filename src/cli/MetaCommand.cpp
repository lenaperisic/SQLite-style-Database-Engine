#include "MetaCommand.h"
#include "Database.h"
#include "ExceptionHandler.h"

#include <iostream>

MetaCommandResult do_meta_command(const std::string& line, Database& database) {
    if (line == ".exit") {
        return MetaCommandResult::Exit;
    }
    if (line == ".tables") {
        for (const TableSchema* schema : database.get_schemas()) {
            std::cout << schema->name << '\n';
        }
        return MetaCommandResult::Continue;
    }
    if (line == ".schema") {
        for (const TableSchema* schema : database.get_schemas()) {
            std::cout << schema->sql << ";\n";
        }
        return MetaCommandResult::Continue;
    }

    throw UnrecognizedCommandException(line);
}
