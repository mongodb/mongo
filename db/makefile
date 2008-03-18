# makefile for our db project

FLAGS= ${CFLAGS} -fPIC -ggdb -pthread -O0  -I .. -Isrc/p -I/src/p/db

LIB_DEPS = /usr/local/lib/libpcrecpp.a /usr/local/lib/libpcre.a
LIBS= $(LIB_DEPS) -lboost_thread -lboost_filesystem

JVM_LIBS = -L/opt/java/lib/

OBJS=../stdafx.o ../util/sock.o ../grid/message.o ../util/mmap.o pdfile.o query.o jsobj.o introspect.o btree.o clientcursor.o ../util/util.o javajs.o

# ../grid/protorecv.o ../grid/protosend.o

include makefile.$(shell uname -s)

.cpp.o:
	g++ -c $(FLAGS) $< -o $@

# Our convention is that passing 'quicktest' on the command line means run 
# fast regressions and then immediately exit.  That way you know immediately if you
# broke something horribly.

db: $(OBJS) db.o $(LIB_DEPS)
	g++ $(FLAGS) -o $@ $(OBJS) db.o $(LIBS) $(JVM_LIBS)
#	-./db quicktest

jtest: $(OBJS) javajs.o $(LIB_DEPS)
	g++ $(FLAGS) -o $@ $(OBJS) javajs.o $(LIBS) $(JVM_LIBS)

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

