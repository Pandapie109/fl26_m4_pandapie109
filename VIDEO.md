# M5 walkthrough video

Paste the shareable link to your own Panopto video here before submitting:

https://virginiatech.hosted.panopto.com/Panopto/Pages/Viewer.aspx?id=c7e0449e-2297-4cf7-bec6-b50900466806

Record one video (3 minutes maximum) showing the running program and the code:
1. How `AsyncCorpusBuild::work_loop()` waits for work and how `stop()` wakes and joins it.
2. Reset during a long build (`--build-delay-ms 500`), then rebuild with `-DAIWS_FAULT_NO_CANCEL` and repeat.
3. One State pattern transition, such as `BuildingState` to `DirtyState` after Add Document.
