LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

APP_PLATFORM = android-10

include $(CLEAR_VARS)
LOCAL_MODULE := uuid
LOCAL_SRC_FILES := uuid/compare.c uuid/gen_uuid.c uuid/isnull.c \
		uuid/parse.c uuid/unpack.c uuid/clear.c uuid/copy.c \
		 uuid/pack.c uuid/unparse.c uuid/uuid_time.c
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := zmq
LOCAL_C_INCLUDES := ../../include \
		../../builds/android/include
LOCAL_SRC_FILES := \
                clock.cpp command.cpp connect_session.cpp \
                ctx.cpp decoder.cpp device.cpp \
                devpoll.cpp dist.cpp encoder.cpp \
                epoll.cpp err.cpp fq.cpp \
                io_object.cpp io_thread.cpp ip.cpp \
                kqueue.cpp lb.cpp mailbox.cpp \
                named_session.cpp object.cpp options.cpp \
                own.cpp pair.cpp pgm_receiver.cpp \
                pgm_sender.cpp pgm_socket.cpp pipe.cpp \
                poll.cpp poller_base.cpp pub.cpp \
                pull.cpp push.cpp reaper.cpp rep.cpp \
                req.cpp select.cpp session.cpp \
                socket_base.cpp sub.cpp swap.cpp \
                tcp_connecter.cpp tcp_listener.cpp \
                tcp_socket.cpp thread.cpp \
                transient_session.cpp trie.cpp uuid.cpp \
                xpub.cpp xrep.cpp xreq.cpp \
                xsub.cpp zmq_connecter.cpp zmq.cpp \
                zmq_engine.cpp zmq_init.cpp zmq_listener.cpp \
                signaler.cpp
LOCAL_STATIC_LIBRARIES := uuid
LOCAL_LDLIBS := -lc -lm -lstdc++
include $(BUILD_SHARED_LIBRARY)
