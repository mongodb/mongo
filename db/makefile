# makefile for our db project

FLAGS= ${CFLAGS} -fPIC -ggdb -pthread -O3  -I .. -Isrc/p -I/src/p/db -L/usr/local/lib -L/usr/lib -rdynamic
#FLAGS= ${CFLAGS} -fPIC -ggdb -pthread -O0  -I .. -Isrc/p -I/src/p/db -L/usr/local/lib -L/usr/lib -rdynamic

LIB_DEPS = -lpcrecpp -lpcre
LIB_BOOST = -lboost_thread -lboost_filesystem

LIBS= $(LIB_DEPS) $(LIB_BOOST) -lstdc++
DBGRID_LIBS = $(LIB_BOOST) -lstdc++
DBTEST_LIBS= $(LIBS) -lunittest

JVM_LIBS = -L/opt/java/lib/

OBJS=../stdafx.o ../util/sock.o ../grid/message.o ../util/mmap.o pdfile.o query.o jsobj.o introspect.o btree.o clientcursor.o ../util/util.o javajs.o tests.o json.o repl.o ../client/dbclient.o btreecursor.o cloner.o namespace.o commands.o matcher.o dbcommands.o dbeval.o ../util/background.o ../util/miniwebserver.o dbwebserver.o dbinfo.o instance.o

DBGRID_OBJS=../stdafx.o json.o ../util/sock.o ../grid/message.o ../util/util.o jsobj.o ../client/dbclient.o ../dbgrid/dbgrid.o ../dbgrid/request.o ../client/connpool.o ../dbgrid/gridconfig.o commands.o ../dbgrid/dbgrid_commands.o ../dbgrid/griddatabase.o ../client/model.o ../util/background.o ../dbgrid/shard.o

DBTEST_OBJS= $(OBJS) ../dbtests/dbtests.o ../dbtests/btreetests.o

GPP = g++

#
#  get platform-specific flags and overrides
#
include makefile.$(shell uname -s).$(shell uname -m)

#
#  get any local overrides from makefile.local  Sorry about the double-negative
#
-include makefile.local

#
#  now, get the version of GPP.  Requirement is 4.2
#
_GPP_VER := $(shell $(GPP) -dumpversion 2>&1 )
_REQ_GPP_VER = 4.2

.cpp.o:
	$(GPP) -c $(FLAGS) $< -o $@

# Our convention is that passing 'quicktest' on the command line means run 
# fast regressions and then immediately exit.  That way you know immediately if you
# broke something horribly.

db: $(OBJS) db.o  
	$(GPP) $(FLAGS) -o $@ $(OBJS) db.o $(LIBS) $(JVM_LIBS)

dbtests: $(DBTEST_OBJS)
	$(GPP) $(FLAGS) -o $@ $(DBTEST_OBJS) $(DBTEST_LIBS) $(JVM_LIBS)

dbgrid: $(DBGRID_OBJS) ../dbgrid/dbgrid.o
	$(GPP) $(FLAGS) -o $@ $(DBGRID_OBJS) $(DBGRID_LIBS)
    
javatest: javatest.o $(OBJS)
	$(GPP) $(FLAGS) -o $@ javatest.o $(OBJS) $(LIBS) $(JVM_LIBS)

javatest2: javatest.o javajs.o
	$(GPP) $(FLAGS) -o javatest2 javatest.o javajs.o $(LIBS) $(JVM_LIBS)

# FIXME Currently dbgrid, javatests{,2} binaries not cleaned.
clean:
	-rm -f $(OBJS) db.o $(DBTEST_OBJS)
	-rm -f db dbtests
	-rm -f *.o

cleandb:
	rm /data/db/*

/usr/local/lib/libpcrecpp.a:
	cat pcre.txt

pcre:
	curl -O ftp://ftp.csx.cam.ac.uk/pub/software/programming/pcre/pcre-7.4.tar.gz
	tar -xzf pcre-7.4.tar.gz 
	cd pcre-7.4 && ./configure --enable-utf8 --with-match-limit=200000 --with-match-limit-recursion=4000 && make && make install

info:
	@echo ''
	@echo 'GPP command :' $(GPP)
	@echo 'Using version of GPP :' $(_GPP_VER)
	@echo 'Req version of GPP :' $(_REQ_GPP_VER)
	@# todo - determine if we have the right version of gcc and error if not
	@echo ''
