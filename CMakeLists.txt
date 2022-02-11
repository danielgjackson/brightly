cmake_minimum_required(VERSION 3.4)

set(CMAKE_SYSTEM_NAME Windows)
IF(WIN32)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
ELSE()
	set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
	set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
	set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
	set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)	# https://gitlab.kitware.com/cmake/cmake/-/issues/20500
	set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(MINGW TRUE)
ENDIF()

project(brightly)

add_executable(brightly WIN32 brightly.c monitor.c monitor.h)
add_definitions(-DUNICODE -D_UNICODE)
target_link_libraries(brightly user32 gdi32 comctl32 shell32 advapi32 comdlg32 ole32 oleaut32 wbemuuid dxva2 version)
IF(MINGW)
	target_link_libraries(brightly "-municode")
ENDIF()
target_sources(brightly PRIVATE brightly.rc)
#target_sources(brightly PRIVATE brightly.exe.manifest)
# Release binary output at top level
set_target_properties(brightly PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "..")

# cmake -S . -B build  &&  cmake --build build --config Release
