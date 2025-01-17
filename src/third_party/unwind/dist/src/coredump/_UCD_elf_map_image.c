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

#if defined(HAVE_ELF_H)
# include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
# include <sys/elf.h>
#endif

#include "_UCD_lib.h"
#include "_UCD_internal.h"
#include "ucd_file_table.h"

static coredump_phdr_t *
CD_elf_map_image(struct UCD_info *ui, coredump_phdr_t *phdr)
{
  struct elf_image *ei = &ui->edi.ei;

  if (phdr->p_backing_file_index == ucd_file_no_index)
    {
      /* Note: coredump file contains only phdr->p_filesz bytes.
       * We want to map bigger area (phdr->p_memsz bytes) to make sure
       * these pages are allocated, but non-accessible.
       */
      /* addr, length, prot, flags, fd, fd_offset */
      ei->image = mi_mmap(NULL, phdr->p_memsz, PROT_READ, MAP_PRIVATE, ui->coredump_fd, phdr->p_offset);
      if (ei->image == MAP_FAILED)
        {
          Debug(0, "error in mmap()\n");
          ei->image = NULL;
          return NULL;
        }
      ei->size = phdr->p_filesz;
      size_t remainder_len = phdr->p_memsz - phdr->p_filesz;
      if (remainder_len > 0)
        {
          void *remainder_base = (char*) ei->image + phdr->p_filesz;
          mi_munmap(remainder_base, remainder_len);
        }
    } else {
      ucd_file_t *ucd_file =  ucd_file_table_at(&ui->ucd_file_table, phdr->p_backing_file_index);
      if (ucd_file == NULL)
        {
          Debug(0, "error retrieving backing file for index %d\n", phdr->p_backing_file_index);
          return NULL;
        }
      /* addr, length, prot, flags, fd, fd_offset */
      ei->image = ucd_file_map (ucd_file);
      if (ei->image == NULL)
        {
          return NULL;
        }
      ei->size = ucd_file->size;
    }

  /* Check ELF header for sanity */
  if (!elf_w(valid_object)(ei))
    {
      mi_munmap(ei->image, ei->size);
      ei->image = NULL;
      ei->size = 0;
      return NULL;
    }

  return phdr;
}

HIDDEN coredump_phdr_t *
_UCD_get_elf_image(struct UCD_info *ui, unw_word_t ip)
{
  unsigned i;
  for (i = 0; i < ui->phdrs_count; i++)
    {
      coredump_phdr_t *phdr = &ui->phdrs[i];
      if (phdr->p_vaddr <= ip && ip < phdr->p_vaddr + phdr->p_memsz)
        {
          phdr = CD_elf_map_image(ui, phdr);
          return phdr;
        }
    }
  return NULL;
}
