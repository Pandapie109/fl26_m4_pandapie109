# ECE 3574 — Milestone 4: Responsive AI Workspace

This source project integrates the student's completed M3 archive with M4 background corpus building and change request CR-1 (Remote Add). The M3 GUI and M1/M2 public APIs remain in place.

## Build (course Ubuntu 22.04 Docker container)

From the project root, with Qt6 development headers/tools installed in the course container:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CMake builds `ai_workspace_m4`, `aiws_send`, `student_tests`, and a separate backend/IPC smoke test. Run the application with Qt's offscreen platform for automated work if necessary:

```sh
QT_QPA_PLATFORM=offscreen ./build/ai_workspace_m4 --build-delay-ms 300 --ipc-socket /tmp/aiws-demo.sock
```

For interactive GUI use, omit `QT_QPA_PLATFORM=offscreen` and use the course container's X display. To add a file from another process while the application runs:

```sh
./build/aiws_send --socket /tmp/aiws-demo.sock add data/public/doc_ai.txt
```

When `--ipc-socket` / `--socket` are omitted, both programs use `/tmp/aiws-$USER.sock`, or `/tmp/aiws-<numeric uid>.sock` when USER is not set. Relative file paths passed to `aiws_send` are interpreted in the sender's working directory.

`aiws_send` exit codes: **0** accepted/readable; **1** rejected/unreadable; **2** server not reachable or IPC failure. Reasons for nonzero exits are printed to stderr.

## Verification and design

`tests/student_tests.cpp` exercises corpus state, stale-build protection, failed builds and recovery, remote add, UTF-8/newlines, fragmented socket messages, and socket cleanup. It deliberately **does not** name any fault macros. The intentional fault switches reside only in `src/main_window.cpp`, as specified. `tests/backend_ipc_tests.cpp` also works without Qt to exercise the thread worker and socket protocol.

`DESIGN_NOTE.md` is the prior checkpoint document, retained unchanged. The design changes and wire protocol are described in `DESIGN.md`.


## Project structure

- `include/aiws/` — M1/M2 and GUI interfaces; background worker and socket interfaces.
- `src/` — M1/M2 backend, M3 GUI, async worker, socket server/client implementation, GUI entry point.
- `tools/aiws_send.cpp` — CR-1 separate executable.
- `tests/` — Qt component tests and independent backend/IPC smoke test.
- `data/public/` — sample documents.
