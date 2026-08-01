message(STATUS "Using RPC backend")

ggml_add_backend_library(ggml-rpc
                         ggml-rpc.cpp
                         transport.cpp
                        )

if (WIN32)
    target_link_libraries(ggml-rpc PRIVATE ws2_32)
endif()

# RDMA auto-detection (Linux only, requires libibverbs)
if (NOT WIN32 AND NOT APPLE)
    find_library(IBVERBS_LIB ibverbs)
    if (IBVERBS_LIB)
        option(GGML_RPC_RDMA "ggml: enable RDMA transport for RPC" ON)
    else()
        option(GGML_RPC_RDMA "ggml: enable RDMA transport for RPC" OFF)
    endif()
else()
    set(GGML_RPC_RDMA OFF CACHE BOOL "RDMA not available on this platform" FORCE)
endif()

if (GGML_RPC_RDMA)
    if (NOT IBVERBS_LIB)
        find_library(IBVERBS_LIB ibverbs REQUIRED)
    endif()
    target_compile_definitions(ggml-rpc PRIVATE GGML_RPC_RDMA)
    target_link_libraries(ggml-rpc PRIVATE ${IBVERBS_LIB})
    message(STATUS "  RDMA transport enabled (auto-detected)")
else()
    message(STATUS "  RDMA transport disabled")
endif()

# NetworkDirect transport (Windows only, needs a NetworkDirect SDK checkout)
if (WIN32)
    option(GGML_RPC_ND "ggml: enable NetworkDirect RDMA transport for RPC" OFF)
    set(GGML_RPC_ND_SDK "" CACHE PATH "ggml: path to a NetworkDirect checkout providing src/ndutil")
else()
    set(GGML_RPC_ND OFF CACHE BOOL "NetworkDirect not available on this platform" FORCE)
endif()

if (GGML_RPC_ND)
    if (NOT GGML_RPC_ND_SDK)
        message(FATAL_ERROR "GGML_RPC_ND requires GGML_RPC_ND_SDK to point at a NetworkDirect checkout")
    endif()

    set(ND_UTIL_DIR "${GGML_RPC_ND_SDK}/src/ndutil")
    foreach (ND_SRC ndaddr.cpp ndfrmwrk.cpp ndprov.cpp)
        if (NOT EXISTS "${ND_UTIL_DIR}/${ND_SRC}")
            message(FATAL_ERROR "GGML_RPC_ND_SDK does not contain src/ndutil/${ND_SRC}")
        endif()
    endforeach()

    # ndstatus.h is generated from ndstatus.mc into the build output tree
    find_path(GGML_RPC_ND_STATUS_DIR ndstatus.h
              PATHS "${GGML_RPC_ND_SDK}/out"
              PATH_SUFFIXES Release-x64/include Debug-x64/include
              NO_DEFAULT_PATH)
    if (NOT GGML_RPC_ND_STATUS_DIR)
        message(FATAL_ERROR "ndstatus.h not found under ${GGML_RPC_ND_SDK}/out - build the NetworkDirect SDK first")
    endif()

    # the prebuilt ndutil.lib is /MT, so compile the loader sources in instead of linking it
    set(ND_UTIL_SOURCES
        "${ND_UTIL_DIR}/ndaddr.cpp"
        "${ND_UTIL_DIR}/ndfrmwrk.cpp"
        "${ND_UTIL_DIR}/ndprov.cpp")
    target_sources(ggml-rpc PRIVATE ${ND_UTIL_SOURCES})
    set_source_files_properties(${ND_UTIL_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

    target_include_directories(ggml-rpc PRIVATE "${ND_UTIL_DIR}" "${GGML_RPC_ND_STATUS_DIR}")
    target_compile_definitions(ggml-rpc PRIVATE GGML_RPC_ND _CRT_SECURE_NO_WARNINGS)
    message(STATUS "  NetworkDirect transport enabled (${GGML_RPC_ND_SDK})")
else()
    message(STATUS "  NetworkDirect transport disabled")
endif()
