# makefile for our db project

FLAGS=-ggdb -O0 -I .. -I/opt/java/include/ -I/opt/java/include/linux -Isrc/p -I/src/p/db -I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/

#BOOSTLIB=libboost_regex-gcc41-1_34_1.so.1.34.1
#BOOSTLIB=libboost_regex-gcc41-d-1_34_1.so.1.34.1

LIBS= /usr/local/lib/libpcrecpp.a /usr/local/lib/libpcre.a

OBJS=../stdafx.o ../util/sock.o ../grid/message.o ../util/mmap.o pdfile.o query.o jsobj.o

.cpp.o:
	g++ -c $(FLAGS) $< -o $@

# Our convention is that passing 'quicktest' on the command line means run 
# fast regressions and then immediately exit.  That way you know immediately if you
# broke something horribly.

db: $(OBJS) db.o $(LIBS)
	g++ $(FLAGS) -o $@ $(OBJS) db.o $(LIBS)
	-./db quicktest

clean:
	-rm -f $(OBJS) db.o
	-rm -f db

cleandb:
	rm /data/db/*

/usr/local/lib/libpcrecpp.a:
	cat pcre.txt
