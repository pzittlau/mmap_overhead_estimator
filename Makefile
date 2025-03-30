# Compiler and Flags
CC = gcc
# Use gnu11 standard for _GNU_SOURCE features (madvise flags, MAP_HUGE_*)
# Add -g for debugging symbols, -O2 for optimization
CFLAGS = -Wall -Wextra -std=gnu11 -g -O2
# Linker flags (add -lm if math functions like pow were used)
LDFLAGS = -lm

# Source and Target
SRCS = mmap_overhead_estimator.c
TARGET = mmap_overhead

# Default target: build the executable
all: $(TARGET)

# Rule to build the target executable from sources
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Phony target to clean up build artifacts
clean:
	rm -f $(TARGET)

# Declare phony targets
.PHONY: all clean

