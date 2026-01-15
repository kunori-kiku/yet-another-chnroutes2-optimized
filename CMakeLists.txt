cmake_minimum_required(VERSION 3.16)
project(cnroutes_aggregator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(cnroutes_aggregator src/main.cpp)
target_link_libraries(cnroutes_aggregator PRIVATE CURL::libcurl)
