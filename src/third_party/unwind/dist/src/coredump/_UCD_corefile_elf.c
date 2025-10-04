/**
 * Support functions for ELF corefiles
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

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>


/**
 * Read an ELF segment into an allocated memory buffer.
 * @param[in]  ui	    the unwind-coredump context
 * @param[in]  phdr         pointer to the PHDR of the segment to load
 * @param[out] segment      pointer to the segment loaded
 * @param[out] segment_size size of the @segment in bytes
 *
 * Allocates an appropriately-sized buffer to contain the segment of the
 * coredump described by @phdr and reads it in from the core file.
 *
 * The caller is responsible for freeing the allocated segment memory.
 *
 * @returns UNW_SUCCESS on success, something else otherwise.
 */
HIDDEN int
_UCD_elf_read_segment(struct UCD_info *ui, coredump_phdr_t *phdr, uint8_t **segment, size_t *segment_size)
{
  int ret = -UNW_EUNSPEC;
  if (lseek(ui->coredump_fd, phdr->p_offset, SEEK_SET) != (off_t)phdr->p_offset)
  {
    Debug(0, "errno %d setting offset to %lu in '%s': %s\n",
    	  errno, phdr->p_offset, ui->coredump_filename, strerror(errno));
    return ret;
  }

  *segment_size = phdr->p_filesz;
  *segment = malloc(*segment_size);
  if (*segment == NULL)
  {
    Debug(0, "error %zu bytes of memory for segment\n", *segment_size);
    return ret;
  }

  if (read(ui->coredump_fd, *segment, *segment_size) != (ssize_t)*segment_size)
  {
    Debug(0, "errno %d reading %zu bytes from '%s': %s\n",
    	  errno, *segment_size, ui->coredump_filename, strerror(errno));
    return ret;
  }

  ret = UNW_ESUCCESS;
  return ret;
}


/**
 * Parse a PT_NOTE segment into zero or more notes and visit each one
 * @param[in]  segment      pointer to the PT_NOTE segment
 * @param[in]  segment_size size of @p segment in bytes
 * @param[in]  visit        callback to process to the notes
 * @param[in]  arg          context to forward to the callback
 *
 * One PT_NOTE segment might contain many variable-length notes.  Parsing them
 * out is just a matter of calculating the size of each note from the size
 * fields contained in the (fixed-size) note header and adjusting for 4-byte
 * alignment.
 *
 * For each note found the @p visit callback will be invoked.  If the callback
 * returns anything but UNW_ESUCCESS, traversal of the notes will be terminated
 * and processing will return immediately, passing the return code through.
 *
 * @returns UNW_SUCCESS on success or the return value from @p visit otherwise.
 */
HIDDEN int
_UCD_elf_visit_notes(uint8_t *segment, size_t segment_size, note_visitor_t visit, void *arg)
{
  int ret = UNW_ESUCCESS;
  size_t parsed_size = 0;
  while (parsed_size < segment_size)
  {
    /*
     * Note that Elf32_Nhdr and Elf64_Nhdr are identical, so it doesn't matter which
     * structure is chosen here. I chose the one with the larger number because
     * bigger is better.
     */
    Elf64_Nhdr *note = (Elf64_Nhdr *)(segment + parsed_size);
    unsigned header_size = sizeof(Elf64_Nhdr);
    unsigned name_size = UNW_ALIGN(note->n_namesz, 4);
    unsigned desc_size = UNW_ALIGN(note->n_descsz, 4);
    unsigned note_size = header_size + name_size + desc_size;
    char *name = (char *)(note) + header_size;
    uint8_t *desc = (uint8_t *)(note) + header_size + name_size;

    ret = visit(note->n_namesz, note->n_descsz, note->n_type, name, desc, arg);
    if (ret != UNW_ESUCCESS)
    {
      break;
    }

    parsed_size += note_size;
  }
  return ret;
}

