CC = gcc -W -Wall -Wno-unused -I../../include # -x c++
OBJS = ex_access.o ex_config.o ex_hello.o ex_sequence.o ex_stat.o ex_schema.o

all: $(OBJS)

clean:
	rm -f $(OBJS)

%.o:	%.c ../../include/wt/wtds.h
	$(CC) -o $@ -c $<
