#
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# You may not use this file except in compliance with the License.
# A copy of the License is located at
#
#  http://aws.amazon.com/apache2.0
#
# or in the "license" file accompanying this file. This file is distributed
# on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
# express or implied. See the License for the specific language governing
# permissions and limitations under the License.
#

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)
SUB_BUILDS = $(patsubst %/Makefile, %, $(wildcard */Makefile))

BITCODE_DIR?=../tests/saw/bitcode/

BCS_1=s2n_handshake_io.bc s2n_handshake_type.bc s2n_connection.bc s2n_kex.bc \
	s2n_tls13.bc s2n_quic_support.bc s2n_tls13_key_schedule.bc
BCS=$(addprefix $(BITCODE_DIR), $(BCS_1))

.PHONY : all
all: $(OBJS)
	$(foreach subfolder, $(SUB_BUILDS), $(MAKE) -C $(subfolder);)

.PHONY : bc
bc: $(BCS)
	$(foreach subfolder, $(SUB_BUILDS), $(MAKE) -C $(subfolder) bc;)

.PHONY : run-lcov
run-lcov: lcov
	$(foreach subfolder, $(SUB_BUILDS), $(MAKE) -C $(subfolder) lcov;)
	
.PHONY : indent
indent: indentsource
	$(foreach subfolder, $(SUB_BUILDS), ${MAKE} -C $(subfolder) indentsource;)

.PHONY : clean
clean: decruft
	$(foreach subfolder, $(SUB_BUILDS), ${MAKE} -C $(subfolder) decruft;)

include ../s2n.mk
