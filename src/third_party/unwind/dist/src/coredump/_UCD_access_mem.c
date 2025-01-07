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

#include "_UCD_lib.h"
#include "_UCD_internal.h"
#include "ucd_file_table.h"

int
_UCD_access_mem (unw_addr_space_t  as UNUSED,
                 unw_word_t        addr,
                 unw_word_t       *val,
                 int               write,
                 void             *arg)
{
  if (write)
    {
      Debug (0, "write is not supported\n");
      return -UNW_EINVAL;
    }

  struct UCD_info *ui = arg;

  unw_word_t addr_last = addr + sizeof (*val) - 1;

  unsigned i;

  for (i = 0; i < ui->phdrs_count; i++)
    {
      coredump_phdr_t *phdr = &ui->phdrs[i];

      /* First check the (in-memory) backup file image. */
      if (phdr->p_backing_file_index != ucd_file_no_index)
        {
          ucd_file_t *ucd_file = ucd_file_table_at (&ui->ucd_file_table, phdr->p_backing_file_index);

          if (ucd_file == NULL)
            {
              Debug (0, "invalid backing file index for phdr[%d]\n", i);
              return -UNW_EINVAL;
            }

          off_t image_offset = addr - phdr->p_vaddr;

          if (phdr->p_vaddr <= addr && addr_last < phdr->p_vaddr + ucd_file->size)
            {
              memcpy (val, ucd_file->image + image_offset, sizeof (*val));
              Debug (16, "%#010llx <- [addr:%#010llx file:%s]\n",
                     (unsigned long long) (*val),
                     (unsigned long long)image_offset,
                     ucd_file->filename);
              return UNW_ESUCCESS;
            }
        }

      /* Next, check the on-disk corefile. */
      if (phdr->p_vaddr <= addr && addr_last < phdr->p_vaddr + phdr->p_memsz)
        {
          off_t fileofs = phdr->p_offset + (addr - phdr->p_vaddr);

          if (lseek (ui->coredump_fd, fileofs, SEEK_SET) != fileofs)
            {
              Debug (0, "error %d in lseek(\"%s\", %lld): %s\n",
                     errno,  ui->coredump_filename, (long long)fileofs, strerror (errno));
              return -UNW_EINVAL;
            }

          if (read (ui->coredump_fd, val, sizeof (*val)) != sizeof (*val))
            {
              Debug (0, "error %d in read(\"%s\", %lld): %s\n",
                     errno,  ui->coredump_filename, (long long)sizeof (*val), strerror (errno));
              return -UNW_EINVAL;
            }

          Debug (16, "0x%llx <- [addr:0x%llx fileofs:0x%llx file:%s]\n",
                 (unsigned long long) (*val),
                 (unsigned long long)addr,
                 (unsigned long long)fileofs,
                 ui->coredump_filename);
          return UNW_ESUCCESS;
        }
    }

  Debug (0, "addr %#010llx is unmapped\n", (unsigned long long)addr);
  return -UNW_EINVAL;
}
