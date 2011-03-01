
all: FORCE
	scons util/ipv6_parser.h
	scons -j `cat /proc/cpuinfo | grep processor | wc -l` all

debug: FORCE
	scons --d -j `cat /proc/cpuinfo | grep processor | wc -l` all

install: FORCE # as root
	scons --full --sharedclient --prefix=/usr/local install
	echo "/usr/local/lib64" > /etc/ld.so.conf.d/mongodb.conf
	/sbin/ldconfig

build_deps: FORCE
	yum -y install git tcsh scons gcc-c++ glibc-devel
	yum -y install boost-devel pcre-devel js-devel readline-devel
	yum -y install boost-devel-static readline-static ncurses-static
	yum -y install js js-devel

MONGO_DB_PATH=/data/db
MONGOD_LOGFILE=/var/log/mongod

initdb: FORCE # as root
	mkdir -p $(MONGO_DB_PATH)/
	chown `id -u` $(MONGO_DB_PATH)

test: FORCE
	rm -f test
	scons --d test

test_js_ip_addr: FORCE # as root
	python buildscripts/smoke.py jstests/ipaddr.js

run_server: FORCE # as root
	/usr/local/bin/mongod --fork --dbpath=$(MONGO_DB_PATH) --logpath=$(MONGOD_LOGFILE)

run_shell: FORCE
	/usr/local/bin/mongo


#js-1.7.0.tar.gz:
#	curl -O ftp://ftp.mozilla.org/pub/mozilla.org/js/js-1.7.0.tar.gz
#	tar zxvf js-1.7.0.tar.gz

#js.lib: js-1.7.0.tar.gz
#	cd js/src; \
#	export CFLAGS="-DJS_C_STRINGS_ARE_UTF8"; \
#	make -f Makefile.ref; \
#	JS_DIST=/usr make -f Makefile.ref export

clean: FORCE
	scons --clean
#	rm -rf js-1.7.0.tar.gz js *.so *.a
	rm -rf *.so *.a

# This is a target used to 'force' rebuild of targets that depend on it
.PHONY: FORCE

