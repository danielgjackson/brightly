cmake_minimum_required(VERSION 3.4)
project(brightly)
add_executable(brightly WIN32 brightly.c monitor.c monitor.h)
add_definitions(-DUNICODE -D_UNICODE)
target_sources(brightly PRIVATE brightly.rc)
#target_sources(brightly PRIVATE brightly.exe.manifest)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
# Release binary output at top level
set_target_properties(brightly PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "..")
# cmake -S . -B build  &&  cmake --build build --config Release
