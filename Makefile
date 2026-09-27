CXX      ?= g++
CC       ?= gcc
PYTHON   ?= python3

CXXFLAGS ?= -O3 -std=c++14 -Wall -Wextra
CFLAGS   ?= -O3 -Wall -Wextra
LDFLAGS  ?= -lm

TARGET    = view_sdf
CONFIG    = sdf_config.ini
GENERATOR = ttf2sdf.py

FONT_H    = sdf_font.h
FONT_C    = sdf_font.c

CPP_SRCS  = view_sdf.cpp TextLayout.cpp SdfRenderer.cpp
C_SRCS    = $(FONT_C)
OBJS      = $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

all: $(TARGET)

# Target: build binary
$(TARGET): $(FONT_H) $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

# Compile C++ sources
%.o: %.cpp $(FONT_H) Matrix2D.hpp TextLayout.hpp SdfRenderer.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile generated C font data
%.o: %.c $(FONT_H)
	$(CC) $(CFLAGS) -c $< -o $@

# Automatic TTF -> SDF font regeneration when config or generator changes
$(FONT_H) $(FONT_C): $(CONFIG) $(GENERATOR)
	@echo "[TTF2SDF] Regenerating font from $(CONFIG)..."
	$(PYTHON) $(GENERATOR) $(CONFIG)

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -f $(FONT_H) $(FONT_C)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean distclean run
