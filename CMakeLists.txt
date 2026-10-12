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

include(FetchContent)

# ---------------------------------------------------------------------------
# 第三方：MinHook（注入与函数挂钩）
#
# 上游不含 CMakeLists.txt（只有 VS 工程），故只取源码、自行建库；SOURCE_SUBDIR 指向不存在的
# 子目录，以阻止 FetchContent 调用 add_subdirectory()。
# 指向本地已有源码：-DFETCHCONTENT_SOURCE_DIR_MINHOOK=<dir>
# ---------------------------------------------------------------------------
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    SOURCE_SUBDIR  no-cmake
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
    "${minhook_SOURCE_DIR}/src/hde/hde32.c"
)
target_include_directories(minhook PUBLIC "${minhook_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------
# 第三方：Dear ImGui（界面），Win32 + D3D11 后端
# ---------------------------------------------------------------------------
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
# waf_core：配置模型、YAML、运行环境、IFEO（被 injector.dll、主程序与启动代理共用）
# ---------------------------------------------------------------------------
add_library(waf_core STATIC
    src/core/yamlmini.cpp
    src/core/runtime.cpp
    src/core/config.cpp
    src/core/config_io.cpp
    src/core/ifeo.cpp
)
target_include_directories(waf_core PUBLIC include)
target_link_libraries(waf_core PUBLIC user32 gdi32)

# ---------------------------------------------------------------------------
# injector.dll：被注入目标进程的 hook 模块
#   组件在 src/hook/components/ 下，一个文件一个 hook
# ---------------------------------------------------------------------------
add_library(injector SHARED
    src/hook/dllmain.cpp
    src/hook/components/enum_display_settings.cpp
    src/hook/components/get_device_caps.cpp
    src/hook/components/get_monitor_info.cpp
)
target_link_libraries(injector PRIVATE waf_core minhook)
set_target_properties(injector PROPERTIES
    PREFIX ""
    OUTPUT_NAME "injector"
)

# ---------------------------------------------------------------------------
# WorkAreaFullscreen.exe：配置编辑器 + 启动代理管理者（不监听进程、不注入）
# ---------------------------------------------------------------------------
add_executable(WorkAreaFullscreen
    src/app/main.cpp
    src/app/app.cpp
    src/app/ui.cpp
)
target_link_libraries(WorkAreaFullscreen PRIVATE waf_core imgui)

# ---------------------------------------------------------------------------
# HookLauncher.exe：启动代理
#   配合 IFEO，在目标执行用户代码之前就把 injector.dll 注入进去，彻底摆脱注入时机问题
# ---------------------------------------------------------------------------
add_executable(HookLauncher src/launcher/main.cpp)
target_link_libraries(HookLauncher PRIVATE waf_core)

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
