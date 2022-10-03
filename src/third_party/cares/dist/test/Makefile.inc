TESTSOURCES = ares-test-main.cc	\
  ares-test-init.cc			\
  ares-test.cc				\
  ares-test-ns.cc			\
  ares-test-parse.cc			\
  ares-test-parse-a.cc			\
  ares-test-parse-aaaa.cc		\
  ares-test-parse-caa.cc		\
  ares-test-parse-mx.cc		\
  ares-test-parse-naptr.cc		\
  ares-test-parse-ns.cc		\
  ares-test-parse-ptr.cc		\
  ares-test-parse-soa.cc		\
  ares-test-parse-soa-any.cc		\
  ares-test-parse-srv.cc		\
  ares-test-parse-txt.cc		\
  ares-test-misc.cc			\
  ares-test-live.cc			\
  ares-test-mock.cc			\
  ares-test-mock-ai.cc			\
  ares-test-internal.cc		\
  dns-proto.cc				\
  dns-proto-test.cc

TESTHEADERS = ares-test.h		\
  dns-proto.h                           \
  ares-test-ai.h

FUZZSOURCES = ares-test-fuzz.c		\
  ares-fuzz.c

FUZZNAMESOURCES = ares-test-fuzz-name.c	\
  ares-fuzz.c

DUMPSOURCES = dns-proto.cc		\
  dns-dump.cc
