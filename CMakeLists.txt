cmake_minimum_required(VERSION 3.15)
project(LLVMProject LANGUAGES C CXX)  # Enable both C and C++

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Set the include directories for LLVM
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Print LLVM configuration details
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "LLVM Include directories: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM Definitions: ${LLVM_DEFINITIONS}")

# Avoid conflict with existing targets
set(PROJECT_EXECUTABLE isere)

# Add the source file for your project
add_executable(${PROJECT_EXECUTABLE} main.cpp)

# Link LLVM libraries
llvm_map_components_to_libnames(LLVM_LIBS core orcjit native)
target_link_libraries(${PROJECT_EXECUTABLE} PRIVATE ${LLVM_LIBS})

