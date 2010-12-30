CC = gcc -W -Wall -I../../include # -Wno-unused -x c++
OBJS = ex_access.o ex_call_center.o ex_config.o ex_hello.o ex_sequence.o ex_stat.o ex_schema.o ex_thread.o

all: $(OBJS)

clean:
	rm -f $(OBJS)

%.o:	%.c ../../include/wiredtiger.h
	$(CC) -o $@ -c $<
