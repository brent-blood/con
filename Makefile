# Compiler and Flags
CC = gcc
CFLAGS = -Wall -Wextra -Iinclude
LDFLAGS = -lresolv

# Target Executable Name
TARGET = con

# Source and Object Files
SRC = src/con.c
OBJ = $(SRC:.c=.o)

# Installation variables
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

# Default target
all: $(TARGET)

# Link the target executable
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean install uninstall

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
