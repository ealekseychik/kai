cmake_minimum_required(VERSION 3.10)

# Set the project name and version
project(kai VERSION 0.1)

# Specify the C standard
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED True)

# Add the executable
add_executable(kai kai.c)

# Add compiler options
target_compile_options(kai PRIVATE -Wall -Wextra -pedantic)
