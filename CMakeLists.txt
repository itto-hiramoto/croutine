cmake_minimum_required(VERSION 3.10)
project(croutine VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CROUTINE_SOURCES
    src/channel.c
    src/io.c
    src/poller_stub.c
    src/runtime.c
    src/task.c
    src/worker.c
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(REMOVE_ITEM CROUTINE_SOURCES src/poller_stub.c)
    list(APPEND CROUTINE_SOURCES src/poller_epoll.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(REMOVE_ITEM CROUTINE_SOURCES src/poller_stub.c)
    list(APPEND CROUTINE_SOURCES src/poller_kqueue.c)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND CROUTINE_SOURCES src/arch/aarch64.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    list(APPEND CROUTINE_SOURCES src/arch/x86_64.c)
else()
    message(FATAL_ERROR "croutine does not support ${CMAKE_SYSTEM_PROCESSOR}")
endif()

find_package(Threads REQUIRED)
add_library(croutine STATIC ${CROUTINE_SOURCES})
add_library(croutine::croutine ALIAS croutine)
target_include_directories(croutine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(croutine PUBLIC Threads::Threads)
set_target_properties(croutine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(croutine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(CROUTINE_SANITIZER "" CACHE STRING
    "Sanitizer for tests and examples: address, undefined, or thread")
if(CROUTINE_SANITIZER)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CROUTINE_SANITIZER requires GCC or Clang")
    endif()
    if(NOT CROUTINE_SANITIZER MATCHES "^(address|undefined|thread)$")
        message(FATAL_ERROR "Unsupported sanitizer: ${CROUTINE_SANITIZER}")
    endif()
    target_compile_options(croutine PRIVATE
        -fsanitize=${CROUTINE_SANITIZER} -fno-omit-frame-pointer)
    target_link_libraries(croutine PUBLIC -fsanitize=${CROUTINE_SANITIZER})
    if(CROUTINE_SANITIZER STREQUAL "address")
        target_compile_definitions(croutine PRIVATE CROUTINE_USE_ASAN=1)
    elseif(CROUTINE_SANITIZER STREQUAL "thread")
        target_compile_definitions(croutine PRIVATE CROUTINE_USE_TSAN=1)
    endif()
endif()

option(CROUTINE_BUILD_EXAMPLES "Build croutine examples" ON)
if(CROUTINE_BUILD_EXAMPLES)
    add_executable(croutine_basic examples/basic.c)
    target_link_libraries(croutine_basic PRIVATE croutine::croutine)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(croutine_tests tests/runtime_test.c)
    target_link_libraries(croutine_tests PRIVATE croutine::croutine)
    add_test(NAME croutine_runtime COMMAND croutine_tests)

    add_executable(croutine_lifecycle_tests tests/lifecycle_test.c)
    target_link_libraries(croutine_lifecycle_tests PRIVATE croutine::croutine)
    add_test(NAME croutine_lifecycle COMMAND croutine_lifecycle_tests)

    add_executable(croutine_migration_tests tests/migration_test.c)
    target_link_libraries(croutine_migration_tests PRIVATE croutine::croutine)
    add_test(NAME croutine_migration COMMAND croutine_migration_tests)

    add_executable(croutine_queue_tests tests/task_queue_test.c src/task.c)
    target_include_directories(croutine_queue_tests PRIVATE include src)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(croutine_queue_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(CROUTINE_SANITIZER)
        target_compile_options(croutine_queue_tests PRIVATE
            -fsanitize=${CROUTINE_SANITIZER} -fno-omit-frame-pointer)
        target_link_libraries(croutine_queue_tests PRIVATE
            -fsanitize=${CROUTINE_SANITIZER})
    endif()
    add_test(NAME croutine_task_queue COMMAND croutine_queue_tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS croutine EXPORT croutineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/croutine/runtime.h include/croutine/channel.h
              include/croutine/io.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/croutine)
install(EXPORT croutineTargets NAMESPACE croutine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/croutine)
configure_package_config_file(cmake/croutineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/croutineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/croutine)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/croutineConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/croutineConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/croutineConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/croutine)
