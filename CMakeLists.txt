cmake_minimum_required(VERSION 3.20)
project(WorkAreaFullscreen LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # 一律静态 CRT，DLL 不依赖 CRT 运行库

if(NOT MSVC)
    message(FATAL_ERROR "WorkAreaFullscreen 需要 MSVC 工具链（Windows x64 ABI 与 SEH）")
endif()

add_compile_options(/utf-8 /W3 /permissive-)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------------------------
# 第三方：MinHook（注入与函数挂钩）
#
# 查找顺序：-DHOOKKIT_MINHOOK_DIR=<dir> > third_party/minhook > _mh/minhook-1.3.3
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
    message(FATAL_ERROR "未找到 MinHook。用 -DHOOKKIT_MINHOOK_DIR=<dir> 指定，或放到 third_party/minhook。")
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

# ---------------------------------------------------------------------------
# 第三方：Dear ImGui（界面），Win32 + D3D11 后端
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler user32)
target_compile_options(imgui PRIVATE /W1)

# ---------------------------------------------------------------------------
# waf_core：配置模型、YAML、运行环境（被 injector.dll 与主程序共用）
# ---------------------------------------------------------------------------
add_library(waf_core STATIC
    src/core/yamlmini.cpp
    src/core/runtime.cpp
    src/core/config.cpp
)
target_include_directories(waf_core PUBLIC include)
target_link_libraries(waf_core PUBLIC user32 gdi32)

# ---------------------------------------------------------------------------
# injector.dll：被注入目标进程的 hook 模块
#   组件在 src/hook/components/ 下，一个文件一个 hook
# ---------------------------------------------------------------------------
add_library(injector SHARED
    src/hook/dllmain.cpp
    src/hook/components/get_device_caps.cpp
    src/hook/components/get_monitor_info.cpp
)
target_link_libraries(injector PRIVATE waf_core minhook)
set_target_properties(injector PROPERTIES
    PREFIX ""
    OUTPUT_NAME "injector"
)

# ---------------------------------------------------------------------------
# WorkAreaFullscreen.exe：常驻宿主（ImGui 界面 + 托盘 + 进程监听 + 注入）
# ---------------------------------------------------------------------------
add_executable(WorkAreaFullscreen
    src/app/main.cpp
    src/app/app.cpp
    src/app/ui.cpp
    src/app/watch.cpp
)
target_link_libraries(WorkAreaFullscreen PRIVATE waf_core imgui)

# ---------------------------------------------------------------------------
# 自检（-DBUILD_TESTING=OFF 可关闭）
# ---------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    add_executable(yamlmini_test tests/yamlmini_test.cpp)
    target_link_libraries(yamlmini_test PRIVATE waf_core)
    add_test(NAME yamlmini COMMAND yamlmini_test)

    # 集成自检的目标进程；断言脚本见 tests/run_integration.ps1
    add_executable(probe_host tests/probe_host.cpp)
    target_link_libraries(probe_host PRIVATE user32 gdi32)
endif()
