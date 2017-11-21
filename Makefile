TARGET=nshost.so.9.4
build=automatic

SRCS=$(wildcard *.c) $(wildcard ../libnsp/com/*.c)
OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ../libnsp/icom -fPIC -Wall
LDFLAGS=-shared

ifeq ($(build),debug)
	CFLAGS+=-g
else
	CFLAGS+=-O2
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	$(RM) $(OBJS) $(TARGET)
debug:
	$(CFLAGS)+=-g

.PHONY:clean all
