cmake_minimum_required(VERSION 3.10)
project(RockPaperScissorsArena)

set(CMAKE_CXX_STANDARD 17)

find_package(Boost REQUIRED COMPONENTS system)

add_executable(server server/main.cpp)
target_link_libraries(server Boost::system pthread)

