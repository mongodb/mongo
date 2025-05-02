# Copyright 2010-2014 Mario Mulansky
# Copyright 2010-2012 Karsten Ahnert
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or
# copy at http://www.boost.org/LICENSE_1_0.txt)

# make sure BOOST_ROOT is pointing to your boost directory
# otherwise, set it here:
# BOOST_ROOT = /path/to/boost

# path to the cuda installation
CUDA_ROOT = /usr/local/cuda
# target architecture
ARCH = sm_13

NVCC = $(CUDA_ROOT)/bin/nvcc

INCLUDES += -I../../include/ -I$(BOOST_ROOT)

NVCCFLAGS = -O3 $(INCLUDES) -arch $(ARCH)

%.o : %.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

% : %.o
	$(NVCC) $(NVCCFLAGS) -o $@ $<


all : check_thrust


clean :
	-rm *~ *.o check_thrust
