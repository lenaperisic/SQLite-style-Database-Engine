cmake_minimum_required(VERSION 3.31)
project(minisqlite CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(minisqlite
        src/main.cpp
        src/cli/MetaCommand.cpp
        src/sql/Tokenizer.cpp src/sql/Parser.cpp src/sql/Statement.cpp src/sql/Expression.cpp
        src/engine/Database.cpp
        src/storage/Pager.cpp src/storage/Node.cpp src/storage/Table.cpp src/storage/Cursor.cpp
        src/record/Row.cpp src/record/Value.cpp src/record/Varint.cpp)
target_include_directories(minisqlite PRIVATE
        src/cli src/sql src/engine src/storage src/record)

# Tests: build, then run `ctest --output-on-failure` in the build folder
# (or the CTest run configuration in CLion). They need Python 3.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    foreach (test sql query transactions)
        add_test(NAME ${test}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_${test}.py $<TARGET_FILE:minisqlite>)
    endforeach ()
else ()
    message(WARNING "Python 3 not found, the tests in tests/ can't run")
endif ()
