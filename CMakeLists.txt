cmake_minimum_required(VERSION 3.10)
project(EngineProject)
file(GLOB_RECURSE ALL_SRCS "src/*.c")
add_executable(RPS
    ${ALL_SRCS}
)

target_include_directories(RPS 
    PRIVATE  ${CMAKE_CURRENT_SOURCE_DIR}/src/
)