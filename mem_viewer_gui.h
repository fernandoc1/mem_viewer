#ifndef MEM_VIEWER_GUI_H
#define MEM_VIEWER_GUI_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

int mem_viewer_run_gui(pid_t target_pid, uintptr_t target_address, size_t size);

#endif
