CC = gcc -W -Wall -I../../include # -Wno-unused -x c++
OBJS = \
	ex_access.o ex_call_center.o ex_config.o ex_cursor.o ex_extending.o \
	ex_hello.o ex_pack.o ex_process.o ex_schema.o ex_sequence.o ex_stat.o \
	ex_thread.o ex_tpcb.o ex_transaction.o

all: $(OBJS)

clean:
	rm -f $(OBJS)

%.o:	%.c ../../include/wiredtiger.h
	$(CC) -o $@ -c $<
