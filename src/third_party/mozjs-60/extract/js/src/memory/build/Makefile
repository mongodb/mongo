# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT MODIFY BY HAND.
DEPTH := ../..
topobjdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
topsrcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release
srcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/build
VPATH := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/build
relativesrcdir := memory/build
include $(DEPTH)/config/autoconf.mk
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Workaround for alignment problems in gcov code. See
# https://bugzilla.mozilla.org/show_bug.cgi?id=1413570#c2.
ifeq ($(CPU_ARCH),x86)
PROFILE_GEN_CFLAGS += -mno-sse2
endif

include $(topsrcdir)/config/recurse.mk
