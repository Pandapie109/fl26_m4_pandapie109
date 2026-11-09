# Threads and ownership
I kept the Workspace, GUI widgets, and the current ProcessingCore in MainWindow on the Qt GUI thread. When Build Corpus is clicked, MainWindow::build_corpus() gives AsyncCorpusBuild a copy of the Workspace and its revision number. AsyncCorpusBuild::start() uses a std::thread to build a separate ProcessingCore. A std::promise and std::future transfer the result, which MainWindow::poll_build_completion() checks using a 20 ms QTimer. Both threads are joined during shutdown.

For the shared-data requirement, the worker reads AsyncCorpusBuild::cancel_ while the GUI thread can change it, so it is an std::atomic<bool>. The IPC thread and GUI thread similarly share ipc::Server::stopping_ as an atomic. The promise/future handles the build result transfer. The server callback and descriptor stay unchanged while its thread runs. The worker owns its Workspace copy; live Workspace, published corpus, revision, and widgets stay on the GUI thread. The IPC thread queues an event instead of changing the GUI itself.

# Corpus state and completed builds
Whenever a document is added or Reset is clicked, I increment workspace_revision_ and mark the corpus as not current. A finished build is accepted only if it succeeded and its revision still matches the Workspace. Otherwise, it is discarded. A failure shows an error and allows rebuilding. Search, Inspect, and Context are only enabled when the corpus is current. Build Corpus is disabled during a build.

Changes from my design checkpoint
My original DESIGN_NOTE.md proposed using QThread and running search operations on the worker. I changed that because M4 requires standard C++ threading. I used std::thread for rebuilding and left Search, Inspector, and Context on the GUI thread, where they use the completed corpus. The checkpoint note is unchanged.

# CR-1 remote add
I added ipc::Server::start(), run(), and serve(), plus ipc::send_add() and the separate aiws_send program. The server receives requests on its own thread, checks whether the file is readable, and queues MainWindow::remote_document_arrived(). It calls add_document_file(), giving remote adds the same invalidation behavior. The server replies without waiting for the GUI.

I used a POSIX Unix-domain stream socket with this byte format: a request starts with a 4-byte unsigned big-endian path length, followed by that many UTF-8 bytes. A reply has a 1-byte status (0 for accepted, 1 for rejected), a 4-byte unsigned big-endian reason length, and the UTF-8 reason text. Paths are limited to 1 MiB. Length-prefixing supports spaces and newlines. The M1/M2 processing code did not need changes for CR-1.

# Fault tests and AI use
Both fault switches are in MainWindow::poll_build_completion(). adding_while_building_never_accepts_old_corpus() and reset_during_build_never_restores_stale_result() catch stale results. failure_returns_to_recoverable_state() catches the lost failure notification. Other tests cover normal builds, remote addition, split messages, and shutdown. I used ChatGPT to help explain parts of M4 so I could understand them better and draft some tests. I built and tested the project in the course Docker environment.

# Pair Programming Log
Me: Daniel Robles-Hinckley
Partner: Pranav Narayan
============================================================
SESSION 1 - MY M4 CODE
============================================================
Date: 11/6/2026
Start Time: 8:00 PM
End Time:   9:00 PM
Code Worked On: CR-1 Remote Add

Time                 Driver       Navigator    Work
-------------------  -----------  -----------  ------------------------------
2:00 - 2:15 PM       Me           Partner      Set up the Unix-domain socket and discussed receiving paths.
2:15 - 2:30 PM       Partner      Me           Worked on the message format and receiving data in pieces.
2:30 - 2:45 PM       Me           Partner      Connected remote requests to add Document behavior.
2:45 - 3:00 PM       Partner      Me           Reviewed errors and tested valid and invalid file paths.

During this session, we focused on getting the remote document feature working
without interfering with the GUI. When I was navigator, I suggested testing
requests that arrived in multiple pieces instead of one message. My partner
pointed out that a remote addition should also invalidate the current corpus,
just like adding a document through the GUI.

============================================================
SESSION 2 - PARTNER'S M4 CODE
============================================================
Date: 11/8/2026
Start Time: 8:00 PM
End Time:   9:00 PM
Code Worked On: CR-1 Remote Add

Time                 Driver       Navigator    Work
-------------------  -----------  -----------  ------------------------------
3:00 - 3:15 PM       Partner      Me           Reviewed the socket server and application communication.
3:15 - 3:30 PM       Me           Partner      Worked on aiws_send and its success and error responses.
3:30 - 3:45 PM       Partner      Me           Tested adding a document while a build was running.
3:45 - 4:00 PM       Me           Partner      Checked socket cleanup during shutdown and more file paths.

During this session, we worked on my partner's CR-1 implementation. As
navigator, I suggested testing remote document additions during an active
corpus build, since the completed build could otherwise be out of date.
My partner suggested checking that the socket file was removed after Exit,
so the server could start cleanly the next time.

============================================================
OVERALL REFLECTION
============================================================
Switching roles every 15 minutes worked well because both of us got a chance
to write code and review what the other person was doing. Explaining the code
while driving helped us catch issues sooner. Working together also helped us
understand how the socket communication, GUI updates, and background builds
fit together.

