# M5 Design

## M4 fixes
My M4 could leave an old build running after Reset or Add Document. I changed it so outdated builds stop and failed rebuilds leave Search, Inspect, and Context disabled. I also replaced the two fixed-delay stale-build tests with bounded checks of actual events.

## Threads and shared data
The GUI thread owns `MainWindow`, its widgets, corpus states, the live `Workspace`, and the current `ProcessingCore`. `AsyncCorpusBuild::work_loop()` runs on one `std::thread` from the first build through Exit. CR-1 uses another thread to receive socket requests.

The GUI and worker share `pending_`, `results_`, `factory_`, `active_`, and `waiting_` under `AsyncCorpusBuild::mutex_`. `generation_` and `stopping_` are atomic so cancellation can be checked during a document. `request_cv_` waits for work, `delay_cv_` interrupts a simulated build delay, and `result_cv_` wakes test waiters. The worker builds a private workspace snapshot, not the GUI's live workspace.

For GUI notifications, `notification_read_fd_` and `notification_write_fd_` form a nonblocking pipe. Background threads only write a byte with `notify_gui()`. A `QSocketNotifier` calls `handle_notifications()` on the GUI thread, which drains the pipe and takes completed results. Remote filenames are shared between the socket and GUI threads using `remote_files_` protected by `remote_files_mutex_`. The pipe descriptors stay open until both background threads have joined. Qt widgets and events are only accessed on the GUI thread.

## States

| State           | Meaning                         | Changes                                                   |
|-----------------|---------------------------------|-----------------------------------------------------------|
| `EmptyState`    | No documents                    | Add -> Dirty; Reset -> Empty                              |
| `DirtyState`    | Documents but no current corpus | Build -> Building; Reset -> Empty                         |
| `BuildingState` | A snapshot is being built       | Success -> Ready; failure or Add -> Dirty; Reset -> Empty |
| `ReadyState`    | Corpus matches workspace        | Build -> Building; Add -> Dirty; Reset -> Empty           |

Each state object handles its own events on the GUI thread. Late results are ignored unless the current Building state accepts the revision.

## Example: Reset during a build
1. GUI: `build_corpus()` calls `begin_build()`, which sends a snapshot.
2. Worker: `work_loop()` wakes from `request_cv_` and begins processing.
3. GUI: `reset_application()` calls the state's `reset()`; `request_cancel()` changes the generation.
4. Worker: `delay_cv_` wakes the delay; `rebuild_cancelable()` stops early.
5. Worker: `notify_gui()` writes to the pipe. GUI: `handle_notifications()` gets the result and Empty ignores it.

## Three decisions
1. Build is disabled while Building. After a workspace change, `request_cancel()` and `begin_build()` let the new request replace the old one.
2. A remote add goes through `handle_notifications()` and `add_document_file()`. It cancels the old build, shows the new file, and asks the user to rebuild.
3. Only the request/result/status data, cancel signals, remote filename queue, and notification pipe cross threads. Sharing widgets or the live workspace would be unsafe and unnecessary.

## Faults
`AIWS_FAULT_NO_CANCEL` is in `cancelled()` and is caught by the long-build test. `AIWS_FAULT_SILENT_SHUTDOWN` is in `stop()` and is caught by the idle Exit test.
