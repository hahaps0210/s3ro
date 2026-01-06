CC ?= gcc
CFLAGS ?= -O2 -g -fPIC -Wall -Wextra -std=gnu99
LDFLAGS ?= -shared
LIBS ?= -lcurl -ldl -lpthread
ifeq ($(shell uname -s),Linux)
LIBS += -lrt
endif
TARGET ?= libremotefs_shim.so

SRCS = remotefs_shim.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c jsmn.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
