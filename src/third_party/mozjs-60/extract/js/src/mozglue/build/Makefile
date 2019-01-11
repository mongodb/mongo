# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT MODIFY BY HAND.
DEPTH := ../..
topobjdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
topsrcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release
srcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/mozglue/build
VPATH := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/mozglue/build
relativesrcdir := mozglue/build
include $(DEPTH)/config/autoconf.mk
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# For FORCE_SHARED_LIB
include $(topsrcdir)/config/config.mk

ifeq (WINNT,$(OS_TARGET))
mozglue.def: mozglue.def.in $(GLOBAL_DEPS)
	$(call py_action,preprocessor,$(if $(MOZ_REPLACE_MALLOC),-DMOZ_REPLACE_MALLOC) $(ACDEFINES) $< -o $@)

GARBAGE += mozglue.def
endif

include $(topsrcdir)/mozglue/build/replace_malloc.mk

include $(topsrcdir)/config/recurse.mk
