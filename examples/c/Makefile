CC = gcc -W -Wall -Wno-unused -I../../include # -x c++
OBJS = ex_access.o ex_config.o ex_hello.o ex_sequence.o ex_stat.o \
       ex_column_app.o ex_column_schema.o \
       ex_columnmod_app.o ex_columnmod_schema.o \

all: $(OBJS)

clean:
	rm -f $(OBJS)

%.o:	%.c ../../include/wt/wtds.h
	$(CC) -o $@ -c $<

ex_columnmod_app.o:	ex_column_app.c
	$(CC) -DLOADABLE_MODULE -o $@ -c $<

ex_columnmod_schema.o:	ex_column_schema.c
	$(CC) -DLOADABLE_MODULE -o $@ -c $<
