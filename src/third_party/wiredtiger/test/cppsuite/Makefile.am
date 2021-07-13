AM_CPPFLAGS = -std='c++11' -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility -I$(top_srcdir)/test/cppsuite
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

# Copy across the default configuration files with the artifacts.
all:
	rsync -rup $(top_srcdir)/test/cppsuite/configs $(top_builddir)/test/cppsuite

all_TESTS=
noinst_PROGRAMS=

run_SOURCES = test_harness/core/component.cxx \
              test_harness/core/configuration.cxx \
              test_harness/core/throttle.cxx \
              test_harness/util/logger.cxx \
              test_harness/util/scoped_types.cxx \
              test_harness/workload/database_model.cxx \
              test_harness/workload/database_operation.cxx \
              test_harness/workload/random_generator.cxx \
              test_harness/workload/thread_context.cxx \
              test_harness/workload/workload_tracking.cxx \
              test_harness/workload/workload_validation.cxx \
              test_harness/checkpoint_manager.cxx \
              test_harness/connection_manager.cxx \
              test_harness/runtime_monitor.cxx \
              test_harness/test.cxx \
              test_harness/thread_manager.cxx \
              test_harness/timestamp_manager.cxx \
              test_harness/workload_generator.cxx \
              tests/run.cxx

noinst_PROGRAMS += run
all_TESTS += run

# Run this during a "make check" smoke test.
TESTS = $(all_TESTS)

clean-local:
	rm -rf WT_TEST.* core.* *.core
