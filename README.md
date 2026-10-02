# llama.cpp

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp?filter=v*&color=brightgreen)](https://github.com/ggml-org/llama.cpp/releases?q=tag:v0)
[![Nightly](https://img.shields.io/github/v/release/ggml-org/llama.cpp?label=nightly&filter=b*&color=orange)](https://github.com/ggml-org/llama.cpp/releases?q=b)
[![Server](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/server.yml?label=Server)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)
[![Docker](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/docker.yml?label=Docker)](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml)
[![Winget](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/winget.yml?label=Winget)](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml)

[ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Ajhen0409%20OR%20author%3Abartowski1182%20OR%20author%3Anikwen%20OR%20author%3Ahipudding%20OR%20author%3Aravi9%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3Amarty1885%20OR%20author%3A0cc4m%20OR%20author%3ATitaniumtown%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Awine99%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc) / [dev stats](https://github.com/ggml-org/llama.cpp-dev) / [lib llama API](https://github.com/ggml-org/llama.cpp/issues/9289) / [llama-server REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

</div>

> [!WARNING]
> **This is an experimental fork of llama.cpp, not upstream.**
>
> It carries prototype work on the RPC backend (a Windows NetworkDirect RDMA transport and transport-level send batching), a tracing facility that emits an HTML latency report, and a Windows SYCL packaging script. None of it has been reviewed by llama.cpp maintainers and none of it is ready for submission as-is. Expect rough edges, and do not assume upstream behaviour where these features are involved.
>
> See [Experimental features](#experimental-features) below. Everything else in this README is upstream documentation.

## Experimental features

### Windows NetworkDirect (RDMA) transport for RPC

An RDMA transport for the RPC backend built on the Windows NetworkDirect Service Provider Interface (NDSPI). It fills the same role on Windows that the existing `GGML_RPC_RDMA` libibverbs/RoCEv2 path fills on Linux. TCP and libibverbs are untouched and remain available.

Build it with a NetworkDirect SDK checkout:

```sh
cmake -B build -DGGML_RPC=ON -DGGML_RPC_ND=ON -DGGML_RPC_ND_SDK=C:/path/to/NetworkDirect
cmake --build build --config Release
```

The SDK checkout must provide `src/ndutil/{ndaddr,ndfrmwrk,ndprov}.cpp`, which are compiled in directly because the prebuilt `ndutil.lib` is `/MT`, and a generated `ndstatus.h` under `out/Release-x64/include` or `out/Debug-x64/include`. `ndstatus.h` is produced from `ndstatus.mc`, so build the SDK first. CMake fails with an explicit message if either is missing.

There are no command-line changes. The transport is negotiated during the HELLO handshake, and if no provider is installed or the adapter cannot be opened, both peers silently stay on TCP. A NetworkDirect-capable server reports:

```
  transport      : TCP (NetworkDirect auto-negotiate enabled)
```

Set `GGML_RPC_NO_RDMA=1` on both peers to explicitly use TCP.

Notes:

- The TCP connection is what selects the local adapter. If RDMA and TCP run over different NICs, set `GGML_ND_ADDR` on both peers to the IPv4 address of the RDMA NIC.
- ND connection management uses port `23517`, separate from the RPC port. Different worker IPs can use it simultaneously; additional listeners on the same local IP and port fall back to TCP.
- Data moves as two-sided Send/Receive in 256 KiB chunks behind a 16-byte framing header, with 24 pre-posted receives and an ACK-based credit window.

See [tools/rpc/README.md](tools/rpc/README.md) for more detail.

### Batched RPC commands

`socket_t` gained `cork()` and `uncork()`. Sends issued between the two are batched into as few transport messages as possible.

`send_rpc_cmd` writes the command byte, the payload size and the payload as three separate sends. On a message-oriented transport each of those blocks for its own completion, so every command cost three round trips; it is now one. Bulk payloads larger than one 256 KiB chunk still split across messages, as they must.

This is a no-op on TCP, where the kernel already coalesces. It is also wire-compatible in both directions: the receive path already handles a partially consumed message, so a batching client works against a non-batching server.

### Tensor-parallel / RPC trace report

A lightweight tracing facility (`ggml/include/ggml-trace.h`) that records tensor-parallel allreduces and RPC traffic and writes a self-contained HTML report. All entry points compile to no-ops when tracing is off.

```sh
set GGML_TRACE=1
llama-cli -m model.gguf -ngl 999 --rpc 192.168.1.10:50052 --split-mode tensor
```

The report is written to `llama-trace.html`. `llama-cli` writes it on `/exit`; an `atexit` handler is the fallback for other exits.

It contains summary cards (decode steps, allreduce count, butterfly fallback vs native comm, transfer volume, bytes sent and received, wire bytes per decode step), a per-endpoint byte table, and a collapsible timeline. Each decode step expands into its allreduces, and each allreduce into the individual tensor transfers and RPC calls it produced, with per-command totals.

| Variable | Default | Meaning |
| --- | --- | --- |
| `GGML_TRACE` | off | set to `1` to enable tracing |
| `GGML_TRACE_HTML` | `llama-trace.html` | output path |
| `GGML_TRACE_MAX_STEPS` | 64 | decode steps recorded in detail; totals stay exact past this |
| `GGML_TRACE_MAX_EVENTS` | 4096 | rows stored per bucket; totals stay exact past this |

### Windows SYCL drop-in build script

`sycl-build.ps1` builds the SYCL backend with Intel oneAPI and vcpkg.

```powershell
.\sycl-build.ps1                 # plain SYCL build into .\build
.\sycl-build.ps1 fp16            # same, with GGML_SYCL_F16
.\sycl-build.ps1 -DropIn         # redistributable package matching the official release layout
```

`-DropIn` runs two configures, because `icx` sets `MSVC=TRUE` and therefore takes the MSVC branch in `ggml-cpu/CMakeLists.txt`, which hand-defines `__AVX512VNNI__` and friends instead of enabling the matching clang target features. It produces a `cl` host build (`GGML_BACKEND_DL`, `GGML_CPU_ALL_VARIANTS`, `GGML_RPC`, plus NetworkDirect when the SDK is present) and an `icx` build of `ggml-sycl.dll` only, then assembles `build-dist` from both along with the oneAPI runtime DLLs. `ONEAPI_ROOT` must be set for the runtime collection step.

`-NdSdk` defaults to `C:/repo/local/NetworkDirect`; if that path does not exist the script says so and builds RPC as TCP only. Other parameters: `-BuildDir`, `-VcpkgToolchain`, `-VcpkgTriplet`, `-OneApiSetvars`, and any trailing arguments are passed through to CMake.

### RPC connection cache

The dispatcher cache owns each connection, so device queries do not repeatedly reconnect when no buffer holds it. This matters for RDMA and NetworkDirect, where a reconnect means rebuilding the queue pair, memory registrations and completion queue.

### Debugging

Set `GGML_RPC_DEBUG=1` on both peers to trace transport bring-up: adapter selection and capabilities, memory registration, completion queue and queue pair creation, and each stage of the connection handshake. Every point that can decline to TCP reports why.

On the client, these messages go through the `llama-cli` log filter, so `GGML_RPC_DEBUG=1` alone is not enough. Add `-v` (or `-lv 5`). The server prints them unconditionally.

----

## Quick start

A few options to get `llama.cpp` installed on your machine:

```bash
# curl
curl -LsSf https://llama.app/install.sh | sh

# powershell
irm https://llama.app/install.ps1 | iex
```

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [nothings/stb](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
