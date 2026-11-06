cmake_minimum_required(VERSION 3.14)
project(drpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cl-bootstrap.cmake)
cl_repo()

find_package(Threads REQUIRED)

set(DRPC_SOURCES src/activity.cpp src/protocol.cpp src/client.cpp src/server.cpp)
if(WIN32)
  list(APPEND DRPC_SOURCES src/connection_win.cpp)
else()
  list(APPEND DRPC_SOURCES src/connection_posix.cpp)
endif()

add_library(drpc ${DRPC_SOURCES})
target_include_directories(drpc PUBLIC include PRIVATE src)
target_link_libraries(drpc PUBLIC Threads::Threads)
cl_add_dep(drpc nlohmann_json PUBLIC)

option(BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})

if(BUILD_EXAMPLES)
  add_executable(drpc_client_example examples/client_example.cpp)
  target_link_libraries(drpc_client_example PRIVATE drpc)

  add_executable(drpc_server_example examples/server_example.cpp)
  target_link_libraries(drpc_server_example PRIVATE drpc)
endif()
