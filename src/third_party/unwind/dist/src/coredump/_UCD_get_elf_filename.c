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
#include "_UCD_lib.h"
#include "_UCD_internal.h"

#if defined(HAVE_ELF_H)
# include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
# include <sys/elf.h>
#endif

static off_t
_get_text_offset (uint8_t *image)
{
  off_t offset = 0;
  typedef union
  {
    Elf32_Ehdr h32;
    Elf64_Ehdr h64;
  } elf_header_t;

  elf_header_t *elf_header = (elf_header_t *)image;
  bool _64bits     = (elf_header->h32.e_ident[EI_CLASS] == ELFCLASS64);
  off_t e_phofs    = _64bits ? elf_header->h64.e_phoff  : elf_header->h32.e_phoff;
  unsigned e_phnum = _64bits ? elf_header->h64.e_phnum  : elf_header->h32.e_phnum;

  for (unsigned i = 0; i < e_phnum; ++i)
    {
      if (_64bits)
        {
          Elf64_Phdr *phdr = (Elf64_Phdr *) (image + e_phofs);

          if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X) == PF_X)
            {
              offset = phdr[i].p_offset;
              break;
            }
        }
      else
        {
          Elf32_Phdr *phdr = (Elf32_Phdr *) (image + e_phofs);

          if ((phdr[i].p_flags & PF_X) == PF_X)
            {
              offset = phdr[i].p_offset;
              break;
            }
        }
    }

  Debug (4, "returning offset %ld\n", (long)offset);
  return offset;
}

static int
elf_w (CD_get_elf_filename) (struct UCD_info *ui, unw_addr_space_t as, unw_word_t ip,
                             char *buf, size_t buf_len, unw_word_t *offp)
{
  int ret = UNW_ESUCCESS;

  /* Used to be tdep_get_elf_image() in ptrace unwinding code */
  coredump_phdr_t *cphdr = _UCD_get_elf_image (ui, ip);
  if (!cphdr)
    {
      Debug (1, "returns error: _UCD_get_elf_image failed\n");
      return -UNW_ENOINFO;
    }

  const ucd_file_t *ucd_file = ucd_file_table_at(&ui->ucd_file_table, cphdr->p_backing_file_index);
  if (!ucd_file)
    {
      Debug (1, "backing_fie_index:%d ucd_file_table_at failed\n", cphdr->p_backing_file_index);
      return -UNW_ENOINFO;
    }

  if (buf)
    {
      strncpy(buf, ucd_file->filename, buf_len);
      buf[buf_len - 1] = '\0';
      if (strlen(ucd_file->filename) >= buf_len)
        ret = -UNW_ENOMEM;
    }

  /* Adjust IP to be relative to start of the .text section of the ELF file */
  if (offp)
    *offp = ip - cphdr->p_vaddr + _get_text_offset (ui->edi.ei.image);

  return ret;
}

int
_UCD_get_elf_filename (unw_addr_space_t as, unw_word_t ip,
                   char *buf, size_t buf_len, unw_word_t *offp, void *arg)
{
  struct UCD_info *ui = arg;
#if UNW_ELF_CLASS == UNW_ELFCLASS64
  return _Uelf64_CD_get_elf_filename (ui, as, ip, buf, buf_len, offp);
#elif UNW_ELF_CLASS == UNW_ELFCLASS32
  return _Uelf32_CD_get_elf_filename (ui, as, ip, buf, buf_len, offp);
#else
  return -UNW_ENOINFO;
#endif
}
