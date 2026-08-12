cmake_minimum_required(VERSION 3.14)
project(rtspcli VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RTSPCLI_TOPLEVEL ON)
else()
  set(RTSPCLI_TOPLEVEL OFF)
endif()

option(RTSPCLI_BUILD_EXAMPLES "Build rtspcli examples" ${RTSPCLI_TOPLEVEL})

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(rtspcli STATIC
  src/client.cpp
  src/sdp.cpp
  src/depack.cpp
)
add_library(rtspcli::rtspcli ALIAS rtspcli)

target_include_directories(rtspcli PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(rtspcli PUBLIC cxx_std_17)
target_link_libraries(rtspcli
  PRIVATE CURL::libcurl
  PUBLIC Threads::Threads
)
set_target_properties(rtspcli PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(RTSPCLI_BUILD_EXAMPLES)
  add_executable(rtsp_dump examples/rtsp_dump.cpp)
  target_link_libraries(rtsp_dump PRIVATE rtspcli::rtspcli)
endif()

include(GNUInstallDirs)
install(TARGETS rtspcli EXPORT rtspcli-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/rtspcli DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT rtspcli-targets
  NAMESPACE rtspcli::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtspcli)
