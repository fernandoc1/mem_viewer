#ifndef MEM_VIEWER_GUI_H
#define MEM_VIEWER_GUI_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int mem_viewer_run_gui(pid_t target_pid, uintptr_t target_address, size_t size);
int mem_viewer_run_gui_shared(void *memory, size_t size);

#ifdef __cplusplus
}
#endif

#endif
