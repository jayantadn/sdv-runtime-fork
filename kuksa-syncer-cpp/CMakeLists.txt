# Copyright (c) 2025 Eclipse Foundation.
#
# This program and the accompanying materials are made available under the
# terms of the MIT License which is available at
# https://opensource.org/licenses/MIT.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(kuksa-syncer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─── Required system packages ──────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(absl CONFIG REQUIRED)   # abseil must be found before gRPC
find_package(OpenSSL REQUIRED)

# gRPC: prefer cmake config (installed from source / vcpkg / conan).
# Ubuntu 22.04 apt ships gRPC 1.30.x which only provides pkg-config files,
# so fall back to pkg-config and synthesise the same IMPORTED target names
# that gRPCConfig.cmake would normally create.
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_GRPCPP   REQUIRED grpc++)
    pkg_check_modules(PC_GRPCPP_U REQUIRED grpc++_unsecure)

    find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED
                 DOC "gRPC C++ protoc plugin")

    # grpc++ (used by mock-provider)
    add_library(gRPC::grpc++ INTERFACE IMPORTED)
    set_target_properties(gRPC::grpc++ PROPERTIES
        INTERFACE_LINK_LIBRARIES  "${PC_GRPCPP_LINK_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${PC_GRPCPP_INCLUDE_DIRS}")

    # grpc++_unsecure (used by kuksa-syncer / kuksa-syncer-tls)
    add_library(gRPC::grpc++_unsecure INTERFACE IMPORTED)
    set_target_properties(gRPC::grpc++_unsecure PROPERTIES
        INTERFACE_LINK_LIBRARIES  "${PC_GRPCPP_U_LINK_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${PC_GRPCPP_U_INCLUDE_DIRS}")

    # grpc++_reflection has no .pc file — link the .so directly
    add_library(gRPC::grpc++_reflection INTERFACE IMPORTED)
    set_target_properties(gRPC::grpc++_reflection PROPERTIES
        INTERFACE_LINK_LIBRARIES "grpc++_reflection")

    # Executable plugin — consumed via get_target_property(... LOCATION)
    add_executable(gRPC::grpc_cpp_plugin IMPORTED)
    set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
        IMPORTED_LOCATION "${_GRPC_CPP_PLUGIN}")

    message(STATUS "gRPC found via pkg-config (${PC_GRPCPP_VERSION})")
endif()

# ─── FetchContent dependencies ─────────────────────────────────────────────
include(FetchContent)

# nlohmann/json (header-only) — use system package if available (e.g. nlohmann-json3-dev),
# fall back to FetchContent so the build works without network access for that package.
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
    message(STATUS "nlohmann_json: using FetchContent")
else()
    message(STATUS "nlohmann_json: using system package (${nlohmann_json_VERSION})")
endif()

# socket.io-client-cpp
FetchContent_Declare(
    sioclient
    GIT_REPOSITORY https://github.com/socketio/socket.io-client-cpp.git
    GIT_TAG        3.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(sioclient)
if(NOT sioclient_POPULATED)
    FetchContent_Populate(sioclient)

    # Patch 1: close_impl() prints "Error: No active session" when m_con is expired,
    # which is an expected condition during reconnect loops (connect() failed, then
    # sync_close() is called to join the network thread). Silently return instead.
    set(_sio_impl "${sioclient_SOURCE_DIR}/src/internal/sio_client_impl.cpp")
    if(EXISTS "${_sio_impl}")
        file(READ "${_sio_impl}" _sio_content)
        string(REPLACE
            "cerr << \"Error: No active session\" << endl;"
            "return; // No active session — expected during reconnect loops, nothing to close"
            _sio_patched "${_sio_content}")

        # Patch 2: on_ping() uses the throwing overload of websocketpp::send().
        # If m_con expires between the check and the send (TOCTOU), websocketpp
        # throws bad_connection uncaught in the network thread → std::terminate().
        # Use the error_code overload instead, matching the safe pattern in ping().
        string(REPLACE
            "this->m_client.send(this->m_con, *payload, frame::opcode::text);"
            "lib::error_code ec; this->m_client.send(this->m_con, *payload, frame::opcode::text, ec);"
            _sio_patched "${_sio_patched}")

        file(WRITE "${_sio_impl}" "${_sio_patched}")
        message(STATUS "sioclient: applied patches (close_impl + on_ping)")
    endif()

    # Build with TLS support
    set(BUILD_PT_SIOCLIENT     OFF CACHE BOOL "" FORCE)
    set(BUILD_PT_SIOCLIENT_TLS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${sioclient_SOURCE_DIR} ${sioclient_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# ─── Protobuf / gRPC code generation ───────────────────────────────────────
set(PROTO_DIR    "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_BINDIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_BINDIR}/kuksa/val/v1")

get_target_property(_PROTOC  protobuf::protoc         LOCATION)
get_target_property(_GRPC_PL gRPC::grpc_cpp_plugin    LOCATION)

set(PROTO_FILES
    "${PROTO_DIR}/kuksa/val/v1/types.proto"
    "${PROTO_DIR}/kuksa/val/v1/val.proto"
)

foreach(_proto ${PROTO_FILES})
    get_filename_component(_name  ${_proto} NAME_WE)
    get_filename_component(_pdir  ${_proto} DIRECTORY)
    # Relative path from PROTO_DIR to keep directory structure in output
    file(RELATIVE_PATH _rel "${PROTO_DIR}" "${_pdir}")

    set(_pb_h  "${PROTO_BINDIR}/${_rel}/${_name}.pb.h")
    set(_pb_cc "${PROTO_BINDIR}/${_rel}/${_name}.pb.cc")
    set(_gr_h  "${PROTO_BINDIR}/${_rel}/${_name}.grpc.pb.h")
    set(_gr_cc "${PROTO_BINDIR}/${_rel}/${_name}.grpc.pb.cc")

    add_custom_command(
        OUTPUT  ${_pb_h} ${_pb_cc} ${_gr_h} ${_gr_cc}
        COMMAND ${_PROTOC}
                --cpp_out=${PROTO_BINDIR}
                --grpc_out=${PROTO_BINDIR}
                --plugin=protoc-gen-grpc=${_GRPC_PL}
                -I${PROTO_DIR}
                ${_proto}
        DEPENDS ${_proto}
        COMMENT "Generating protobuf/gRPC sources for ${_name}"
    )

    list(APPEND _PROTO_SRCS ${_pb_cc})
    list(APPEND _GRPC_SRCS  ${_gr_cc})
endforeach()

# ─── Common source list ─────────────────────────────────────────────────────
set(_SYNCER_SRCS
    src/main.cpp
    src/syncer.cpp
    src/kuksa_client.cpp
    src/subpiper.cpp
    src/utils.cpp
    src/project_utils.cpp
    src/vehicle_model_manager.cpp
    ${_PROTO_SRCS}
    ${_GRPC_SRCS}
)

# ─── kuksa-syncer (plain HTTP — for http:// Kit Server URLs) ───────────────
add_executable(kuksa-syncer ${_SYNCER_SRCS})

target_include_directories(kuksa-syncer PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${PROTO_BINDIR}"
    "${sioclient_SOURCE_DIR}/src"
)

target_link_libraries(kuksa-syncer PRIVATE
    Threads::Threads
    protobuf::libprotobuf
    gRPC::grpc++_unsecure    # no embedded BoringSSL
    nlohmann_json::nlohmann_json
    sioclient                # non-TLS: for http:// connections
)

target_compile_options(kuksa-syncer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

# ─── kuksa-syncer-tls (HTTPS/WSS — for https:// Kit Server URLs) ──────────
add_executable(kuksa-syncer-tls ${_SYNCER_SRCS})

target_include_directories(kuksa-syncer-tls PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${PROTO_BINDIR}"
    "${sioclient_SOURCE_DIR}/src"
    "${OPENSSL_INCLUDE_DIR}"
)

target_link_libraries(kuksa-syncer-tls PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    protobuf::libprotobuf
    gRPC::grpc++_unsecure    # no embedded BoringSSL, safe alongside OpenSSL
    nlohmann_json::nlohmann_json
    sioclient_tls            # TLS: for https:// connections
)

target_compile_options(kuksa-syncer-tls PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

# ─── Compiler warnings ─────────────────────────────────────────────────────

# ─── mock-provider binary ──────────────────────────────────────────────────
# C++ equivalent of mockprovider.py + mockservice.py + mock.py
# Reads signals.json, writes initial values to KUKSA databroker, and
# reflects actuator-target changes back as current values (actuator follow).
add_executable(mock-provider
    src/mock_main.cpp
    src/mock_provider.cpp
    src/mock_service.cpp
    src/mock_action.cpp
    src/mock_trigger.cpp
    src/mock_animator.cpp
    src/mock_datapoint.cpp
    src/mock_behavior.cpp
    src/kuksa_client.cpp
    ${_PROTO_SRCS}
    ${_GRPC_SRCS}
)

target_include_directories(mock-provider PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${PROTO_BINDIR}"
)

target_link_libraries(mock-provider PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    protobuf::libprotobuf
    gRPC::grpc++
    gRPC::grpc++_reflection
    nlohmann_json::nlohmann_json
)

target_compile_options(mock-provider PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

# ─── Install ───────────────────────────────────────────────────────────────
install(TARGETS kuksa-syncer kuksa-syncer-tls mock-provider RUNTIME DESTINATION bin)
