cmake_minimum_required(VERSION 3.16)
project(shadnet VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# === Runtime library: must match Qt6 which always uses DLL runtime ===
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    # /MD (release) or /MDd (debug) — DLL runtime, same as Qt6.
    # protobuf_MSVC_STATIC_RUNTIME=OFF ensures protobuf follows the same setting.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_compile_options(/EHsc /utf-8)
    # Remove /VERBOSE:LIB from link options (noisy, only needed for debugging)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent HttpServer)

add_subdirectory(externals)

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/shadnet.pb.cc"
        "${PROTO_OUT}/shadnet.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
        protoc
    COMMENT "Generating shadnet protobuf sources"
    VERBATIM
)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/config.h
    src/server.cpp
    src/server.h
    src/client_session.cpp
    src/client_session.h
    src/protocol.h
    src/stream_extractor.h
    src/database.cpp
    src/database.h
    src/cmd_account.cpp
    src/cmd_friend.cpp
    src/score_types.h
    src/score_db.h
    src/score_db.cpp
    src/score_cache.cpp
    src/score_cache.h
    src/score_files.h
    src/cmd_score.cpp
    src/proto_utils.h
    "${PROTO_OUT}/shadnet.pb.cc"
    # Matchmaking
    src/matching.h
    src/cmd_matching.cpp
    src/stun_server.h
    src/stun_server.cpp
    # WebAPI HTTP/JSON listener
    src/webapi_server.h
    src/webapi_server.cpp
    src/webapi_auth.h
    src/webapi_auth.cpp
    src/webapi_routes_users.h
    src/webapi_routes_users.cpp
    src/webapi_routes_profile.h
    src/webapi_routes_profile.cpp
    src/webapi_routes_presence.h
    src/webapi_routes_presence.cpp
    src/webapi_routes_session.h
    src/webapi_routes_session.cpp
    src/bloodborne_summon_broker.h
    src/bloodborne_summon_broker.cpp
    src/webapi_routes_bloodborne.h
    src/webapi_routes_bloodborne.cpp
    src/webapi_routes_common.h
    # Read-only stats HTTP/JSON listener
    src/stats_server.h
    src/stats_server.cpp
)

add_executable(shadnet ${SOURCES})

target_include_directories(shadnet PRIVATE
    src
    "${PROTO_OUT}"                                    
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)

target_link_libraries(shadnet PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::HttpServer
    libprotobuf
)

if(BUILD_TESTING)
    add_executable(test_multiplayer_relay EXCLUDE_FROM_ALL
        tests/test_multiplayer_relay.cpp src/stun_server.cpp)
    target_include_directories(test_multiplayer_relay PRIVATE src tests "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src")
    target_link_libraries(test_multiplayer_relay PRIVATE Qt6::Core Qt6::Network Qt6::Sql libprotobuf)
    add_test(NAME multiplayer_relay COMMAND test_multiplayer_relay)
    add_executable(test_bloodborne_summon_broker EXCLUDE_FROM_ALL
        tests/test_bloodborne_summon_broker.cpp
        src/bloodborne_summon_broker.cpp
    )
    target_include_directories(test_bloodborne_summon_broker PRIVATE src)
    target_link_libraries(test_bloodborne_summon_broker PRIVATE Qt6::Core)
    add_test(NAME bloodborne_summon_broker COMMAND test_bloodborne_summon_broker)

    add_executable(bloodborne_matching_guest_probe EXCLUDE_FROM_ALL
        tests/bloodborne_matching_guest_probe.cpp
        "${PROTO_OUT}/shadnet.pb.cc"
    )
    target_include_directories(bloodborne_matching_guest_probe PRIVATE
        src
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(bloodborne_matching_guest_probe PRIVATE libprotobuf)

    add_executable(transparent_relay_probe EXCLUDE_FROM_ALL
        tests/transparent_relay_probe.cpp
        src/stun_server.cpp
    )
    target_include_directories(transparent_relay_probe PRIVATE
        src
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(transparent_relay_probe PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Sql
        libprotobuf
    )
endif()

add_custom_command(TARGET shadnet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/worlds.cfg"
        "$<TARGET_FILE_DIR:shadnet>/worlds.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scoreboards.cfg"
        "$<TARGET_FILE_DIR:shadnet>/scoreboards.cfg"
)


install(TARGETS shadnet RUNTIME DESTINATION bin)
install(FILES worlds.cfg scoreboards.cfg DESTINATION bin)
