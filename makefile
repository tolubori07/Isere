# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

# Project name and files
TARGET = hug
SRCS = main.cpp

# Automatically generate object file names from source files
OBJS = $(SRCS:.cpp=.o)

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile source files into object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Phony targets to avoid filename conflicts
.PHONY: all clean

