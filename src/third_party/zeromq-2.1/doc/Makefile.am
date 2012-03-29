MAN1 = 
MAN3 = zmq_bind.3 zmq_close.3 zmq_connect.3 zmq_device.3 zmq_init.3 \
    zmq_msg_close.3 zmq_msg_copy.3 zmq_msg_data.3 zmq_msg_init.3 \
    zmq_msg_init_data.3 zmq_msg_init_size.3 zmq_msg_move.3 zmq_msg_size.3 \
    zmq_poll.3 zmq_recv.3 zmq_send.3 zmq_setsockopt.3 zmq_socket.3 \
    zmq_strerror.3 zmq_term.3 zmq_version.3 zmq_getsockopt.3 zmq_errno.3
MAN7 = zmq.7 zmq_tcp.7 zmq_pgm.7 zmq_epgm.7 zmq_inproc.7 zmq_ipc.7 \
    zmq_cpp.7
MAN_DOC = $(MAN1) $(MAN3) $(MAN7)

MAN_TXT = $(MAN1:%.1=%.txt)
MAN_TXT += $(MAN3:%.3=%.txt)
MAN_TXT += $(MAN7:%.7=%.txt)
MAN_HTML = $(MAN_TXT:%.txt=%.html)

if INSTALL_MAN
dist_man_MANS = $(MAN_DOC)
endif

EXTRA_DIST = asciidoc.conf $(MAN_TXT)
if BUILD_DOC
EXTRA_DIST += $(MAN_HTML)
endif

MAINTAINERCLEANFILES = $(MAN_DOC) $(MAN_HTML)

dist-hook : $(MAN_DOC) $(MAN_HTML)

if BUILD_DOC
SUFFIXES=.html .txt .xml .1 .3 .7

.txt.html:
	$(AM_V_GEN)$(ASCIIDOC) -d manpage -b xhtml11 -f asciidoc.conf \
        -azmq_version=@PACKAGE_VERSION@ $<
.txt.xml:
	$(AM_V_GEN)$(ASCIIDOC) -d manpage -b docbook -f asciidoc.conf \
		-azmq_version=@PACKAGE_VERSION@ $<
.xml.1:
	$(AM_V_GEN)$(XMLTO) man $<
.xml.3:
	$(AM_V_GEN)$(XMLTO) man $<
.xml.7:
	$(AM_V_GEN)$(XMLTO) man $<
zmq_epgm.7: zmq_pgm.7
	$(AM_V_GEN)cp zmq_pgm.7 $@
endif
