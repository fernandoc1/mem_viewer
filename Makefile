PKG_CONFIG ?= pkg-config

CXX := g++
CC := gcc

CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -fPIC
CFLAGS := -std=c11 -Wall -Wextra -O2
LDFLAGS :=
DL_LIBS := -ldl

GTKMM_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtkmm-4.0)
GTKMM_LIBS := $(shell $(PKG_CONFIG) --libs gtkmm-4.0)
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2)

LIB_SOURCES := mem_viewer.cpp
LIB_OBJECTS := $(LIB_SOURCES:.cpp=.o)
GUI_SOURCES := mem_viewer_gui.cpp
GUI_OBJECTS := $(GUI_SOURCES:.cpp=.o)

all: libmemviewer.so mem_viewer_helper test_c test_sdl test_file

libmemviewer.so: $(LIB_OBJECTS)
	$(CXX) -shared -o $@ $(LIB_OBJECTS) $(DL_LIBS)

mem_viewer_helper: mem_viewer_helper.o $(GUI_OBJECTS)
	$(CXX) -o $@ mem_viewer_helper.o $(GUI_OBJECTS) $(GTKMM_LIBS)

mem_viewer.o: mem_viewer.cpp mem_viewer.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

mem_viewer_gui.o: mem_viewer_gui.cpp mem_viewer_gui.h
	$(CXX) $(CXXFLAGS) $(GTKMM_CFLAGS) -c -o $@ $<

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

clean:
	rm -f *.o *.so mem_viewer_helper test_c test_sdl test_file

.PHONY: all clean
