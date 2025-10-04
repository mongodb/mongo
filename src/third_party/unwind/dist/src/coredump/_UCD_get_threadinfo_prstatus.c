/**
 * Extract threadinfo from a coredump (supported targets)
 */
/*
 This file is part of libunwind.

 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
*/
#include "_UCD_internal.h"

#if defined(HAVE_ELF_H)
# include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
# include <sys/elf.h>
#endif


/**
 * Accumulate a count of the number of thread notes
 *
 * This _UCD_elf_visit_notes() callback just increments a count for each
 * thread status note seen.
 */
static int
_count_thread_notes(uint32_t  n_namesz UNUSED,
                    uint32_t  n_descsz UNUSED,
                    uint32_t  n_type,
                    char     *name UNUSED,
                    uint8_t  *desc UNUSED,
                    void     *arg)
{
  size_t *thread_count = (size_t *)arg;
#if defined(HAVE_PROCFS_STATUS)
  if (0 == strcmp(name, QNX_NOTE_NAME) && n_type == QNT_CORE_STATUS)
#else
  if (n_type == NT_PRSTATUS)
#endif /* defined(HAVE_PROCFS_STATUS) */
  {
    ++*thread_count;
  }
  return UNW_ESUCCESS;
}


/**
 * Save a thread note to the unwind-coredump context
 *
 * This _UCD_elf_visit_notes() callback just copies the actual data structure(s)
 * from any notes seen into an array of such structures and increments the count.
 *
 * Some targets have multiple notes for each thread that MUST always come in the
 * right order (eg. NT_PRSTATUS followed by NT_FPREGSET for Linux and the BSDs).
 * The count only gets incremented on the first note for the thread so the
 * remaining notes need to have their zero-based index adjusted.
 */
static int
_save_thread_notes(uint32_t  n_namesz UNUSED,
                   uint32_t  n_descsz UNUSED,
                   uint32_t  n_type,
                   char     *name UNUSED,
                   uint8_t  *desc,
                   void     *arg)
{
  struct UCD_info *ui = (struct UCD_info *)arg;
#if defined(HAVE_PROCFS_STATUS)
  if (0 == strcmp(name, QNX_NOTE_NAME))
    {
      switch (n_type)
	    {
        case QNT_CORE_STATUS:
          ++ui->n_threads;
          memcpy(&ui->threads[ui->n_threads-1].prstatus.thread, desc, (size_t)n_descsz);
          break;
        case QNT_CORE_GREG:
          memcpy(&ui->threads[ui->n_threads-1].prstatus.greg, desc, (size_t)n_descsz);
          break;
        case QNT_CORE_FPREG:
          memcpy(&ui->threads[ui->n_threads-1].prstatus.fpreg, desc, (size_t)n_descsz);
          break;
        default:
          break;
		}
    }
#else
  if (n_type == NT_PRSTATUS)
    {
      memcpy(&ui->threads[ui->n_threads].prstatus, desc, sizeof(UCD_proc_status_t));
      ++ui->n_threads;
    }
#endif
#ifdef HAVE_ELF_FPREGSET_T
  if (n_type == NT_FPREGSET)
    {
      memcpy(&ui->threads[ui->n_threads-1].fpregset, desc, sizeof(elf_fpregset_t));
    }
#endif
  return UNW_ESUCCESS;
}


/**
 * Get thread info from core file
 *
 * On Linux threads are emulated by cloned processes sharing an address space
 * and the process information is described by a note in the core file of type
 * NT_PRSTATUS.  In fact, on Linux, the state of a thread is described by a
 * CPU-dependent group of notes but right now we're only going to care about the
 * one process-status note.  This statement is also true for the BSDs.
 *
 * Depending on how the core file is created, there may be one PT_NOTE segment
 * with multiple NT_PRSTATUS notes in it, or multiple PT_NOTE segments.  Just to
 * be safe, it's better to assume there are multiple PT_NOTE segments each with
 * multiple NT_PRSTATUS notes, as that covers all the cases.
 */
int
_UCD_get_threadinfo(struct UCD_info *ui, coredump_phdr_t *phdrs, unsigned phdr_size)
{
  int ret = -UNW_ENOINFO;

  for (unsigned i = 0; i < phdr_size; ++i)
    {
      Debug(8, "phdr[%03d]: type:%d", i, phdrs[i].p_type);
      if (phdrs[i].p_type == PT_NOTE)
        {
          size_t thread_count = 0;
          uint8_t *segment;
          size_t segment_size;
          ret = _UCD_elf_read_segment(ui, &phdrs[i], &segment, &segment_size);
          if (ret == UNW_ESUCCESS)
            {
              _UCD_elf_visit_notes(segment, segment_size, _count_thread_notes, &thread_count);
              Debug(2, "found %zu threads\n", thread_count);

              size_t new_size = sizeof(struct UCD_thread_info) * (ui->n_threads + thread_count);
              ui->threads = realloc(ui->threads, new_size);
              if (ui->threads == NULL)
                {
                  Debug(0, "error allocating %zu bytes of memory \n", new_size);
                  free(segment);
                  return -UNW_EUNSPEC;
                }
              _UCD_elf_visit_notes(segment, segment_size, _save_thread_notes, ui);

              free(segment);
            }
        }
    }

  return ret;
}
