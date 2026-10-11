cmake_minimum_required(VERSION 3.20)
project(hookkit LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "hookkit 需要 MSVC 工具链（依赖 Windows x64 ABI 与 SEH）")
endif()

# 无隐藏的全局定制；每个 target 的设置就近给出。
add_compile_options(/utf-8 /W3 /permissive-)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------------------------
# MinHook（第三方，静态库）
#
# 查找顺序：-DHOOKKIT_MINHOOK_DIR=<dir> > third_party/minhook > _mh/minhook-1.3.3
# 只读取源码，不写入这些目录。
# ---------------------------------------------------------------------------
set(HOOKKIT_MINHOOK_DIR "" CACHE PATH "MinHook 源码目录（含 include/MinHook.h 与 src/）")

if(NOT HOOKKIT_MINHOOK_DIR)
    foreach(_cand
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook"
            "${CMAKE_CURRENT_SOURCE_DIR}/_mh/minhook-1.3.3")
        if(EXISTS "${_cand}/include/MinHook.h")
            set(HOOKKIT_MINHOOK_DIR "${_cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT HOOKKIT_MINHOOK_DIR OR NOT EXISTS "${HOOKKIT_MINHOOK_DIR}/include/MinHook.h")
    message(FATAL_ERROR
        "未找到 MinHook。请用 -DHOOKKIT_MINHOOK_DIR=<dir> 指定，或把源码放到 third_party/minhook。")
endif()
message(STATUS "MinHook: ${HOOKKIT_MINHOOK_DIR}")

add_library(minhook STATIC
    "${HOOKKIT_MINHOOK_DIR}/src/buffer.c"
    "${HOOKKIT_MINHOOK_DIR}/src/hook.c"
    "${HOOKKIT_MINHOOK_DIR}/src/trampoline.c"
    "${HOOKKIT_MINHOOK_DIR}/src/hde/hde64.c"
    "${HOOKKIT_MINHOOK_DIR}/src/hde/hde32.c"
)
target_include_directories(minhook PUBLIC "${HOOKKIT_MINHOOK_DIR}/include")
set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ---------------------------------------------------------------------------
# hookkit.dll —— 宿主 + 组件（组件是独立的 .cpp，见 src/components/）
# ---------------------------------------------------------------------------
add_library(hookkit SHARED
    src/hookkit.cpp
    src/runtime.cpp
    src/yamlmini.cpp
    src/components/get_device_caps.cpp
    src/components/get_monitor_info.cpp
)
target_include_directories(hookkit PRIVATE include)
target_link_libraries(hookkit PRIVATE minhook user32 gdi32)
set_property(TARGET hookkit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_target_properties(hookkit PROPERTIES
    PREFIX ""                 # 产物为 hookkit.dll，不加 lib 前缀
    OUTPUT_NAME "hookkit"
)

# ---------------------------------------------------------------------------
# injector.exe —— 按配置启动目标并注入 hookkit.dll
# ---------------------------------------------------------------------------
add_executable(injector src/injector.cpp src/yamlmini.cpp)
target_include_directories(injector PRIVATE include)
set_property(TARGET injector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ---------------------------------------------------------------------------
# 自检（-DBUILD_TESTING=OFF 可关闭）
# ---------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    add_executable(yamlmini_test tests/yamlmini_test.cpp src/yamlmini.cpp)
    target_include_directories(yamlmini_test PRIVATE include)
    set_property(TARGET yamlmini_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_test(NAME yamlmini COMMAND yamlmini_test)

    # 集成自检用的目标进程；断言脚本见 tests/run_integration.ps1
    add_executable(probe_host tests/probe_host.cpp)
    target_link_libraries(probe_host PRIVATE user32 gdi32)
    set_property(TARGET probe_host PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
