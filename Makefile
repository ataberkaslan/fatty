# Compiler & Flags
CC       := gcc
CFLAGS   := -Wall -Wextra -O2 $(shell pkg-config --cflags sdl2 SDL2_ttf)
LDFLAGS  := $(shell pkg-config --libs sdl2 SDL2_ttf) -lutil

# Target and Sources
TARGET   := f4tty 
SRCS     := f4tty.c
OBJS     := $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
