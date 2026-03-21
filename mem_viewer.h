#ifndef MEM_VIEWER_H
#define MEM_VIEWER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MemViewer MemViewer;

MemViewer *mem_viewer_open(void *memory, size_t size);
MemViewer *mem_viewer_open_shared(void *memory, size_t size);
void mem_viewer_destroy(MemViewer *viewer);
int mem_viewer_is_open(MemViewer *viewer);

#ifdef __cplusplus
}
#endif

#endif
