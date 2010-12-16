CC = gcc -W -Wall -I../../include # -Wno-unused -x c++
OBJS = ex_access.o ex_call_center.o ex_config.o ex_hello.o ex_sequence.o ex_stat.o ex_schema.o

all: $(OBJS)

clean:
	rm -f $(OBJS)

%.o:	%.c ../../include/wt/wtds.h
	$(CC) -o $@ -c $<
