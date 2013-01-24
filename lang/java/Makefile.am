AM_CPPFLAGS = -I$(abs_top_builddir)

JAVASRC = $(top_srcdir)/lang/java
JAVADEST = src/com/wiredtiger/db
JAVADESTFULL = $(JAVASRC)/$(JAVADEST)
BUILT_SOURCES = $(JAVASRC)/wiredtiger_wrap.c
SWIG_SOURCES = $(JAVASRC)/wiredtiger.i

JDOCDIR = $(top_srcdir)/docs/java
java_DATA = $(JDOCDIR)/index.html

javadir = $(datadir)/java
dist_java_JAVA = \
		$(JAVADESTFULL)/Connection.java \
		$(JAVADESTFULL)/Cursor.java \
		$(JAVADESTFULL)/DbException.java \
		$(JAVADESTFULL)/SearchStatus.java \
		$(JAVADESTFULL)/Session.java \
		$(JAVADESTFULL)/wiredtiger.java \
		$(JAVADESTFULL)/wiredtigerConstants.java \
		$(JAVADESTFULL)/wiredtigerJNI.java
java_LTLIBRARIES = libwiredtiger_java.la

CPPFLAGS += $(JNI_CPPFLAGS)
# Some warnings when compiling the generated code are unavoidable
CFLAGS += -w
libwiredtiger_java_la_SOURCES = $(BUILT_SOURCES) $(SWIG_SOURCES)
#libwiredtiger_java_la_LDFLAGS = -module
libwiredtiger_java_la_LIBADD = $(abs_top_builddir)/libwiredtiger.la

$(JAVASRC)/wiredtiger_wrap.c: $(top_srcdir)/src/include/wiredtiger.in $(SWIG_SOURCES)
	(cd $(JAVASRC) && \
	    $(SWIG) -Wall -v -java -nodefaultctor -nodefaultdtor -package com.wiredtiger.db -I$(abs_top_builddir) -outdir $(JAVADEST) -o wiredtiger_wrap.c wiredtiger.i)

$(JDOCDIR)/index.html:	$(dist_java_JAVA)
	mkdir -p $(JDOCDIR)
	javadoc -public -d $(JDOCDIR) -link http://docs.oracle.com/javase/6/docs/api $(JAVADESTFULL)/wiredtiger.java $(JAVADESTFULL)/wiredtigerConstants.java $(JAVADESTFULL)/[A-Z]*.java
