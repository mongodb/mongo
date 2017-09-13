/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#ifndef OP_SYSTEM_H
#define OP_SYSTEM_H


#if (defined(dos) || defined(DOS))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64 
#       undef  darwin 
#       undef  interix 

#	define dos 1
#	define OP_SYSTEM dos


#elif (defined(vms) || defined(VMS))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef   win64
#       undef  darwin 
#       undef  interix 

#	define vms 2
#	define OP_SYSTEM vms


#elif ( defined(wnt) || defined(WNT) || defined(winnt))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define wnt 3
#	define OP_SYSTEM wnt


#elif (defined(linux) || defined(LINUX) || defined(__linux))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define linux 8
#	define OP_SYSTEM linux


#elif (defined(osf) || defined(OSF) || defined(__osf__))


#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define osf 4
#	define OP_SYSTEM osf


#elif (defined(hp_ux) || defined(HP_UX) || defined(__hpux) || defined(__HPUX))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define hp_ux 5
#	define OP_SYSTEM hp_ux


#elif (defined(unicos) || defined(UNICOS))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define unicos 6
#	define OP_SYSTEM unicos


#elif (defined(ultrix) || defined(ULTRIX))

#	undef  dos
#	undef  vms
#	undef  wnt
#	undef  osf
#	undef  hp_ux
#	undef  linux
#	undef  unicos
#	undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#	define ultrix 7
#	define OP_SYSTEM ultrix


#elif (defined(win64) || defined(WIN64))
#       undef  dos
#       undef  vms
#       undef  wnt
#       undef  osf
#       undef  hp_ux
#       undef  linux
#       undef  unicos
#       undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#       define win64 9
#       define OP_SYSTEM win64


#elif defined(__APPLE__) || defined(darwin)
#       undef  dos
#       undef  vms
#       undef  wnt
#       undef  osf
#       undef  hp_ux
#       undef  linux
#       undef  unicos
#       undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#       define darwin 10
#       define OP_SYSTEM darwin

#elif defined(interix)
#       undef  dos
#       undef  vms
#       undef  wnt
#       undef  osf
#       undef  hp_ux
#       undef  linux
#       undef  unicos
#       undef  ultrix
#       undef  win64
#       undef  darwin 
#       undef  interix 

#       define interix 11
#       define OP_SYSTEM interix
#else

#	error Operating system must be specified.

#endif

#define IS_UNIX ( \
	OP_SYSTEM == hp_ux || \
	OP_SYSTEM == linux || \
	OP_SYSTEM == osf || \
	OP_SYSTEM == ultrix || \
	OP_SYSTEM == unicos \
)

#endif  /* OP_SYSTEM_H */

