message(STATUS "Using RPC backend")

ggml_add_backend_library(ggml-rpc
                         ggml-rpc.cpp
                         transport.cpp
                        )

if (WIN32)
    target_link_libraries(ggml-rpc PRIVATE ws2_32)
endif()

# RDMA auto-detection: Linux RoCE/IB via libibverbs, Apple RDMA-over-Thunderbolt via librdma
if (APPLE)
    set(RDMA_LIB_NAME rdma)
    set(RDMA_DESC "Apple RDMA-over-Thunderbolt, UC")
elseif (NOT WIN32)
    set(RDMA_LIB_NAME ibverbs)
    set(RDMA_DESC "auto-detected")
endif()

if (RDMA_LIB_NAME)
    find_library(RDMA_LIB ${RDMA_LIB_NAME})
    if (RDMA_LIB)
        option(GGML_RPC_RDMA "ggml: enable RDMA transport for RPC" ON)
    else()
        option(GGML_RPC_RDMA "ggml: enable RDMA transport for RPC" OFF)
    endif()
else()
    set(GGML_RPC_RDMA OFF CACHE BOOL "RDMA not available on this platform" FORCE)
endif()

if (GGML_RPC_RDMA)
    if (NOT RDMA_LIB)
        find_library(RDMA_LIB ${RDMA_LIB_NAME} REQUIRED)
    endif()
    target_compile_definitions(ggml-rpc PRIVATE GGML_RPC_RDMA)
    if (APPLE)
        # librdma.dylib only exists on macOS 26.2 and later. Link it weakly so a build made
        # where it exists still loads where it does not; checked at runtime before use
        # but with BUILD_SHARED_LIBS=OFF ggml-rpc is a static archive and never links
        # so the librdma symbols used by transport-apple.cpp stay undefined.
        target_link_options(ggml-rpc PUBLIC "LINKER:-weak_library,${RDMA_LIB}")
        target_compile_definitions(ggml-rpc PRIVATE GGML_RPC_RDMA_APPLE)
        target_sources(ggml-rpc PRIVATE transport-apple.cpp)
    else()
        target_link_libraries(ggml-rpc PRIVATE ${RDMA_LIB})
    endif()
    message(STATUS "  RDMA transport enabled (${RDMA_DESC})")
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
