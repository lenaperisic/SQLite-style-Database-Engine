#ifndef METACOMMAND_H
#define METACOMMAND_H

#include <string>

class Database;

enum class MetaCommandResult {
    Continue,
    Exit
};

// Handles commands starting with '.':
//   .exit     closes the database
//   .tables   lists the tables
//   .schema   prints the CREATE TABLE of every table
// Throws UnrecognizedCommandException for unknown ones.
MetaCommandResult do_meta_command(const std::string& line, Database& database);

#endif //METACOMMAND_H
