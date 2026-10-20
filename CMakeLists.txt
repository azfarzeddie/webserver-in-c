cmake_minimum_required(VERSION 3.20)
project(webserver_in_c C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(webserver_in_c src/main.c)
target_compile_options(webserver_in_c PRIVATE -Wall -Wextra)
