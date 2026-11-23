# CMake toolchain: build with clang-cl + lld-link and a Microsoft CRT/SDK that xwin fetched on this PC
# (deps/xwin), without Visual Studio. setup/toolchain.py passes GL_LLVM_BIN and puts the SDK's include and
# library folders in the INCLUDE and LIB environment variables, which clang-cl and lld-link read the way
# cl.exe and link.exe do.
# Only a non-Windows host cross-compiles. On Windows, naming the system would make CMake treat the build as
# a cross build, and RT64's custom commands would then no longer find its own file_to_c tool.
if(NOT CMAKE_HOST_WIN32)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_SYSTEM_VERSION 10.0)
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
endif()

# try_compile projects re-read this file without the -D values of the main configure
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GL_LLVM_BIN GL_MSVC_COMPAT)
if(NOT GL_LLVM_BIN)
    message(FATAL_ERROR "GL_LLVM_BIN (the LLVM bin folder with clang-cl and lld-link) is not set; build with SETUP.bat")
endif()
if(CMAKE_HOST_WIN32)
    set(_gl_exe ".exe")
else()
    set(_gl_exe "")
endif()

set(CMAKE_C_COMPILER "${GL_LLVM_BIN}/clang-cl${_gl_exe}")
set(CMAKE_CXX_COMPILER "${GL_LLVM_BIN}/clang-cl${_gl_exe}")
set(CMAKE_LINKER "${GL_LLVM_BIN}/lld-link${_gl_exe}")
set(CMAKE_AR "${GL_LLVM_BIN}/llvm-lib${_gl_exe}")
set(CMAKE_RC_COMPILER "${GL_LLVM_BIN}/llvm-rc${_gl_exe}")
set(CMAKE_MT "${GL_LLVM_BIN}/llvm-mt${_gl_exe}")

# Without Visual Studio clang-cl cannot tell which MSVC to imitate: the CRT's (toolchain.py reads it from
# the folder name, 14.44 -> 19.44 = Visual Studio 2022 17.14).
if(NOT GL_MSVC_COMPAT)
    set(GL_MSVC_COMPAT 19.40)
endif()
string(APPEND CMAKE_C_FLAGS_INIT " --target=x86_64-pc-windows-msvc -fms-compatibility-version=${GL_MSVC_COMPAT}")
string(APPEND CMAKE_CXX_FLAGS_INIT " --target=x86_64-pc-windows-msvc -fms-compatibility-version=${GL_MSVC_COMPAT}")
# No manifest step: it needs llvm-mt built with libxml2, which some LLVM builds lack, and
# the default manifest (asInvoker) changes nothing for this exe.
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " /manifest:no")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
