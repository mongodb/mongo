/**
 * Public interface for the QNX Neutrino remote unwinding library.
 *
 * This library provides helper routines to make it possible to use libunwind
 * via the QNX Neutrino procfs.
 */
/*
 * Copyright 2020, 2022 QNX Blackberry Limited.
 *
 * This file is part of libunwind.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LIBUNWIND_NTO_H
#define LIBUNWIND_NTO_H

#include <libunwind.h>
#include <pthread.h>

#if defined(__cplusplus)
extern "C" {
#endif

static const pthread_t LUT_ALL_THREADS = -1;

/**
 * Helper routines to make it easy to unwind using devctl() on unw_nto.
 */
extern void *unw_nto_create(pid_t, pthread_t);
extern void unw_nto_destroy(void *);

extern int unw_nto_find_proc_info(unw_addr_space_t, unw_word_t, unw_proc_info_t *, int, void *);
extern void unw_nto_put_unwind_info(unw_addr_space_t, unw_proc_info_t *, void *);
extern int unw_nto_get_dyn_info_list_addr(unw_addr_space_t, unw_word_t *, void *);
extern int unw_nto_access_mem(unw_addr_space_t, unw_word_t, unw_word_t *, int, void *);
extern int unw_nto_access_reg(unw_addr_space_t, unw_regnum_t, unw_word_t *, int, void *);
extern int unw_nto_access_fpreg(unw_addr_space_t, unw_regnum_t, unw_fpreg_t *, int, void *);
extern int unw_nto_get_proc_name(unw_addr_space_t, unw_word_t, char *, size_t, unw_word_t *, void *);
extern int unw_nto_get_proc_ip_range (unw_addr_space_t as, unw_word_t ip, unw_word_t *start, unw_word_t *end, void *);
extern int unw_nto_get_elf_filename(unw_addr_space_t, unw_word_t, char *, size_t, unw_word_t *, void *);
extern int unw_nto_resume(unw_addr_space_t, unw_cursor_t *, void *);

/**
 * A handy pre-defined accessor with all of the above.
 */
extern unw_accessors_t unw_nto_accessors;

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* LIBUNWIND_NTO_H */
