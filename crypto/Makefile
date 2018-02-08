include ../config.mk

all: $(patsubst %.c,%.o,$(wildcard *.c))

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o
