# Binary File Comparator

A Qt6-based side-by-side binary file comparison tool built on the mem_viewer infrastructure.

## Features

- **Side-by-side hex display**: Compare two binary files with 16-byte rows
- **Synchronized scrolling**: Scroll positions stay in sync between panes
- **Difference highlighting**: Differing bytes are highlighted in orange for quick identification
- **Difference navigation**: Jump to next/previous differences with keyboard shortcuts
- **File statistics**: View file sizes, byte counts, and percentage of differences
- **Error handling**: Graceful handling of missing files, permission errors, and size limits

## Building

The tool is built as part of the mem_viewer project:

```bash
cd mem_viewer
make binary_compare
```

The executable will be placed at `./binary_compare`

## Usage

### GUI Mode Without Files

```bash
./binary_compare
```

Then:
1. Click **File → Open Files...** to select two binary files
2. Files will be displayed side-by-side with differences highlighted in orange
3. Use navigation buttons or keyboard shortcuts to jump between differences

### Command-Line Mode (Auto-load Files)

```bash
./binary_compare file1.bin file2.bin
```

When two files are provided as command-line arguments:
1. Files are automatically loaded on startup
2. Comparison is displayed immediately
3. Ready to navigate differences right away

### Examples

```bash
# Compare ROM files
./binary_compare original.rom modified.rom

# Compare object files
./binary_compare main.o main_debug.o

# Compare firmware versions
./binary_compare firmware_v1.0.bin firmware_v1.1.bin

# Compare with relative paths
./binary_compare ../backup/file.bin ./current/file.bin

# Compare with absolute paths
./binary_compare /mnt/backup/data.bin /home/user/data.bin
```

### Keyboard Shortcuts

- **Ctrl+O**: Open files for comparison
- **Ctrl+N**: Jump to next difference
- **Ctrl+P**: Jump to previous difference  
- **Ctrl+Q**: Quit application

### File Size Limits

- Maximum file size per file: **100 MB**
- Files larger than this will be rejected with an error message

## Display Format

Each pane shows memory in a classic hex viewer format:

```
00000000  00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f  | ................
00000010  10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f  | ................
```

- **Left column**: Byte offset (hex address)
- **Center columns**: Hexadecimal byte values (3 chars per byte)
- **Right column**: ASCII representation (. for non-printable)
- **Highlighted bytes**: Differing bytes shown with orange background

## Comparison Algorithm

The tool compares files byte-by-byte and records all differences:

- If one file is shorter than the other, the remaining bytes are considered "different"
- Difference navigation uses binary search for efficient position tracking
- Statistics show the count and percentage of different bytes

## Statistics Panel

The bottom dock displays:
- File 1 size (bytes)
- File 2 size (bytes)
- Total different bytes and percentage

Example:
```
File 1: 1024 bytes
File 2: 1024 bytes
Different bytes: 13 (1.3%)
```

## Implementation Details

### Architecture

1. **DualFileBuffer** (`file_comparator.h/cpp`)
   - Loads both files into memory
   - Performs byte-by-byte difference analysis
   - Provides efficient difference navigation

2. **HexDisplayWidget** (`comparison_widget.cpp`)
   - Custom Qt widget for rendering hex display
   - Handles highlighting, scrolling, and layout
   - Implemented independently without relying on mem_viewer's MemViewerWidget

3. **ComparisonWidget** (`comparison_widget.h/cpp`)
   - Combines two HexDisplayWidget instances side-by-side
   - Synchronizes scrolling between panes
   - Manages navigation between differences

4. **ComparisonWindow** (`comparison_window.h/cpp`)
   - Main application window (QMainWindow)
   - File dialogs and menu bar
   - Status bar and dock widgets for statistics
   - Keyboard shortcut handling

### Build System

The Makefile handles:
- Qt6 meta-object compilation (moc)
- Proper include paths and library linking
- Clean separation of concerns via object files

## Error Handling

The tool handles:
- **File not found**: Shows error dialog with file path
- **Permission denied**: Shows permission error
- **File too large**: Rejects files larger than 100MB
- **Empty files**: Accepts and handles zero-byte files correctly
- **Memory allocation failures**: Graceful error reporting

## Performance

- Loading: Typically <100ms for 1MB files
- Difference analysis: Linear scan, O(n) complexity
- Memory usage: 2x the combined file size + overhead
- Navigation: O(log n) using binary search on difference positions

## Testing

Test files with known differences can be created:

```bash
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)) * 4)" > test1.bin
python3 << 'EOF' > test2.bin
import sys
data = bytearray(bytes(range(256)) * 4)
data[10] = 0xFF
data[50] = 0x00
sys.stdout.buffer.write(bytes(data))
EOF
./binary_compare
# Then open both test files
```

## Limitations

- Maximum file size: 100 MB per file
- No support for files that are actively being modified
- Entire files must fit in RAM
- No differential/delta compression

## Future Enhancements

- Configurable file size limits
- Export comparison results to report
- Pattern search across both files
- Byte editing capabilities
- Undo/redo for edits
- Session saving/loading
