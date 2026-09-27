CXX      ?= g++
CC       ?= gcc
PYTHON   ?= python3

CXXFLAGS ?= -O3 -std=c++14 -Wall -Wextra -I./SDFLib
CFLAGS   ?= -O3 -Wall -Wextra -I./SDFLib
LDFLAGS  ?= -lm

TARGET    = TerminalDemo/view_sdf
CONFIG    = SDFLib/sdf_config.ini
GENERATOR = SDFLib/ttf2sdf.py

FONT_H    = SDFLib/sdf_font.h
FONT_C    = SDFLib/sdf_font.c

CPP_SRCS  = TerminalDemo/view_sdf.cpp SDFLib/TextLayout.cpp SDFLib/SdfRenderer.cpp
C_SRCS    = $(FONT_C)
OBJS      = $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

all: $(TARGET)

# Target: build binary
$(TARGET): $(FONT_H) $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

# Compile C++ sources
%.o: %.cpp $(FONT_H) SDFLib/Matrix2D.hpp SDFLib/TextLayout.hpp SDFLib/SdfRenderer.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile generated C font data
%.o: %.c $(FONT_H)
	$(CC) $(CFLAGS) -c $< -o $@

# Automatic TTF -> SDF font regeneration when config or generator changes
$(FONT_H) $(FONT_C): $(CONFIG) $(GENERATOR)
	@echo "[TTF2SDF] Regenerating font from $(CONFIG)..."
	cd SDFLib && $(PYTHON) $(notdir $(GENERATOR)) $(notdir $(CONFIG))

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -f $(FONT_H) $(FONT_C)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean distclean run
