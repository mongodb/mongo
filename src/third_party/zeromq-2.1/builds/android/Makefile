LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

APP_PLATFORM = android-8

LOCAL_MODULE := libzmq
LOCAL_SRC_FILES := clock.cpp options.cpp socket_base.cpp command.cpp own.cpp streamer.cpp connect_session.cpp pair.cpp sub.cpp ctx.cpp pgm_receiver.cpp swap.cpp decoder.cpp pgm_sender.cpp tcp_connecter.cpp devpoll.cpp pgm_socket.cpp tcp_listener.cpp encoder.cpp pipe.cpp tcp_socket.cpp epoll.cpp poll.cpp thread.cpp err.cpp poller_base.cpp transient_session.cpp forwarder.cpp pub.cpp trie.cpp fq.cpp pull.cpp uuid.cpp io_object.cpp push.cpp xrep.cpp io_thread.cpp queue.cpp xreq.cpp ip.cpp rep.cpp zmq_connecter.cpp kqueue.cpp req.cpp zmq.cpp lb.cpp select.cpp zmq_engine.cpp named_session.cpp session.cpp zmq_init.cpp object.cpp zmq_listener.cpp mailbox.cpp

LOCAL_LDLIBS := -luuid -lc -lm -lstdc++
include $(BUILD_SHARED_LIBRARY)
