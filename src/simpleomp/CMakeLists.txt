cmake_minimum_required(VERSION 3.9)
project(omp CXX)

set(CMAKE_CXX_STANDARD 11)

add_library(omp SHARED
    simpleomp.cpp
)

target_include_directories(omp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(omp PRIVATE
    -fvisibility=default
    -flto=thin
    -fno-rtti
    -fno-exceptions
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
)

target_link_libraries(omp PRIVATE dl)

set_target_properties(omp PROPERTIES
    OUTPUT_NAME "omp"
    LINK_FLAGS "-Wl,-z,max-page-size=16384 -Wl,-soname,libomp.so -flto=thin -Wl,--gc-sections -Wl,--icf=all -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/omp.sym"
)
