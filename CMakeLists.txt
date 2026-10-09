cmake_minimum_required(VERSION 3.20)
project(upPlayer LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/include/mpv/client.h")
  message(FATAL_ERROR
    "缺少 mpv submodule（third_party/mpv）。\n"
    "在仓库根目录执行: git submodule update --init --depth 1 third_party/mpv")
endif()

add_executable(upPlayer WIN32
  src/main.cpp
  src/player.cpp
  src/upplayer.rc
)

target_include_directories(upPlayer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/include
)

target_compile_definitions(upPlayer PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00
)

if(MSVC)
  target_compile_options(upPlayer PRIVATE /utf-8 /W4 /permissive-)
  target_link_options(upPlayer PRIVATE "/ENTRY:wWinMainCRTStartup")
endif()

target_link_libraries(upPlayer PRIVATE comctl32 comdlg32 shell32 dwmapi)

set(LIBMPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libmpv-win")
if(EXISTS "${LIBMPV_DIR}/libmpv-2.dll")
  file(GLOB LIBMPV_DLLS "${LIBMPV_DIR}/*.dll")
  foreach(dll ${LIBMPV_DLLS})
    add_custom_command(TARGET upPlayer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${dll}" "$<TARGET_FILE_DIR:upPlayer>"
      VERBATIM)
  endforeach()
endif()
