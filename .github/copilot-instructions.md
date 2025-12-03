<!-- SPDX-License-Identifier: MIT
  Copyright (c) 2025 WinUDPShardedEcho contributors -->

# Copilot / AI Agent Instructions for WinUDPShardedEcho

This file gives focused, actionable guidance to AI coding agents working on this repository. Keep suggestions small, testable, and aligned with the project's design choices.

1. Big picture
- Purpose: a high-performance Windows UDP echo server and client demonstrating socket CPU affinity, per-worker IOCP, one-socket-per-core server design, and multiple-socket clients for throughput testing.
- Major components:
  - `src/server/main.cpp` — server entrypoint and worker orchestration (one worker per core; per-worker IOCP; pinned threads).
  - `src/client/main.cpp` — client entrypoint, creates per-worker sockets and posts send/recv operations for load testing.
  - `src/common/socket_utils.cpp/.hpp` — shared helpers (argument parsing, socket setup, bind/affinity, buffer sizing).
- Data flow: client(s) send UDP packets formatted as [sequence (8B), timestamp ns (8B), payload]. Server posts multiple overlapped receives per socket and either responds synchronously or with overlapped sends depending on `--sync-reply`.

2. Build & test workflows (essential commands)
- Configure (Visual Studio / multi-config):
```powershell
cmake -S . -B build
```
- Configure with MSVC AddressSanitizer and static CRT (common debug/analysis option):
```powershell
cmake -S . -B build -DENABLE_MSVC_ADDRESS_SANITIZER=ON -DUSE_DYNAMIC_MSVC_RUNTIME=OFF
```
- Build (Release):
```powershell
cmake --build build --config Release -j
```
- Open in Visual Studio:
```powershell
start .\build\WinUDPShardedEcho.sln
```
- Clean rebuild tip: remove `build` to change CRT flags or sanitizer settings to avoid linker/object mismatches.

3. Project-specific conventions & patterns
- Prefer per-target properties in CMake: use `target_compile_options` and `target_link_options` for sanitizer flags; `CMAKE_MSVC_RUNTIME_LIBRARY` is set at configure time to control `/MT` vs `/MD`.
- Socket affinity: code uses `SIO_CPU_AFFINITY` and binds sockets to specific cores. When modifying socket creation logic, ensure you preserve the per-core binding and test on multi-core setups.
- IO model:
  - Server: one socket per worker, each with multiple outstanding receives and own IOCP serviced by the worker thread.
  - Client: multiple sockets per worker are allowed; each socket is bound to its own ephemeral port (no `SO_REUSEADDR`) to increase 5-tuple entropy.
- Reply modes: `--sync-reply` toggles synchronous `sendto` vs overlapped IOCP sends. Do not remove the synchronous path — it's useful for microbenchmarks and diagnostics.

4. Important files to inspect when making changes
- `src/server/main.cpp` — worker loop, IOCP handling, posting receives, send completion handling
- `src/client/main.cpp` — rate limiting, per-worker socket setup, statistics
- `src/common/socket_utils.cpp/.hpp` — argument parsing, buffer sizing, helper functions
- `CMakeLists.txt` — build options including `ENABLE_MSVC_ADDRESS_SANITIZER` and `USE_DYNAMIC_MSVC_RUNTIME` (controls CRT)
- `README.md` — usage, architecture, expected behavior

5. Cross-component communication & integration points
- Networking: uses Windows sockets (`ws2_32`) and IOCP APIs (`CreateIoCompletionPort`, `GetQueuedCompletionStatusEx`). Watch for platform-specific headers and linking.
- External dependency: Microsoft WIL is pulled via FetchContent in `CMakeLists.txt` (`wil` repository). Changes to WIL usage should include compatibility checks with the resolved commit in `FetchContent_Declare`.

6. Code change guidance (concise)
- Keep changes small and testable; prefer adding new CMake options rather than altering default behaviors silently.
- When touching IO or threading code, include a short local test plan (e.g., run `echo_server` on 2 cores, run `echo_client` with `--sockets 4 --rate 10000 --duration 5` and confirm packets/second and no crashes).
- Preserve configurability: maintain `--sync-reply`, `--cores`, `--sockets` CLI flags and ensure defaults match README.

7. Examples and patterns from the codebase
- Argument parsing: see `src/common/arg_parser.hpp` for how CLI flags are parsed and validated — follow similar validation style.
- Socket initialization: `socket_utils.cpp` contains buffer sizing and binding logic; replicate its error handling and logging style when adding new socket features.

8. Debugging & diagnostics
- Reproduce issues locally on Windows with Visual Studio (tools like the debugger, thread window, and address sanitizer when enabled).
- Use `--sync-reply` to simplify tracing of request/response flow during debugging.
- For data correctness, enable large `--payload` and short `--duration` to inspect contents and timestamps returned by the server.

9. When to ask the maintainers
- If a proposed change alters the core architecture (e.g., moving to shared-socket model, changing IO model), ask before implementing — the design is intentionally per-core to avoid contention.
- If bumping or changing the WIL commit in `FetchContent_Declare`, confirm compatibility with maintainers.

10. Tone & PR expectations for AI agents
- Create focused changes with good commit messages, e.g., "CMake: add USE_DYNAMIC_MSVC_RUNTIME option (default static CRT)".
- Include a short test case/steps in the PR description demonstrating the change works (build + quick run command and expected log lines).

If anything above is unclear or you want more examples (unit-test skeletons, CI entries, or PR templates), tell me what to add and I will iterate.