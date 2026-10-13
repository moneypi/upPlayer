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
  src/caption_worker.cpp
  src/whisper_runtime.cpp
  src/yolo_runtime.cpp
  src/yolo_worker.cpp
  src/lightfc_runtime.cpp
  src/lightfc_worker.cpp
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
elseif(MINGW)
  target_link_options(upPlayer PRIVATE -municode)
endif()

target_link_libraries(upPlayer PRIVATE comctl32 comdlg32 shell32 dwmapi imm32 windowscodecs ole32 winhttp)

set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
if(EXISTS "${ONNXRUNTIME_DIR}/include/onnxruntime_cxx_api.h" AND
   EXISTS "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
  message(STATUS "ONNX Runtime found — YOLO26 / LightFC enabled")
  target_compile_definitions(upPlayer PRIVATE UPPLAYER_HAS_ONNXRUNTIME=1)
  target_include_directories(upPlayer PRIVATE "${ONNXRUNTIME_DIR}/include")
  target_link_libraries(upPlayer PRIVATE "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
  add_custom_command(TARGET upPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:upPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:upPlayer>"
    VERBATIM)
else()
  message(STATUS "ONNX Runtime not found — YOLO26/LightFC disabled (run scripts/fetch-yolo-deps.ps1)")
endif()

set(YOLO26_ONNX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yolo26/yolo26n.onnx")
if(EXISTS "${YOLO26_ONNX}")
  add_custom_command(TARGET upPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:upPlayer>/yolo26"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${YOLO26_ONNX}"
            "$<TARGET_FILE_DIR:upPlayer>/yolo26/yolo26n.onnx"
    VERBATIM)
endif()

set(LIGHTFC_Z_ONNX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lightfc/lightfc_z.onnx")
set(LIGHTFC_X_ONNX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lightfc/lightfc_x.onnx")
if(EXISTS "${LIGHTFC_Z_ONNX}" AND EXISTS "${LIGHTFC_X_ONNX}")
  add_custom_command(TARGET upPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:upPlayer>/lightfc"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIGHTFC_Z_ONNX}"
            "$<TARGET_FILE_DIR:upPlayer>/lightfc/lightfc_z.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIGHTFC_X_ONNX}"
            "$<TARGET_FILE_DIR:upPlayer>/lightfc/lightfc_x.onnx"
    VERBATIM)
endif()

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

set(POTPLAYER_WHISPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/potplayer-whisper")
if(EXISTS "${POTPLAYER_WHISPER_DIR}/Vulkan/main64.exe")
  add_custom_command(TARGET upPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${POTPLAYER_WHISPER_DIR}"
            "$<TARGET_FILE_DIR:upPlayer>/potplayer-whisper"
    VERBATIM)
endif()
