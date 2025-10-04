#
# This file is a part of the libunwind project.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# This permission notice shall be included in all copies or substantial portions
# of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

# Set the DSO versions
SOVERSION=9:0:1		# See comments at end of file.
SETJMP_SO_VERSION=0:0:0
COREDUMP_SO_VERSION=0:0:0

#
# Don't link with start-files since we don't use any constructors/destructors:
#
COMMON_SO_LDFLAGS = $(LDFLAGS_NOSTARTFILES)

#
# Which libraries to build and install
#
# Order is important here. The names of the libraries need to end up in reverse
# dependency order for `make install` to do its job properly.
#
lib_LTLIBRARIES =
if !REMOTE_ONLY
 lib_LTLIBRARIES += libunwind.la
endif
if ARCH_AARCH64
 lib_LTLIBRARIES += libunwind-aarch64.la
endif
if ARCH_ARM
 lib_LTLIBRARIES += libunwind-arm.la
endif
if ARCH_HPPA
 lib_LTLIBRARIES += libunwind-hppa.la
endif
if ARCH_IA64
 lib_LTLIBRARIES += libunwind-ia64.la
endif
if ARCH_LOONGARCH64
 lib_LTLIBRARIES += libunwind-loongarch64.la
endif
if ARCH_MIPS
 lib_LTLIBRARIES += libunwind-mips.la
endif
if ARCH_PPC32
 lib_LTLIBRARIES += libunwind-ppc32.la
endif
if ARCH_PPC64
 lib_LTLIBRARIES += libunwind-ppc64.la
endif
if ARCH_RISCV
 lib_LTLIBRARIES += libunwind-riscv.la
endif
if ARCH_S390X
 lib_LTLIBRARIES += libunwind-s390x.la
endif
if ARCH_SH
 lib_LTLIBRARIES += libunwind-sh.la
endif
if ARCH_X86
 lib_LTLIBRARIES += libunwind-x86.la
endif
if ARCH_X86_64
 lib_LTLIBRARIES += libunwind-x86_64.la
endif

if BUILD_COREDUMP
 lib_LTLIBRARIES += libunwind-coredump.la
endif
if BUILD_NTO
 lib_LTLIBRARIES += libunwind-nto.la
endif
if BUILD_PTRACE
 lib_LTLIBRARIES += libunwind-ptrace.la
endif
if BUILD_SETJMP
 lib_LTLIBRARIES += libunwind-setjmp.la
endif

noinst_HEADERS =
noinst_LTLIBRARIES =

if USE_ELF32
libunwind_elf_libs = libunwind-elf32.la
endif
if USE_ELF64
libunwind_elf_libs = libunwind-elf64.la
endif
if USE_ELFXX
libunwind_elf_libs = libunwind-elfxx.la
endif

# If local unwinding is being built, link in the local unwinding functions
libunwind_libadd =
if !REMOTE_ONLY
  libunwind_libadd += libunwind.la -lc
endif

### pkg-config:
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libunwind-generic.pc

if !REMOTE_ONLY
pkgconfig_DATA += unwind/libunwind.pc
endif
if BUILD_COREDUMP
pkgconfig_DATA += coredump/libunwind-coredump.pc
endif
if BUILD_PTRACE
pkgconfig_DATA += ptrace/libunwind-ptrace.pc
endif
if BUILD_SETJMP
pkgconfig_DATA += setjmp/libunwind-setjmp.pc
endif

### libunwind-coredump:
noinst_HEADERS += coredump/_UCD_internal.h     \
                  coredump/_UCD_lib.h          \
                  coredump/ucd_file_table.h
libunwind_coredump_la_SOURCES =                \
	coredump/_UCD_access_mem.c             \
	coredump/_UCD_accessors.c              \
	coredump/_UCD_corefile_elf.c           \
	coredump/_UCD_create.c                 \
	coredump/_UCD_destroy.c                \
	coredump/_UCD_elf_map_image.c          \
	coredump/ucd_file_table.c              \
	coredump/_UCD_find_proc_info.c         \
	coredump/_UCD_get_proc_name.c          \
	coredump/_UCD_get_elf_filename.c       \
	\
	mi/init.c                              \
	coredump/_UPT_elf.c                    \
	coredump/_UPT_access_fpreg.c           \
	coredump/_UPT_get_dyn_info_list_addr.c \
	coredump/_UPT_put_unwind_info.c        \
	coredump/_UPT_resume.c
libunwind_coredump_la_LDFLAGS =                \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(COREDUMP_SO_VERSION)
libunwind_coredump_la_LIBADD =                 \
	libunwind-$(arch).la                   \
	$(LIBLZMA) $(LIBZ)

### libunwind-nto:
noinst_HEADERS += nto/unw_nto_internal.h
libunwind_nto_la_SOURCES =                     \
	mi/init.c                              \
	nto/unw_nto_access_fpreg.c             \
	nto/unw_nto_access_mem.c               \
	nto/unw_nto_accessors.c                \
	nto/unw_nto_access_reg.c               \
	nto/unw_nto_create.c                   \
	nto/unw_nto_destroy.c                  \
	nto/unw_nto_elf.c                      \
	nto/unw_nto_find_proc_info.c           \
	nto/unw_nto_get_dyn_info_list_addr.c   \
	nto/unw_nto_get_proc_name.c            \
	nto/unw_nto_get_elf_filename.c         \
	nto/unw_nto_put_unwind_info.c          \
	nto/unw_nto_resume.c
libunwind_nto_la_LIBADD =                      \
	libunwind-$(arch).la                   \
	$(libunwind_libadd)

### libunwind-ptrace:
noinst_HEADERS += ptrace/_UPT_internal.h
libunwind_ptrace_la_SOURCES =                  \
	mi/init.c                              \
	ptrace/_UPT_access_fpreg.c             \
	ptrace/_UPT_access_mem.c               \
	ptrace/_UPT_accessors.c                \
	ptrace/_UPT_access_reg.c               \
	ptrace/_UPT_create.c                   \
	ptrace/_UPT_destroy.c                  \
	ptrace/_UPT_elf.c                      \
	ptrace/_UPT_find_proc_info.c           \
	ptrace/_UPT_get_dyn_info_list_addr.c   \
	ptrace/_UPT_get_proc_name.c            \
	ptrace/_UPT_get_elf_filename.c         \
	ptrace/_UPT_put_unwind_info.c          \
	ptrace/_UPT_reg_offset.c               \
	ptrace/_UPT_resume.c
libunwind_ptrace_la_LIBADD =                   \
	libunwind-$(arch).la                   \
	$(LIBLZMA) $(LIBZ)

### libunwind-setjmp:
noinst_HEADERS += setjmp/setjmp_i.h
libunwind_setjmp_la_SOURCES =                  \
	mi/init.c                              \
	setjmp/longjmp.c                       \
	setjmp/siglongjmp.c
libunwind_setjmp_la_LDFLAGS =                  \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SETJMP_SO_VERSION)
libunwind_setjmp_la_LIBADD =                   \
	$(libunwind_elf_libs)                  \
	libunwind-$(arch).la                   \
	$(libunwind_libadd)

### libunwind:
libunwind_la_LIBADD =

# List of arch-independent files needed by both local-only and generic
# libraries:
libunwind_la_SOURCES_common =                  \
	$(libunwind_la_SOURCES_os)             \
	mi/init.c                              \
	mi/flush_cache.c                       \
	mi/mempool.c                           \
	mi/strerror.c

# List of arch-independent files needed by generic library (libunwind-$ARCH):
libunwind_la_SOURCES_generic =                 \
	mi/Gaddress_validator.c                \
	mi/Gdestroy_addr_space.c               \
	mi/Gdyn-extract.c                      \
	mi/Gdyn-remote.c                       \
	mi/Gfind_dynamic_proc_info.c           \
	mi/Gget_accessors.c                    \
	mi/Gget_fpreg.c                        \
	mi/Gget_proc_info_by_ip.c              \
	mi/Gget_proc_name.c                    \
	mi/Gget_reg.c                          \
	mi/Gput_dynamic_unwind_info.c          \
	mi/Gset_cache_size.c                   \
	mi/Gset_caching_policy.c               \
	mi/Gset_fpreg.c                        \
	mi/Gset_iterate_phdr_function.c        \
	mi/Gset_reg.c                          \
	mi/Gget_elf_filename.c

if SUPPORT_CXX_EXCEPTIONS
libunwind_la_SOURCES_local_unwind =            \
	unwind/Backtrace.c                     \
	unwind/DeleteException.c               \
	unwind/FindEnclosingFunction.c         \
	unwind/ForcedUnwind.c                  \
	unwind/GetBSP.c                        \
	unwind/GetCFA.c                        \
	unwind/GetDataRelBase.c                \
	unwind/GetGR.c                         \
	unwind/GetIP.c                         \
	unwind/GetIPInfo.c                     \
	unwind/GetLanguageSpecificData.c       \
	unwind/GetRegionStart.c                \
	unwind/GetTextRelBase.c                \
	unwind/RaiseException.c                \
	unwind/Resume.c                        \
	unwind/Resume_or_Rethrow.c             \
	unwind/SetGR.c                         \
	unwind/SetIP.c

#  _ReadULEB()/_ReadSLEB() are needed for Intel C++ 8.0 compatibility
libunwind_la_SOURCES_os_linux_local =          \
	mi/_ReadULEB.c                         \
	mi/_ReadSLEB.c
endif SUPPORT_CXX_EXCEPTIONS

# List of arch-independent files needed by local-only library (libunwind):
libunwind_la_SOURCES_local_nounwind =          \
	$(libunwind_la_SOURCES_os_local)       \
	mi/backtrace.c                         \
	mi/dyn-cancel.c                        \
	mi/dyn-info-list.c                     \
	mi/dyn-register.c                      \
	mi/Laddress_validator.c                \
	mi/Ldestroy_addr_space.c               \
	mi/Ldyn-extract.c                      \
	mi/Lfind_dynamic_proc_info.c           \
	mi/Lget_accessors.c                    \
	mi/Lget_fpreg.c mi/Lset_fpreg.c        \
	mi/Lget_proc_info_by_ip.c              \
	mi/Lget_proc_name.c                    \
	mi/Lget_reg.c                          \
	mi/Lput_dynamic_unwind_info.c          \
	mi/Lset_cache_size.c                   \
	mi/Lset_caching_policy.c               \
	mi/Lset_iterate_phdr_function.c        \
	mi/Lset_reg.c                          \
	mi/Lget_elf_filename.c

libunwind_la_SOURCES_local =                   \
	$(libunwind_la_SOURCES_local_nounwind) \
	$(libunwind_la_SOURCES_local_unwind)

noinst_HEADERS += os-linux.h

libunwind_dwarf_common_la_SOURCES = dwarf/global.c

libunwind_dwarf_local_la_SOURCES =             \
	dwarf/Lexpr.c                          \
	dwarf/Lfde.c                           \
	dwarf/Lfind_proc_info-lsb.c            \
	dwarf/Lfind_unwind_table.c             \
	dwarf/Lget_proc_info_in_range.c        \
	dwarf/Lparser.c                        \
	dwarf/Lpe.c
libunwind_dwarf_local_la_LIBADD = libunwind-dwarf-common.la

libunwind_dwarf_generic_la_SOURCES =           \
	dwarf/Gexpr.c                          \
	dwarf/Gfde.c                           \
	dwarf/Gfind_proc_info-lsb.c            \
	dwarf/Gfind_unwind_table.c             \
	dwarf/Gget_proc_info_in_range.c        \
	dwarf/Gparser.c                        \
	dwarf/Gpe.c
libunwind_dwarf_generic_la_LIBADD = libunwind-dwarf-common.la

if USE_DWARF
 noinst_LTLIBRARIES += libunwind-dwarf-common.la libunwind-dwarf-generic.la
if !REMOTE_ONLY
 noinst_LTLIBRARIES += libunwind-dwarf-local.la
endif
 libunwind_la_LIBADD += libunwind-dwarf-local.la
endif

noinst_HEADERS += elf32.h elf64.h elfxx.h

libunwind_elf32_la_SOURCES = elf32.c
libunwind_elf64_la_SOURCES = elf64.c
libunwind_elfxx_la_SOURCES = elfxx.c
libunwind_elf32_la_LIBADD  = $(LIBLZMA) $(LIBZ)
libunwind_elf64_la_LIBADD  = $(LIBLZMA) $(LIBZ)
libunwind_elfxx_la_LIBADD  = $(LIBLZMA) $(LIBZ)

noinst_LTLIBRARIES += $(libunwind_elf_libs)
libunwind_la_LIBADD += $(libunwind_elf_libs)

if OS_LINUX
 libunwind_la_SOURCES_os               = os-linux.c dl-iterate-phdr.c
 libunwind_la_SOURCES_os_local         = $(libunwind_la_SOURCES_os_linux_local)
 libunwind_la_SOURCES_aarch64_os       = aarch64/Gos-linux.c
 libunwind_la_SOURCES_aarch64_os_local = aarch64/Los-linux.c
 libunwind_la_SOURCES_arm_os           = arm/Gos-linux.c
 libunwind_la_SOURCES_arm_os_local     = arm/Los-linux.c
 libunwind_la_SOURCES_x86_os           = x86/Gos-linux.c
 libunwind_la_SOURCES_x86_os_local     = x86/Los-linux.c x86/getcontext-linux.S
 libunwind_la_SOURCES_x86_64_os        = x86_64/Gos-linux.c
 libunwind_la_SOURCES_x86_64_os_local  = x86_64/Los-linux.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_access_reg_linux.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_threadinfo_prstatus.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_mapinfo_linux.c
endif

if OS_HPUX
 libunwind_la_SOURCES_os        = os-hpux.c
 libunwind_la_SOURCES_os_local  = $(libunwind_la_SOURCES_os_hpux_local)
endif

if OS_FREEBSD
 libunwind_la_SOURCES_os               = os-freebsd.c
 libunwind_la_SOURCES_aarch64_os       = aarch64/Gos-freebsd.c
 libunwind_la_SOURCES_aarch64_os_local = aarch64/Los-freebsd.c aarch64/setcontext.S
 libunwind_la_SOURCES_arm_os           = arm/Gos-freebsd.c
 libunwind_la_SOURCES_arm_os_local     = arm/Los-freebsd.c
 libunwind_la_SOURCES_x86_os           = x86/Gos-freebsd.c
 libunwind_la_SOURCES_x86_os_local     = x86/Los-freebsd.c x86/getcontext-freebsd.S
 libunwind_la_SOURCES_x86_64_os        = x86_64/Gos-freebsd.c
 libunwind_la_SOURCES_x86_64_os_local  = x86_64/Los-freebsd.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_access_reg_freebsd.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_threadinfo_prstatus.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_mapinfo_generic.c
endif

if OS_SOLARIS
 libunwind_la_SOURCES_os              = os-solaris.c
 libunwind_la_SOURCES_x86_64_os       = x86_64/Gos-solaris.c
 libunwind_la_SOURCES_x86_64_os_local = x86_64/Los-solaris.c
endif

if OS_QNX
 libunwind_la_SOURCES_os               = os-qnx.c
 libunwind_la_SOURCES_aarch64_os       = aarch64/Gos-qnx.c
 libunwind_la_SOURCES_aarch64_os_local = aarch64/Los-qnx.c
 libunwind_la_SOURCES_arm_os           = arm/Gos-other.c
 libunwind_la_SOURCES_arm_os_local     = arm/Los-other.c
 libunwind_la_SOURCES_x86_64_os        = x86_64/Gos-qnx.c
 libunwind_la_SOURCES_x86_64_os_local  = x86_64/Los-qnx.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_access_reg_qnx.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_threadinfo_prstatus.c
 libunwind_coredump_la_SOURCES += coredump/_UCD_get_mapinfo_qnx.c
endif

### target AArch64:
# The list of files that go into libunwind and libunwind-aarch64:
noinst_HEADERS += aarch64/init.h aarch64/ucontext_i.h aarch64/unwind_i.h
libunwind_la_SOURCES_aarch64_common =          \
	$(libunwind_la_SOURCES_common)         \
	aarch64/is_fpreg.c                     \
	aarch64/regname.c

# The list of files that go into libunwind:
if ARCH_AARCH64
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_aarch64_common) \
	$(libunwind_la_SOURCES_local)          \
	$(libunwind_la_SOURCES_aarch64_os_local)   \
	aarch64/getcontext.S                   \
	aarch64/Lapply_reg_state.c             \
	aarch64/Lcreate_addr_space.c           \
	aarch64/Lget_proc_info.c               \
	aarch64/Lget_save_loc.c                \
	aarch64/Lglobal.c                      \
	aarch64/Linit.c                        \
	aarch64/Linit_local.c                  \
	aarch64/Linit_remote.c                 \
	aarch64/Lis_signal_frame.c             \
	aarch64/Lregs.c                        \
	aarch64/Lreg_states_iterate.c          \
	aarch64/Lresume.c                      \
	aarch64/Lstash_frame.c                 \
	aarch64/Lstep.c                        \
	aarch64/Ltrace.c

libunwind_setjmp_la_SOURCES +=                 \
	aarch64/longjmp.S                      \
	aarch64/siglongjmp.S
endif ARCH_AARCH64

libunwind_aarch64_la_SOURCES =                 \
	$(libunwind_la_SOURCES_aarch64_common) \
	$(libunwind_la_SOURCES_generic)        \
	$(libunwind_la_SOURCES_aarch64_os)     \
	aarch64/Gapply_reg_state.c             \
	aarch64/Gcreate_addr_space.c           \
	aarch64/Gget_proc_info.c               \
	aarch64/Gget_save_loc.c                \
	aarch64/Gglobal.c                      \
	aarch64/Ginit.c                        \
	aarch64/Ginit_local.c                  \
	aarch64/Ginit_remote.c                 \
	aarch64/Gis_signal_frame.c             \
	aarch64/Gregs.c                        \
	aarch64/Greg_states_iterate.c          \
	aarch64/Gresume.c                      \
	aarch64/Gstash_frame.c                 \
	aarch64/Gstep.c                        \
	aarch64/Gtrace.c
libunwind_aarch64_la_LDFLAGS =                 \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_aarch64_la_LIBADD =                  \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target ARMv7:
# The list of files that go into libunwind and libunwind-arm:
noinst_HEADERS +=                              \
	arm/init.h                             \
	arm/offsets.h                          \
	arm/unwind_i.h
libunwind_la_SOURCES_arm_common =              \
	$(libunwind_la_SOURCES_common)         \
	arm/is_fpreg.c arm/regname.c

# The list of files that go into libunwind:
if ARCH_ARM
libunwind_la_SOURCES = \
	$(libunwind_la_SOURCES_arm_common)     \
	$(libunwind_la_SOURCES_arm_os_local)   \
	$(libunwind_la_SOURCES_local)          \
	arm/getcontext.S                       \
	arm/Lapply_reg_state.c                 \
	arm/Lcreate_addr_space.c               \
	arm/Lex_tables.c                       \
	arm/Lget_proc_info.c                   \
	arm/Lget_save_loc.c                    \
	arm/Lglobal.c arm/Linit.c              \
	arm/Linit_local.c                      \
	arm/Linit_remote.c                     \
	arm/Lregs.c                            \
	arm/Lreg_states_iterate.c              \
	arm/Lresume.c                          \
	arm/Lstash_frame.c                     \
	arm/Lstep.c                            \
	arm/Ltrace.c

libunwind_setjmp_la_SOURCES += arm/siglongjmp.S
endif # ARCH_ARM

# The list of files that go into libunwind-arm:
libunwind_arm_la_SOURCES =                     \
	$(libunwind_la_SOURCES_arm_common)     \
	$(libunwind_la_SOURCES_arm_os)         \
	$(libunwind_la_SOURCES_generic)        \
	arm/Gapply_reg_state.c                 \
	arm/Gcreate_addr_space.c               \
	arm/Gex_tables.c                       \
	arm/Gget_proc_info.c                   \
	arm/Gget_save_loc.c                    \
	arm/Gglobal.c                          \
	arm/Ginit.c                            \
	arm/Ginit_local.c                      \
	arm/Ginit_remote.c                     \
	arm/Gregs.c                            \
	arm/Greg_states_iterate.c              \
	arm/Gresume.c                          \
	arm/Gstash_frame.c                     \
	arm/Gstep.c                            \
	arm/Gtrace.c
libunwind_arm_la_LDFLAGS =                     \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_arm_la_LIBADD =                      \
	libunwind-dwarf-generic.la             \
	libunwind-elf32.la                     \
	$(libunwind_libadd)

### target HP-PA:
# The list of files that go both into libunwind and libunwind-hppa:
noinst_HEADERS +=                              \
	hppa/init.h                            \
	hppa/offsets.h                         \
	hppa/unwind_i.h
libunwind_la_SOURCES_hppa_common =             \
	$(libunwind_la_SOURCES_common)	       \
	hppa/regname.c

if ARCH_HPPA
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_hppa_common)    \
	$(libunwind_la_SOURCES_local)          \
	hppa/getcontext.S                      \
	hppa/Lapply_reg_state.c                \
	hppa/Lcreate_addr_space.c              \
	hppa/Lget_proc_info.c                  \
	hppa/Lget_save_loc.c                   \
	hppa/Lglobal.c                         \
	hppa/Linit.c                           \
	hppa/Linit_local.c                     \
	hppa/Linit_remote.c                    \
	hppa/Lis_signal_frame.c                \
	hppa/Lregs.c                           \
	hppa/Lreg_states_iterate.c             \
	hppa/Lresume.c                         \
	hppa/Lstep.c                           \
	hppa/setcontext.S

libunwind_setjmp_la_SOURCES += hppa/siglongjmp.S
endif # ARCH_HPPA

# The list of files that go into libunwind-hppa:
libunwind_hppa_la_SOURCES =                    \
	$(libunwind_la_SOURCES_hppa_common)    \
	$(libunwind_la_SOURCES_generic)        \
	hppa/Gapply_reg_state.c                \
	hppa/Gcreate_addr_space.c              \
	hppa/Gget_proc_info.c                  \
	hppa/Gget_save_loc.c                   \
	hppa/Gglobal.c                         \
	hppa/Ginit.c                           \
	hppa/Ginit_local.c                     \
	hppa/Ginit_remote.c                    \
	hppa/Gis_signal_frame.c                \
	hppa/Gregs.c                           \
	hppa/Greg_states_iterate.c             \
	hppa/Gresume.c                         \
	hppa/Gstep.c
libunwind_hppa_la_LDFLAGS =                    \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_hppa_la_LIBADD =                     \
	libunwind-dwarf-generic.la             \
	libunwind-elf32.la                     \
	$(libunwind_libadd)

### target IA-64:
# The list of files that go both into libunwind and libunwind-ia64:
noinst_HEADERS +=                              \
	ia64/init.h                            \
	ia64/offsets.h                         \
	ia64/regs.h                            \
	ia64/ucontext_i.h                      \
	ia64/unwind_decoder.h                  \
	ia64/unwind_i.h
libunwind_la_SOURCES_ia64_common =             \
	$(libunwind_la_SOURCES_common)         \
	ia64/regname.c
libunwind_la_EXTRAS_ia64 =                     \
	ia64/mk_cursor_i                       \
	ia64/mk_Gcursor_i.c                    \
	ia64/mk_Lcursor_i.c

if ARCH_IA64
BUILT_SOURCES = Gcursor_i.h Lcursor_i.h
mk_Gcursor_i.s: $(srcdir)/ia64/mk_Gcursor_i.c
	$(COMPILE) -S "$(srcdir)/ia64/mk_Gcursor_i.c" -o mk_Gcursor_i.s
mk_Lcursor_i.s: $(srcdir)/ia64/mk_Lcursor_i.c
	$(COMPILE) -S "$(srcdir)/ia64/mk_Lcursor_i.c" -o mk_Lcursor_i.s
Gcursor_i.h: mk_Gcursor_i.s
	"$(srcdir)/ia64/mk_cursor_i" mk_Gcursor_i.s > Gcursor_i.h
Lcursor_i.h: mk_Lcursor_i.s
	"$(srcdir)/ia64/mk_cursor_i" mk_Lcursor_i.s > Lcursor_i.h

# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_ia64_common)    \
	$(libunwind_la_SOURCES_local)          \
	ia64/dyn_info_list.S                   \
	ia64/getcontext.S                      \
	ia64/Lapply_reg_state.c                \
	ia64/Lcreate_addr_space.c              \
	ia64/Lfind_unwind_table.c              \
	ia64/Lget_proc_info.c                  \
	ia64/Lget_save_loc.c                   \
	ia64/Lglobal.c                         \
	ia64/Linit.c                           \
	ia64/Linit_local.c                     \
	ia64/Linit_remote.c                    \
	ia64/Linstall_cursor.S                 \
	ia64/Lis_signal_frame.c                \
	ia64/Lparser.c                         \
	ia64/Lrbs.c                            \
	ia64/Lregs.c                           \
	ia64/Lreg_states_iterate.c             \
	ia64/Lresume.c                         \
	ia64/Lscript.c                         \
	ia64/Lstep.c                           \
	ia64/Ltables.c

libunwind_setjmp_la_SOURCES +=                 \
	ia64/longjmp.S                         \
	ia64/setjmp.S                          \
	ia64/siglongjmp.S                      \
	ia64/sigsetjmp.S
endif # ARCH_IA64

# The list of files that go into libunwind-ia64:
libunwind_ia64_la_SOURCES =                    \
	$(libunwind_la_SOURCES_ia64_common)    \
	$(libunwind_la_SOURCES_generic)        \
	ia64/Gapply_reg_state.c                \
	ia64/Gcreate_addr_space.c              \
	ia64/Gfind_unwind_table.c              \
	ia64/Gget_proc_info.c                  \
	ia64/Gget_save_loc.c                   \
	ia64/Gglobal.c                         \
	ia64/Ginit.c                           \
	ia64/Ginit_local.c                     \
	ia64/Ginit_remote.c                    \
	ia64/Ginstall_cursor.S                 \
	ia64/Gis_signal_frame.c                \
	ia64/Gparser.c                         \
	ia64/Grbs.c                            \
	ia64/Gregs.c                           \
	ia64/Greg_states_iterate.c             \
	ia64/Gresume.c                         \
	ia64/Gscript.c                         \
	ia64/Gstep.c                           \
	ia64/Gtables.c
libunwind_ia64_la_LDFLAGS =                    \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_ia64_la_LIBADD =                     \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target LoongArch64:
# The list of files that go info libunwind and libunwind-loongarch64:
noinst_HEADERS +=                              \
	loongarch64/init.h                     \
	loongarch64/offsets.h                  \
	loongarch64/unwind_i.h
libunwind_la_SOURCES_loongarch64_common =      \
	$(libunwind_la_SOURCES_common)         \
	loongarch64/is_fpreg.c                 \
	loongarch64/regname.c

if ARCH_LOONGARCH64
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_loongarch64_common) \
	$(libunwind_la_SOURCES_local)          \
	loongarch64/getcontext.S               \
	loongarch64/Lapply_reg_state.c         \
	loongarch64/Lcreate_addr_space.c       \
	loongarch64/Lget_proc_info.c           \
	loongarch64/Lget_save_loc.c            \
	loongarch64/Lglobal.c                  \
	loongarch64/Linit.c                    \
	loongarch64/Linit_local.c              \
	loongarch64/Linit_remote.c             \
	loongarch64/Lis_signal_frame.c         \
	loongarch64/Lregs.c                    \
	loongarch64/Lreg_states_iterate.c      \
	loongarch64/Lresume.c                  \
	loongarch64/Lstep.c

libunwind_setjmp_la_SOURCES +=                 \
	loongarch64/siglongjmp.S
endif # ARCH_LOONGARCH64

libunwind_loongarch64_la_SOURCES =             \
	$(libunwind_la_SOURCES_loongarch64_common) \
	$(libunwind_la_SOURCES_generic)        \
	loongarch64/Gapply_reg_state.c         \
	loongarch64/Gcreate_addr_space.c       \
	loongarch64/Gget_proc_info.c           \
	loongarch64/Gget_save_loc.c            \
	loongarch64/Gglobal.c                  \
	loongarch64/Ginit.c                    \
	loongarch64/Ginit_local.c              \
	loongarch64/Ginit_remote.c             \
	loongarch64/Gis_signal_frame.c         \
	loongarch64/Gregs.c                    \
	loongarch64/Greg_states_iterate.c      \
	loongarch64/Gresume.c                  \
	loongarch64/Gstep.c
libunwind_loongarch64_la_LDFLAGS =             \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_loongarch64_la_LIBADD =              \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target MIPS:
# The list of files that go info libunwind and libunwind-mips:
noinst_HEADERS +=                              \
	mips/init.h                            \
	mips/offsets.h                         \
	mips/unwind_i.h
libunwind_la_SOURCES_mips_common =             \
	$(libunwind_la_SOURCES_common)         \
	mips/is_fpreg.c                        \
	mips/regname.c

if ARCH_MIPS
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_mips_common)    \
	$(libunwind_la_SOURCES_local)          \
	mips/getcontext.S                      \
	mips/Lapply_reg_state.c                \
	mips/Lcreate_addr_space.c              \
	mips/Lget_proc_info.c                  \
	mips/Lget_save_loc.c                   \
	mips/Lglobal.c                         \
	mips/Linit.c                           \
	mips/Linit_local.c                     \
	mips/Linit_remote.c                    \
	mips/Lis_signal_frame.c                \
	mips/Lregs.c                           \
	mips/Lreg_states_iterate.c             \
	mips/Lresume.c                         \
	mips/Lstep.c

libunwind_setjmp_la_SOURCES += mips/siglongjmp.S
endif # ARCH_MIPS

libunwind_mips_la_SOURCES =                    \
	$(libunwind_la_SOURCES_mips_common)    \
	$(libunwind_la_SOURCES_generic)        \
	mips/Gapply_reg_state.c                \
	mips/Gcreate_addr_space.c              \
	mips/Gget_proc_info.c                  \
	mips/Gget_save_loc.c                   \
	mips/Gglobal.c                         \
	mips/Ginit.c                           \
	mips/Ginit_local.c                     \
	mips/Ginit_remote.c                    \
	mips/Gis_signal_frame.c                \
	mips/Gregs.c                           \
	mips/Greg_states_iterate.c             \
	mips/Gresume.c                         \
	mips/Gstep.c
libunwind_mips_la_LDFLAGS =                    \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_mips_la_LIBADD =                     \
	libunwind-dwarf-generic.la             \
	libunwind-elfxx.la                     \
	$(libunwind_libadd)

### target PowerPC:
# The list of local files that go to Power 64 and 32:
libunwind_la_SOURCES_ppc =                     \
	ppc/Lget_proc_info.c                   \
	ppc/Lget_save_loc.c                    \
	ppc/Linit_local.c                      \
	ppc/Linit_remote.c                     \
	ppc/Lis_signal_frame.c

# The list of generic files that go to Power 64 and 32:
libunwind_ppc_la_SOURCES_ppc_generic =         \
	ppc/Gget_proc_info.c                   \
	ppc/Gget_save_loc.c                    \
	ppc/Ginit_local.c                      \
	ppc/Ginit_remote.c                     \
	ppc/Gis_signal_frame.c


# The list of files that go both into libunwind and libunwind-ppc32:
noinst_HEADERS +=                              \
	ppc32/init.h                           \
	ppc32/unwind_i.h                       \
	ppc32/ucontext_i.h
libunwind_la_SOURCES_ppc32_common =            \
	$(libunwind_la_SOURCES_common)         \
	ppc32/get_func_addr.c                  \
	ppc32/is_fpreg.c                       \
	ppc32/regname.c 

if ARCH_PPC32
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_ppc32_common)   \
	$(libunwind_la_SOURCES_local)          \
	$(libunwind_la_SOURCES_ppc)            \
	ppc32/Lapply_reg_state.c               \
	ppc32/Lcreate_addr_space.c             \
	ppc32/Lglobal.c                        \
	ppc32/Linit.c                          \
	ppc32/Lregs.c                          \
	ppc32/Lreg_states_iterate.c            \
	ppc32/Lresume.c                        \
	ppc32/Lstep.c

libunwind_setjmp_la_SOURCES +=                 \
	ppc/longjmp.S                          \
	ppc/siglongjmp.S
endif # ARCH_PPC32

# The list of files that go into libunwind-ppc32:
libunwind_ppc32_la_SOURCES =                   \
	$(libunwind_la_SOURCES_ppc32_common)   \
	$(libunwind_la_SOURCES_generic)        \
	$(libunwind_ppc_la_SOURCES_ppc_generic)\
	ppc32/Gapply_reg_state.c               \
	ppc32/Gcreate_addr_space.c             \
	ppc32/Gglobal.c                        \
	ppc32/Ginit.c                          \
	ppc32/Gregs.c                          \
	ppc32/Greg_states_iterate.c            \
	ppc32/Gresume.c                        \
	ppc32/Gstep.c
libunwind_ppc32_la_LDFLAGS =                   \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_ppc32_la_LIBADD =                    \
	libunwind-dwarf-generic.la             \
	libunwind-elf32.la                     \
	$(libunwind_libadd)

# The list of files that go both into libunwind and libunwind-ppc64:
noinst_HEADERS +=                              \
	ppc64/init.h                           \
	ppc64/unwind_i.h                       \
	ppc64/ucontext_i.h
libunwind_la_SOURCES_ppc64_common =            \
	$(libunwind_la_SOURCES_common)         \
        ppc64/get_func_addr.c                  \
        ppc64/is_fpreg.c                       \
        ppc64/regname.c

if ARCH_PPC64
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_ppc64_common)   \
	$(libunwind_la_SOURCES_local)          \
	$(libunwind_la_SOURCES_ppc)            \
	ppc64/Lapply_reg_state.c               \
	ppc64/Lcreate_addr_space.c             \
	ppc64/Lglobal.c                        \
	ppc64/Linit.c                          \
	ppc64/Lregs.c                          \
	ppc64/Lreg_states_iterate.c            \
	ppc64/Lresume.c                        \
	ppc64/Lstep.c
libunwind_setjmp_la_SOURCES +=                 \
	ppc/longjmp.S                          \
	ppc/siglongjmp.S
endif # ARCH_PPC64

# The list of files that go into libunwind-ppc64:
libunwind_ppc64_la_SOURCES =                   \
	$(libunwind_la_SOURCES_ppc64_common)   \
	$(libunwind_la_SOURCES_generic)        \
	$(libunwind_ppc_la_SOURCES_ppc_generic)\
	ppc64/Gapply_reg_state.c               \
	ppc64/Gcreate_addr_space.c             \
	ppc64/Gglobal.c                        \
	ppc64/Ginit.c                          \
	ppc64/Gregs.c                          \
	ppc64/Greg_states_iterate.c            \
	ppc64/Gresume.c                        \
	ppc64/Gstep.c
libunwind_ppc64_la_LDFLAGS =                   \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_ppc64_la_LIBADD =                    \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target RISC-V:
# The list of files that go info libunwind and libunwind-riscv:
noinst_HEADERS +=                              \
	riscv/asm.h                            \
	riscv/init.h                           \
	riscv/offsets.h                        \
	riscv/unwind_i.h 
libunwind_la_SOURCES_riscv_common =            \
	$(libunwind_la_SOURCES_common)         \
	riscv/is_fpreg.c                       \
	riscv/regname.c

if ARCH_RISCV
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_riscv_common)   \
	$(libunwind_la_SOURCES_local)          \
	riscv/getcontext.S                     \
	riscv/Lapply_reg_state.c               \
	riscv/Lcreate_addr_space.c             \
	riscv/Lget_proc_info.c                 \
	riscv/Lget_save_loc.c                  \
	riscv/Lglobal.c                        \
	riscv/Linit.c                          \
	riscv/Linit_local.c                    \
	riscv/Linit_remote.c                   \
	riscv/Lis_signal_frame.c               \
	riscv/Lregs.c                          \
	riscv/Lreg_states_iterate.c            \
	riscv/Lresume.c                        \
	riscv/Lstep.c                          \
	riscv/setcontext.S

libunwind_setjmp_la_SOURCES +=                 \
	riscv/siglongjmp.S
endif # ARCH_RISCV

libunwind_riscv_la_SOURCES =                   \
	$(libunwind_la_SOURCES_riscv_common)   \
	$(libunwind_la_SOURCES_generic)        \
	riscv/Gapply_reg_state.c               \
	riscv/Gcreate_addr_space.c             \
	riscv/Gget_proc_info.c                 \
	riscv/Gget_save_loc.c                  \
	riscv/Gglobal.c                        \
	riscv/Ginit.c                          \
	riscv/Ginit_local.c                    \
	riscv/Ginit_remote.c                   \
	riscv/Gis_signal_frame.c               \
	riscv/Gregs.c                          \
	riscv/Greg_states_iterate.c            \
	riscv/Gresume.c                        \
	riscv/Gstep.c

libunwind_riscv_la_LDFLAGS =                   \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_riscv_la_LIBADD =                    \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target s390x:
# The list of files that go both into libunwind and libunwind-s390x:
noinst_HEADERS +=                              \
	s390x/init.h                           \
	s390x/unwind_i.h
libunwind_la_SOURCES_s390x_common =            \
	$(libunwind_la_SOURCES_common)         \
	s390x/is_fpreg.c                       \
	s390x/regname.c

if ARCH_S390X
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_s390x_common)   \
	$(libunwind_la_SOURCES_local)          \
	s390x/getcontext.S                     \
	s390x/Lapply_reg_state.c               \
	s390x/Lcreate_addr_space.c             \
	s390x/Lget_proc_info.c                 \
	s390x/Lget_save_loc.c                  \
	s390x/Lglobal.c                        \
	s390x/Linit.c                          \
	s390x/Linit_local.c                    \
	s390x/Linit_remote.c                   \
	s390x/Lis_signal_frame.c               \
	s390x/Lregs.c                          \
	s390x/Lreg_states_iterate.c            \
	s390x/Lresume.c                        \
	s390x/Lstep.c                          \
	s390x/setcontext.S
endif # ARCH_S390X

# The list of files that go into libunwind-s390x:
libunwind_s390x_la_SOURCES =                   \
	$(libunwind_la_SOURCES_s390x_common)   \
	$(libunwind_la_SOURCES_generic)        \
	s390x/Gapply_reg_state.c               \
	s390x/Gcreate_addr_space.c             \
	s390x/Gget_proc_info.c                 \
	s390x/Gget_save_loc.c                  \
	s390x/Gglobal.c                        \
	s390x/Ginit.c                          \
	s390x/Ginit_local.c                    \
	s390x/Ginit_remote.c                   \
	s390x/Gis_signal_frame.c               \
	s390x/Gregs.c                          \
	s390x/Greg_states_iterate.c            \
	s390x/Gresume.c                        \
	s390x/Gstep.c
libunwind_s390x_la_LDFLAGS =                   \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_s390x_la_LIBADD =                    \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

### target Sh:
# The list of files that go into libunwind and libunwind-sh:
noinst_HEADERS +=                              \
	sh/init.h                              \
	sh/offsets.h                           \
	sh/unwind_i.h
libunwind_la_SOURCES_sh_common =               \
	$(libunwind_la_SOURCES_common)         \
	sh/is_fpreg.c                          \
	sh/regname.c

if ARCH_SH
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_sh_common)      \
	$(libunwind_la_SOURCES_local)          \
	sh/Lapply_reg_state.c                  \
	sh/Lcreate_addr_space.c                \
	sh/Lget_proc_info.c                    \
	sh/Lget_save_loc.c                     \
	sh/Lglobal.c                           \
	sh/Linit.c                             \
	sh/Linit_local.c                       \
	sh/Linit_remote.c                      \
	sh/Lis_signal_frame.c                  \
	sh/Lregs.c                             \
	sh/Lreg_states_iterate.c               \
	sh/Lresume.c                           \
	sh/Lstep.c
libunwind_setjmp_la_SOURCES += sh/siglongjmp.S
endif # ARCH_SH

libunwind_sh_la_SOURCES =                      \
	$(libunwind_la_SOURCES_sh_common)      \
	$(libunwind_la_SOURCES_generic)        \
	sh/Gapply_reg_state.c                  \
	sh/Gcreate_addr_space.c                \
	sh/Gget_proc_info.c                    \
	sh/Gget_save_loc.c                     \
	sh/Gglobal.c                           \
	sh/Ginit.c                             \
	sh/Ginit_local.c                       \
	sh/Ginit_remote.c                      \
	sh/Gis_signal_frame.c                  \
	sh/Gregs.c                             \
	sh/Greg_states_iterate.c               \
	sh/Gresume.c                           \
	sh/Gstep.c
libunwind_sh_la_LDFLAGS =                      \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_sh_la_LIBADD =                       \
	libunwind-dwarf-generic.la             \
	libunwind-elf32.la                     \
	$(libunwind_libadd)

### target x86:
# The list of files that go both into libunwind and libunwind-x86:
noinst_HEADERS +=                              \
	x86/init.h                             \
	x86/offsets.h                          \
	x86/unwind_i.h
libunwind_la_SOURCES_x86_common =              \
	$(libunwind_la_SOURCES_common)         \
	x86/is_fpreg.c                         \
	x86/regname.c

if ARCH_X86
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_x86_common)     \
	$(libunwind_la_SOURCES_x86_os_local)   \
	$(libunwind_la_SOURCES_local)          \
	x86/Lapply_reg_state.c                 \
	x86/Lcreate_addr_space.c               \
	x86/Lget_proc_info.c                   \
	x86/Lget_save_loc.c                    \
	x86/Lglobal.c                          \
	x86/Linit.c                            \
	x86/Linit_local.c                      \
	x86/Linit_remote.c                     \
	x86/Lregs.c                            \
	x86/Lreg_states_iterate.c              \
	x86/Lresume.c                          \
	x86/Lstep.c

libunwind_setjmp_la_SOURCES +=                 \
	x86/longjmp.S                          \
	x86/siglongjmp.S
endif # ARCH_X86

# The list of files that go into libunwind-x86:
libunwind_x86_la_SOURCES =                     \
	$(libunwind_la_SOURCES_x86_common)     \
	$(libunwind_la_SOURCES_x86_os)         \
	$(libunwind_la_SOURCES_generic)        \
	x86/Gapply_reg_state.c                 \
	x86/Gcreate_addr_space.c               \
	x86/Gget_proc_info.c                   \
	x86/Gget_save_loc.c                    \
	x86/Gglobal.c                          \
	x86/Ginit.c                            \
	x86/Ginit_local.c                      \
	x86/Ginit_remote.c                     \
	x86/Gregs.c                            \
	x86/Greg_states_iterate.c              \
	x86/Gresume.c                          \
	x86/Gstep.c
libunwind_x86_la_LDFLAGS =                     \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_x86_la_LIBADD =                      \
	libunwind-dwarf-generic.la             \
	libunwind-elf32.la                     \
	$(libunwind_libadd)

### target x86_64:
# The list of files that go both into libunwind and libunwind-x86_64:
noinst_HEADERS +=                              \
	x86_64/init.h                          \
	x86_64/ucontext_i.h                    \
	x86_64/unwind_i.h
libunwind_la_SOURCES_x86_64_common =           \
	$(libunwind_la_SOURCES_common)         \
	x86_64/is_fpreg.c                      \
	x86_64/regname.c

if ARCH_X86_64
# The list of files that go into libunwind:
libunwind_la_SOURCES =                         \
	$(libunwind_la_SOURCES_x86_64_common)  \
	$(libunwind_la_SOURCES_x86_64_os_local)\
	$(libunwind_la_SOURCES_local)          \
	x86_64/getcontext.S                    \
	x86_64/Lapply_reg_state.c              \
	x86_64/Lcreate_addr_space.c            \
	x86_64/Lget_proc_info.c                \
	x86_64/Lget_save_loc.c                 \
	x86_64/Lglobal.c                       \
	x86_64/Linit.c                         \
	x86_64/Linit_local.c                   \
	x86_64/Linit_remote.c                  \
	x86_64/Lregs.c                         \
	x86_64/Lreg_states_iterate.c           \
	x86_64/Lresume.c                       \
	x86_64/Lstash_frame.c                  \
	x86_64/Lstep.c                         \
	x86_64/Ltrace.c                        \
	x86_64/setcontext.S

libunwind_setjmp_la_SOURCES +=                 \
	x86_64/longjmp.S                       \
	x86_64/siglongjmp.S
endif # ARCH_X86_64

# The list of files that go into libunwind-x86_64:
libunwind_x86_64_la_SOURCES =                  \
	$(libunwind_la_SOURCES_x86_64_common)  \
	$(libunwind_la_SOURCES_x86_64_os)      \
	$(libunwind_la_SOURCES_generic)        \
	x86_64/Gapply_reg_state.c              \
	x86_64/Gcreate_addr_space.c            \
	x86_64/Gget_proc_info.c                \
	x86_64/Gget_save_loc.c                 \
	x86_64/Gglobal.c                       \
	x86_64/Ginit.c                         \
	x86_64/Ginit_local.c                   \
	x86_64/Ginit_remote.c                  \
	x86_64/Gregs.c                         \
	x86_64/Greg_states_iterate.c           \
	x86_64/Gresume.c                       \
	x86_64/Gstash_frame.c                  \
	x86_64/Gstep.c                         \
	x86_64/Gtrace.c
libunwind_x86_64_la_LDFLAGS =                  \
	$(COMMON_SO_LDFLAGS)                   \
	-version-info $(SOVERSION)
libunwind_x86_64_la_LIBADD =                   \
	libunwind-dwarf-generic.la             \
	libunwind-elf64.la                     \
	$(libunwind_libadd)

if REMOTE_ONLY
install-exec-hook:
#	Nothing to do here....
else
#
# This is not ideal, but I know of no other way to install an
# alias for a library.  For the shared version, we have to do
# a file check before creating the link, because it isn't going
# to be there if the user configured with --disable-shared.
#
install-exec-hook:
	if test -f $(DESTDIR)$(libdir)/libunwind-$(arch).a; then \
		cd $(DESTDIR)$(libdir) && $(LN_S) -f libunwind-$(arch).a libunwind-generic.a; \
	fi
	if test -f $(DESTDIR)$(libdir)/libunwind-$(arch).so; then \
		cd $(DESTDIR)$(libdir) && $(LN_S) -f libunwind-$(arch).so \
		libunwind-generic.so; \
	fi
endif

#
# Don't link with standard libraries, because those may mention
# libunwind already.
#
libunwind_la_LDFLAGS =	$(COMMON_SO_LDFLAGS) -XCClinker -nostdlib \
			$(LDFLAGS_STATIC_LIBCXA) -version-info $(SOVERSION)
libunwind_la_LIBADD  += -lc $(LIBCRTS)
libunwind_la_LIBADD += $(LIBLZMA) $(LIBZ)

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/include/tdep-$(arch) -I.
AM_CCASFLAGS = $(AM_CPPFLAGS)
noinst_HEADERS += unwind/unwind-internal.h

EXTRA_DIST = $(libunwind_la_EXTRAS_ia64)

MAINTAINERCLEANFILES = Makefile.in

# The -version-info flag accepts an argument of the form
# `current[:revision[:age]]'. So, passing `-version-info 3:12:1' sets
# current to 3, revision to 12, and age to 1.

# If either revision or age are omitted, they default to 0. Also note
# that age must be less than or equal to the current interface number.

# Here are a set of rules to help you update your library version
# information:

#    1. Start with version information of `0:0:0' for each libtool
#       library.

#    2. Update the version information only immediately before a public
#       release of your software. More frequent updates are unnecessary,
#       and only guarantee that the current interface number gets larger
#       faster.

#    3. If the library source code has changed at all since the last
#       update, then increment revision (`c:r:a' becomes `c:r+1:a').

#    4. If any interfaces have been added, removed, or changed since the
#       last update, increment current, and set revision to 0.

#    5. If any interfaces have been added since the last public release,
#       then increment age.

#    6. If any interfaces have been removed since the last public
#       release, then set age to 0.
