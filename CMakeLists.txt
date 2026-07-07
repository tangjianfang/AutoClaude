cmake_minimum_required(VERSION 3.20)
project(AutoClaude CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored header-only JSON library
add_subdirectory(third_party/nlohmann_json)

add_executable(AutoClaude WIN32
  src/main.cpp
  src/config.cpp
  src/ui/window.cpp
  src/ui/drawing.cpp
  src/monitor/transcript_watcher.cpp
  src/monitor/event_parser.cpp
  src/core/state_machine.cpp
  src/power/power_action.cpp
)

target_include_directories(AutoClaude PRIVATE src)

target_link_libraries(AutoClaude PRIVATE
  gdi32 user32 advapi32 powrprof shell32 shcore gdiplus ole32
  nlohmann_json::nlohmann_json
)

target_compile_definitions(AutoClaude PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

# Sources contain UTF-8 string literals; tell MSVC the source & exec charsets.
target_compile_options(AutoClaude PRIVATE /utf-8)

# Use the static CRT so the exe runs without redistributable DLL juggling.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")