cmake_minimum_required(VERSION 3.31)
project(untitled CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(untitled main.cpp MetaCommand.cpp Statement.cpp Row.cpp Pager.cpp Table.cpp Cursor.cpp Node.cpp Value.cpp Varint.cpp Database.cpp Tokenizer.cpp Parser.cpp Expression.cpp)

# Tests: build, then run `ctest --output-on-failure` in the build folder
# (or the CTest run configuration in CLion). They need Python 3.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    foreach (test sql query transactions)
        add_test(NAME ${test}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_${test}.py $<TARGET_FILE:untitled>)
    endforeach ()
else ()
    message(WARNING "Python 3 not found, the tests in tests/ can't run")
endif ()
