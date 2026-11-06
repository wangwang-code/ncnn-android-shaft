cmake_minimum_required(VERSION 3.10)
project(rembg-ncnn)

set(CMAKE_CXX_STANDARD 11)

find_package(ncnn REQUIRED)

add_executable(rembg-ncnn main.cpp)

target_include_directories(rembg-ncnn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rembg-ncnn PRIVATE ncnn)

if(ANDROID)
    target_link_libraries(rembg-ncnn PRIVATE log)
endif()
