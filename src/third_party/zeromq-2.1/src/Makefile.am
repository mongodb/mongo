lib_LTLIBRARIES = libzmq.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libzmq.pc

include_HEADERS = ../include/zmq.h ../include/zmq.hpp ../include/zmq_utils.h

libzmq_la_SOURCES = \
    array.hpp \
    atomic_counter.hpp \
    atomic_ptr.hpp \
    blob.hpp \
    clock.hpp \
    command.hpp \
    config.hpp \
    connect_session.hpp \
    ctx.hpp \
    decoder.hpp \
    device.hpp \
    devpoll.hpp \
    dist.hpp \
    encoder.hpp \
    epoll.hpp \
    err.hpp \
    fd.hpp \
    fq.hpp \
    i_inout.hpp \
    io_object.hpp \
    io_thread.hpp \
    ip.hpp \
    i_engine.hpp \
    i_poll_events.hpp \
    kqueue.hpp \
    lb.hpp \
    likely.hpp \
    mailbox.hpp \
    msg_content.hpp \
    mutex.hpp \
    named_session.hpp \
    object.hpp \
    options.hpp \
    own.hpp \
    pgm_receiver.hpp \
    pgm_sender.hpp \
    pgm_socket.hpp \
    pipe.hpp \
    platform.hpp \
    poll.hpp \
    poller.hpp \
    poller_base.hpp \
    pair.hpp \
    pub.hpp \
    pull.hpp \
    push.hpp \
    reaper.hpp \
    rep.hpp \
    req.hpp \
    select.hpp \
    semaphore.hpp \
    session.hpp \
    signaler.hpp \
    socket_base.hpp \
    stdint.hpp \
    sub.hpp \
    swap.hpp \
    tcp_connecter.hpp \
    tcp_listener.hpp \
    tcp_socket.hpp \
    thread.hpp \
    transient_session.hpp \
    trie.hpp \
    uuid.hpp \
    windows.hpp \
    wire.hpp \
    xpub.hpp \
    xrep.hpp \
    xreq.hpp \
    xsub.hpp \
    ypipe.hpp \
    yqueue.hpp \
    zmq_connecter.hpp \
    zmq_engine.hpp \
    zmq_init.hpp \
    zmq_listener.hpp \
    clock.cpp \
    command.cpp \
    ctx.cpp \
    connect_session.cpp \
    decoder.cpp \
    device.cpp \
    devpoll.cpp \
    dist.cpp \
    encoder.cpp \
    epoll.cpp \
    err.cpp \
    fq.cpp \
    io_object.cpp \
    io_thread.cpp \
    ip.cpp \
    kqueue.cpp \
    lb.cpp \
    mailbox.cpp \
    named_session.cpp \
    object.cpp \
    options.cpp \
    own.cpp \
    pair.cpp \
    pgm_receiver.cpp \
    pgm_sender.cpp \
    pgm_socket.cpp \
    pipe.cpp \
    poll.cpp \
    poller_base.cpp \
    pull.cpp \
    push.cpp \
    reaper.cpp \
    pub.cpp \
    rep.cpp \
    req.cpp \
    select.cpp \
    session.cpp \
    signaler.cpp \
    socket_base.cpp \
    sub.cpp \
    swap.cpp \
    tcp_connecter.cpp \
    tcp_listener.cpp \
    tcp_socket.cpp \
    thread.cpp \
    transient_session.cpp \
    trie.cpp \
    uuid.cpp \
    xpub.cpp \
    xrep.cpp \
    xreq.cpp \
    xsub.cpp \
    zmq.cpp \
    zmq_connecter.cpp \
    zmq_engine.cpp \
    zmq_init.cpp \
    zmq_listener.cpp

if ON_MINGW
libzmq_la_LDFLAGS = -no-undefined -avoid-version -version-info @LTVER@ @LIBZMQ_EXTRA_LDFLAGS@
else
libzmq_la_LDFLAGS = -version-info @LTVER@ @LIBZMQ_EXTRA_LDFLAGS@
endif

libzmq_la_CXXFLAGS = @LIBZMQ_EXTRA_CXXFLAGS@

if BUILD_PGM
libzmq_la_CPPFLAGS = -I$(top_srcdir)/@pgm_srcdir@/include/
libzmq_la_LIBADD = $(top_srcdir)/@pgm_srcdir@/libpgm_noinst.la
endif

dist-hook:
		-rm $(distdir)/platform.hpp
