# SPDX-FileCopyrightText: 2026 Ben Jarvis
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.18)
project(stopwatch LANGUAGES C)
enable_testing()
add_executable(stopwatch_test portable_test.c)
target_compile_features(stopwatch_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(stopwatch_test PRIVATE /W3 /WX /UNDEBUG)
else()
    target_compile_options(stopwatch_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
endif()
add_test(NAME stopwatch COMMAND stopwatch_test)
install(FILES stopwatch.h DESTINATION include)
