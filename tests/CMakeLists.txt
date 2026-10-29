cmake_minimum_required(VERSION 3.25)
project(IsolateHostControlTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Exercise the wire ABI against Cap'n Proto 1.x, independently of workerd's 2.x.
set(ISOLATE_TEST_CAPNP_SOURCE "" CACHE PATH "Existing Cap'n Proto 1.x source checkout")
if(NOT ISOLATE_TEST_CAPNP_SOURCE)
  include(FetchContent)
  FetchContent_Declare(client_capnp
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG fd508f325ed65f9d6cdf10a0f4511aa53b5659fc)
  FetchContent_GetProperties(client_capnp)
  if(NOT client_capnp_POPULATED)
    FetchContent_Populate(client_capnp)
  endif()
  set(ISOLATE_TEST_CAPNP_SOURCE "${client_capnp_SOURCE_DIR}")
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_OPENSSL OFF CACHE STRING "" FORCE)
set(WITH_ZLIB OFF CACHE STRING "" FORCE)
set(WITH_FIBERS OFF CACHE STRING "" FORCE)
add_subdirectory("${ISOLATE_TEST_CAPNP_SOURCE}/c++" capnp EXCLUDE_FROM_ALL)
set(_capnp_src "${ISOLATE_TEST_CAPNP_SOURCE}/c++/src")
target_compile_features(kj PUBLIC cxx_std_17)
target_compile_options(kj PUBLIC -include cstdint)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
set(sources)
foreach(schema isolate-host isolate-worker-source capnp/compat/byte-stream capnp/compat/http-over-capnp)
  if(schema MATCHES "^capnp/")
    set(prefix "${_capnp_src}")
  else()
    set(prefix "${CMAKE_CURRENT_SOURCE_DIR}/../protocol")
  endif()
  add_custom_command(OUTPUT "${generated}/${schema}.capnp.c++" "${generated}/${schema}.capnp.h"
    COMMAND $<TARGET_FILE:capnp_tool> compile
      -o "$<TARGET_FILE:capnpc_cpp>:${generated}"
      --src-prefix "${prefix}" -I "${_capnp_src}" "${prefix}/${schema}.capnp"
    DEPENDS "${prefix}/${schema}.capnp" capnp_tool capnpc_cpp VERBATIM)
  list(APPEND sources "${generated}/${schema}.capnp.c++")
endforeach()
add_executable(isolate-host-control-client control-client.c++ ${sources}
  "${_capnp_src}/capnp/compat/byte-stream.c++"
  "${_capnp_src}/capnp/compat/http-over-capnp.c++")
target_include_directories(isolate-host-control-client PRIVATE "${generated}" "${_capnp_src}")
target_compile_features(isolate-host-control-client PRIVATE cxx_std_17)
target_link_libraries(isolate-host-control-client PRIVATE capnp-rpc kj-http)
