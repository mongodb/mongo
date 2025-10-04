/* libunwind - a platform-independent unwind library

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#ifndef _UCD_internal_h
#define _UCD_internal_h

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_PROCFS_H
#include <sys/procfs.h> /* struct elf_prstatus */
#endif
#ifdef HAVE_ASM_PTRACE_H
#include <asm/ptrace.h> /* struct user_regs_struct on s390x */
#endif
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <libunwind-coredump.h>

#include "libunwind_i.h"
#include "ucd_file_table.h"


#if SIZEOF_OFF_T == 4
typedef uint32_t uoff_t;
#elif SIZEOF_OFF_T == 8
typedef uint64_t uoff_t;
#else
# error Unknown size of off_t!
#endif


/* Similar to ELF phdrs. p_paddr element is absent,
 * since it's always 0 in coredumps.
 */
struct coredump_phdr
  {
    uint32_t         p_type;
    uint32_t         p_flags;
    uoff_t           p_offset;
    uoff_t           p_vaddr;
    uoff_t           p_filesz;
    uoff_t           p_memsz;
    uoff_t           p_align;
    ucd_file_index_t p_backing_file_index;
  };

typedef struct coredump_phdr coredump_phdr_t;

#if defined(HAVE_STRUCT_ELF_PRSTATUS)
typedef struct elf_prstatus UCD_proc_status_t;
#elif defined(HAVE_STRUCT_PRSTATUS)
typedef struct prstatus UCD_proc_status_t;
#elif defined(HAVE_PROCFS_STATUS)
typedef struct {
    procfs_status thread;
    procfs_greg   greg;
    procfs_fpreg  fpreg;
} UCD_proc_status_t;
#else
# error UCD_proc_status_t undefined
#endif

struct UCD_thread_info
  {
    UCD_proc_status_t  prstatus;
#ifdef HAVE_ELF_FPREGSET_T
    elf_fpregset_t     fpregset;
#endif
  };

struct UCD_info
  {
    int                     big_endian;        /* bool */
    int                     coredump_fd;
    char                   *coredump_filename; /* for error meesages only */
    coredump_phdr_t        *phdrs;             /* array, allocated */
    unsigned                phdrs_count;
    ucd_file_table_t        ucd_file_table;
    void                   *note_phdr;         /* allocated or NULL */
    UCD_proc_status_t      *prstatus;          /* points inside note_phdr */
#ifdef HAVE_ELF_FPREGSET_T
    elf_fpregset_t         *fpregset;
#endif
    int                     n_threads;
    struct UCD_thread_info *threads;
    struct elf_dyn_info     edi;
  };


typedef int (*note_visitor_t)(uint32_t, uint32_t, uint32_t, char *, uint8_t *, void *);


coredump_phdr_t * _UCD_get_elf_image(struct UCD_info *ui, unw_word_t ip);

int _UCD_elf_read_segment(struct UCD_info *ui, coredump_phdr_t *phdr, uint8_t **segment, size_t *segment_size);
int _UCD_elf_visit_notes(uint8_t *segment, size_t segment_size, note_visitor_t visit, void *arg);
int _UCD_get_threadinfo(struct UCD_info *ui, coredump_phdr_t *phdrs, unsigned phdr_size);
int _UCD_get_mapinfo(struct UCD_info *ui, coredump_phdr_t *phdrs, unsigned phdr_size);


#endif
