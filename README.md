# mem_viewer

`mem_viewer` is a small C/C++ memory inspection library with a Qt6 GUI for viewing and editing a live memory region. It also includes a binary comparison tool and a few local test/demo programs.

## What It Includes

- `libmemviewer.so`: shared library exposing the C API in [`mem_viewer.h`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/mem_viewer.h)
- `mem_viewer_helper`: Qt helper process used by `mem_viewer_open()`
- `binary_compare`: side-by-side binary file comparison GUI
- `test_c`, `test_sdl`, `test_file`, `test_shared`: local test/demo executables

## Features

### Memory Viewer

- Live hex + ASCII memory display
- Byte selection and direct byte editing
- Auto-refresh and manual refresh
- Search by hex or decimal value
- Search navigation between matches
- Go-to memory position by byte offset
- Notes/annotations stored in JSON
- Per-note highlight colors persisted to disk
- Multiple note JSON files loaded at once
- Tabbed side pane organized by feature

### Binary Compare

- Side-by-side binary comparison
- Difference highlighting and navigation
- File statistics and synchronized scrolling

See [`BINARY_COMPARE.md`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/BINARY_COMPARE.md) for binary comparator details.

## Requirements

- `g++` with C++20 support
- `gcc`
- `make`
- Qt6 Core + Widgets development packages
- `pkg-config`
- SDL2 development packages if you want to build `test_sdl`

## Build

```bash
make
```

This builds all targets:

- `libmemviewer.so`
- `mem_viewer_helper`
- `test_c`
- `test_sdl`
- `test_file`
- `test_shared`
- `binary_compare`

To clean:

```bash
make clean
```

## C API

Public API:

```c
typedef struct MemViewer MemViewer;

MemViewer *mem_viewer_open(void *memory, size_t size);
MemViewer *mem_viewer_open_shared(void *memory, size_t size);
void mem_viewer_destroy(MemViewer *viewer);
int mem_viewer_is_open(MemViewer *viewer);

void *mem_viewer_shared_malloc(size_t size);
void mem_viewer_shared_free(void *ptr, size_t size);
```

### `mem_viewer_open`

Launches the viewer for a memory region in the current process by spawning `mem_viewer_helper`, which then opens the Qt GUI against the parent process memory.

### `mem_viewer_open_shared`

Forks and launches the Qt GUI directly for a shared memory region.

### `mem_viewer_shared_malloc`

Allocates a shared mapping suitable for the shared-viewer path.

## Basic Example

```c
#include "mem_viewer.h"
#include <stdint.h>
#include <stdlib.h>

int main(void) {
    size_t size = 65536;
    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer) {
        return 1;
    }

    for (size_t i = 0; i < size; ++i) {
        buffer[i] = (uint8_t)i;
    }

    MemViewer *viewer = mem_viewer_open(buffer, size);
    if (!viewer) {
        free(buffer);
        return 1;
    }

    while (mem_viewer_is_open(viewer)) {
        /* Application work here */
    }

    mem_viewer_destroy(viewer);
    free(buffer);
    return 0;
}
```

## Running The Demos

Start the basic C demo:

```bash
./test_c
```

Run the shared-memory demo:

```bash
./test_shared
```

Open a static file in the viewer:

```bash
./test_file path/to/file.bin [notes1.json notes2.json ...]
```

`test_file` loads the file into shared memory and opens the viewer in shared-memory mode, which avoids the live-process refresh overhead and is faster for large static files. Any additional arguments are passed as note files through `MEM_VIEWER_NOTES`.

Open the binary comparator:

```bash
./binary_compare
```

Or auto-load two files:

```bash
./binary_compare file1.bin file2.bin
```

## Notes File Format

Annotations are stored as JSON. Each annotation entry contains:

- `positions`: array of byte offsets, either as decimal JSON numbers or strings such as `"0x10"`
- `note`: free-form text
- `color`: highlight color as a hex string such as `#72e67a`

When saving, `mem_viewer` writes positions as hex strings.

Example:

```json
{
  "annotations": [
    {
      "positions": ["0x10", "0x11", "0x12"],
      "note": "Header bytes",
      "color": "#72e67a"
    }
  ]
}
```

Older annotation files without `color` still load; they default to the original green highlight.

## Environment Variables

### `MEM_VIEWER_NOTES`

Automatically loads note files when the memory viewer starts. Use `:` as the file separator.

Example:

```bash
MEM_VIEWER_NOTES="notes/main.json:notes/bosses.json:notes/debug.json" ./test_c
```

Each loaded file appears as its own tab in the Notes view. The active tab is the working note file used for edits and saves.

## Debugging

Set `MEM_VIEWER_DEBUG=1` to enable debug logging in the library, helper, and GUI:

```bash
MEM_VIEWER_DEBUG=1 ./test_c
```

## Project Files

- [`mem_viewer.cpp`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/mem_viewer.cpp): library entry points and process launching
- [`mem_viewer_gui.cpp`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/mem_viewer_gui.cpp): Qt memory viewer implementation
- [`mem_viewer_helper.cpp`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/mem_viewer_helper.cpp): helper executable entry point
- [`binary_compare_main.cpp`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/binary_compare_main.cpp): binary compare application entry
- [`Makefile`](/home/fernando/Code/hl_ff6/external/LakeSnes/mem_viewer/Makefile): build rules
