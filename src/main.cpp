#include <cstdlib>
#include <iostream>
#include <string>

#include "ExceptionHandler.h"
#include "MetaCommand.h"
#include "Database.h"
#include "Statement.h"

void print_prompt() {
    std::cout << "db > " << std::flush;
}

// Reads one line from stdin, without the trailing newline.
// Returns false on end of input.
bool read_input(std::string& line) {
    if (!std::getline(std::cin, line)) {
        return false;
    }

    // Windows line endings are "\r\n", drop the '\r'
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    return true;
}

void run_repl(Database& database) {
    std::string line;

    while (true) {
        print_prompt();

        if (!read_input(line)) {
            std::cout << '\n';
            break;
        }

        try {
            if (!line.empty() && line[0] == '.') {
                if (do_meta_command(line, database) == MetaCommandResult::Exit) {
                    break;
                }
                continue;
            }

            Statement statement = prepare_statement(line);
            execute_statement(statement, database);
            std::cout << "Executed.\n";
        }
        catch (const ExceptionHandler& e) {
            std::cout << e.what() << '\n';
        }
    }

    if (database.in_transaction()) {
        std::cout << "Transaction was not committed, its changes were rolled back.\n";
        database.rollback();
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Must supply a database filename.\n";
        return EXIT_FAILURE;
    }

    try {
        // The database's destructor saves everything to disk when run_repl returns
        Database database(argv[1]);
        run_repl(database);
    }
    catch (const ExceptionHandler& e) {
        std::cout << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
