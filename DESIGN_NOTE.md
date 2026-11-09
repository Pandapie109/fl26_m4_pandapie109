# DESIGN_NOTE.md — AI Workspace Threading Plan

*Pre-implementation design note*

### 1. Components and threads

`MainWindow` owns the `Workspace`, revision counters, and application state. All four GUI widgets and `SearchResultModel` run on the main Qt thread, along with their event handlers and display updates. A `BuildWorker` runs on a separate `QThread` and owns the `ProcessingCore`. It performs rebuild, search, term inspection, and context operations. Communication uses queued signals and slots. Each rebuild receives a copied Workspace snapshot, not the GUI's live data.

### 2. Corpus states

The states are **Empty**, **Dirty**, **Building**, and **Ready**. Startup and Reset give Empty. Adding a document makes the corpus Dirty and increments the Workspace revision. Clicking Build Corpus starts Building with a snapshot and revision number. A successful build matching the latest revision changes the state to Ready; a failure returns to Dirty. An edit or Reset during Building invalidates the unfinished request. Search, Inspect, and Build Context are available only in Ready.

### 3. R1–R4 event risks

My proposed safeguards address four risks: **R1:** slow processing freezing the GUI; **R2:** publishing a corpus for an outdated Workspace; **R3:** displaying results after Reset or a newer request; and **R4:** destroying data while the worker still uses it. The exact R1–R4 labels need to be checked against the new milestone specification.

**Sequence A — Add during Build:** (1) Revision 4 starts building. (2) A document is added, advancing the revision to 5 and marking the corpus Dirty. (3) Build 4 finishes. (4) `MainWindow` rejects its completion because the revision is wrong. (5) Only a successful build for revision 5 can enable processing again.

**Sequence B — Reset during Search:** (1) Search begins with a request ID and revision. (2) Reset clears the Workspace and displays, then advances the revision. (3) The worker finishes the old search. (4) The GUI discards the result, leaving Reset's empty state unchanged.

Repeated requests also carry IDs so an older completion cannot overwrite a newer one. Exit stops new requests and waits for active work to finish safely.

### 4. Corpus ownership

The worker creates a temporary `ProcessingCore`, rebuilds it from the copied Workspace, then replaces its previously owned core on success. Only the worker calls its processing functions; the GUI receives copied results. Reset invalidates the revision immediately and queues cleanup. Shutdown destroys the worker's corpus before destroying its thread.

### 5. Rejected alternative

I considered keeping `ProcessingCore::rebuild()` inside `MainWindow`, like M3. It avoids thread coordination, but large builds would freeze the interface and prevent Reset from being handled promptly. A dedicated worker adds complexity, but keeps the GUI responsive and makes outdated results easier to reject.
