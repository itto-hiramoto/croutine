cmake_minimum_required(VERSION 3.10)
project(croutine C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/task.c
    src/arch/x86_64.c
    src/m1.c
)

# Include directories
include_directories(
    include/
)

# Create executable
add_executable(croutine ${SOURCES})

# Set compiler flags for x86_64 assembly
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(croutine PRIVATE -Wall -Wextra)
endif()
