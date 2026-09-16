#pragma once
// Sets a hardware write-watchpoint on `address` across all current threads,
// and logs the instruction pointer + owning module whenever something writes
// to it. Used to find what's reverting our vtable patches.
void InstallWriteWatchpointAllThreads(void* address);
