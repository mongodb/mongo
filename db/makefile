# makefile for our db project

FLAGS=-I .. -I/opt/java/include/ -I/opt/java/include/linux -Isrc/p -I/src/p/db

OBJS=../stdafx.o ../util/sock.o ../grid/message.o ../util/mmap.o pdfile.o query.o jsobj.o

.cpp.o:
	g++ -c $(FLAGS) $< -o $@

# Our convention is that passing 'quicktest' on the command line means run 
# fast regressions and then immediately exit.  That way you know immediately if you
# broke something horribly.

db: $(OBJS) db.o
	g++ $(FLAGS) -o $@ $(OBJS) db.o
	./db quicktest

clean:
	-rm -f $(OBJS) db.o
	-rm -f db
