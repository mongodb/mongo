# makefile for our db project

#FLAGS=-ggdb -pthread -O0 -I .. -I../boost -I/opt/java/include/ -I/opt/java/include/linux -Isrc/p -I/src/p/db -I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/ $(INCS)
FLAGS= ${CFLAGS} -fPIC -ggdb -pthread -O0  -I .. -I/opt/java/include/ -I/opt/java/include/linux -Isrc/p -I/src/p/db -I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/ -I/sw/include

#BOOSTLIB=libboost_regex-gcc41-1_34_1.so.1.34.1
#BOOSTLIB=libboost_regex-gcc41-d-1_34_1.so.1.34.1
#BOOSTLIB=../lib/libboost_thread-gcc41-mt-d-1_34_1.a
BOOSTLIB = -L/sw/lib -lboost_thread

LIBS= /usr/local/lib/libpcrecpp.a /usr/local/lib/libpcre.a

OBJS=../stdafx.o ../util/sock.o ../grid/message.o ../util/mmap.o pdfile.o query.o jsobj.o introspect.o btree.o clientcursor.o ../util/util.o

# ../grid/protorecv.o ../grid/protosend.o

.cpp.o:
	g++ -c $(FLAGS) $< -o $@

# Our convention is that passing 'quicktest' on the command line means run 
# fast regressions and then immediately exit.  That way you know immediately if you
# broke something horribly.

db: $(OBJS) db.o $(LIBS)
	g++ $(FLAGS) -o $@ $(OBJS) db.o $(LIBS) $(BOOSTLIB)
#	-./db quicktest

clean:
	-rm -f $(OBJS) db.o
	-rm -f db

cleandb:
	rm /data/db/*

/usr/local/lib/libpcrecpp.a:
	cat pcre.txt

pcre:
	curl -O ftp://ftp.csx.cam.ac.uk/pub/software/programming/pcre/pcre-7.4.tar.gz
	tar -xzf pcre-7.4.tar.gz 
	cd pcre-7.4 && ./configure --enable-utf8 --with-match-limit=200000 --with-match-limit-recursion=4000 && make && make install

