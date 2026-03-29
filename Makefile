PKG_CONFIG ?= pkg-config
MOC := /usr/lib/qt6/libexec/moc

CXX := g++
CC := gcc

CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -fPIC
CFLAGS := -std=c11 -Wall -Wextra -O2
LDFLAGS :=
DL_LIBS := -ldl

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2)
QT_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Core Qt6Widgets)
QT_LIBS := $(shell $(PKG_CONFIG) --libs Qt6Core Qt6Widgets)

LIB_SOURCES := mem_viewer.cpp
LIB_OBJECTS := $(LIB_SOURCES:.cpp=.o)
GUI_SOURCES := mem_viewer_gui.cpp
GUI_OBJECTS := $(GUI_SOURCES:.cpp=.o)

COMPARE_MOC := moc_comparison_window.cpp moc_comparison_widget.cpp
COMPARE_MOC_OBJECTS := $(COMPARE_MOC:.cpp=.o)

all: libmemviewer.so mem_viewer_helper test_c test_sdl test_file test_shared binary_compare

libmemviewer.so: $(LIB_OBJECTS) $(GUI_OBJECTS)
	$(CXX) -shared -o $@ $(LIB_OBJECTS) $(GUI_OBJECTS) $(QT_LIBS) $(DL_LIBS)

mem_viewer_helper: mem_viewer_helper.o $(GUI_OBJECTS)
	$(CXX) -o $@ mem_viewer_helper.o $(GUI_OBJECTS) $(QT_LIBS)

mem_viewer.o: mem_viewer.cpp mem_viewer.h mem_viewer_gui.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

mem_viewer_gui.o: mem_viewer_gui.cpp mem_viewer_gui.h
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

mem_viewer_helper.o: mem_viewer_helper.cpp mem_viewer_gui.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_c.o: test_c.c mem_viewer.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_sdl.o: test_sdl.cpp mem_viewer.h
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

test_file.o: test_file.cpp mem_viewer.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test_c: test_c.o libmemviewer.so
	$(CC) -o $@ test_c.o -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

test_sdl: test_sdl.o libmemviewer.so
	$(CXX) -o $@ test_sdl.o -L. -lmemviewer -Wl,-rpath,'$$ORIGIN' $(SDL_LIBS)

test_file: test_file.o libmemviewer.so
	$(CXX) -o $@ test_file.o -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

test_shared.o: test_shared.c mem_viewer.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_shared: test_shared.o libmemviewer.so
	$(CC) -o $@ test_shared.o -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

file_comparator.o: file_comparator.cpp file_comparator.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

comparison_widget.o: comparison_widget.cpp comparison_widget.h
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

comparison_window.o: comparison_window.cpp comparison_window.h
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

binary_compare_main.o: binary_compare_main.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

binary_compare: file_comparator.o comparison_widget.o comparison_window.o binary_compare_main.o $(GUI_OBJECTS) $(COMPARE_MOC_OBJECTS)
	$(CXX) -o $@ file_comparator.o comparison_widget.o comparison_window.o binary_compare_main.o $(GUI_OBJECTS) $(COMPARE_MOC_OBJECTS) $(QT_LIBS)

moc_comparison_window.o: moc_comparison_window.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

moc_comparison_widget.o: moc_comparison_widget.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

moc_comparison_window.cpp: comparison_window.h
	$(MOC) $(QT_CFLAGS) -o $@ $<

moc_comparison_widget.cpp: comparison_widget.h
	$(MOC) $(QT_CFLAGS) -o $@ $<

clean:
	rm -f *.o *.so mem_viewer_helper test_c test_sdl test_file test_shared binary_compare moc_*.cpp

.PHONY: all clean
