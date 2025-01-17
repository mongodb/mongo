/**
 * Extract filemap info from a coredump (Linux and similar)
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


/**
 * The format of the NT_FILE note is not well documented, but it goes something
 * like this.
 *
 * The note has a header containing the @i count of the number of file maps, plus a
 * value of the size of the offset field in each map. Since we don;t care about
 * the offset field in a core file, there is no further information available on
 * exactly what the means.
 *
 * Following the header are @count mapinfo structures. The mapinfo structure consists of
 * a start address, and end address, and some wacky offset thing.  The start and
 * end address are the virtual addresses of a LOAD segment that was mapped from
 * the named file.
 *
 * Following the array of mapinfo structures is a block of null-terminated C strings
 * containing the mapped file names.  They are ordered correspondingly to each
 * entry in the map structure array.
 */
struct core_nt_file_hdr_s
{
  unsigned long count;
  unsigned long pagesz;
};
typedef struct core_nt_file_hdr_s core_nt_file_hdr_t;

struct core_nt_file_entry_s
{
  unsigned long start;
  unsigned long end;
  unsigned long offset;
};
typedef struct core_nt_file_entry_s core_nt_file_entry_t;


static const char   deleted[] = "(deleted)";
static const size_t deleted_len = sizeof (deleted);
static const size_t mapinfo_offset = sizeof (core_nt_file_hdr_t);


/**
 * Handle the CORE/NT_FILE note type.
 * @param[in] desc  The note-specific data
 * @param[in] arg   The user-supplied callback argument
 *
 * The CORE/NT_FILE note type contains a list of start/end virtual addresses
 * within the core file and an associated filename. The purpose is to map
 * various segments loaded into memory from ELF files with the ELF file from
 * which those segments were loaded.
 *
 * This function links the file names mapped in the CORE/NT_FILE note with
 * the program headers in the core file through the UCD_info file table.
 *
 * Any file names that end in the string "(deleted)" are ignored.
 */
static int
_handle_nt_file_note (uint8_t *desc, void *arg)
{
  struct UCD_info *ui = (struct UCD_info *)arg;
  core_nt_file_hdr_t *mapinfo = (core_nt_file_hdr_t *)desc;
  core_nt_file_entry_t *maps = (core_nt_file_entry_t *) (desc + mapinfo_offset);
  char *strings = (char *) (desc + mapinfo_offset + sizeof (core_nt_file_entry_t) * mapinfo->count);

  for (unsigned long i = 0; i < mapinfo->count; ++i)
    {
      size_t len = strlen (strings);

      for (unsigned p = 0; p < ui->phdrs_count; ++p)
        {
          if (ui->phdrs[p].p_type == PT_LOAD
              && maps[i].start >= ui->phdrs[p].p_vaddr
              && maps[i].end <= ui->phdrs[p].p_vaddr + ui->phdrs[p].p_memsz)
            {
              if (len > deleted_len && memcmp (strings + len - deleted_len, deleted, deleted_len))
                {
                  ui->phdrs[p].p_backing_file_index = ucd_file_table_insert (&ui->ucd_file_table, strings);
                  Debug (3, "adding '%s' at index %d\n", strings, ui->phdrs[p].p_backing_file_index);
                }

              break;
            }
        }

      strings += (len + 1);
    }

  return UNW_ESUCCESS;
}

/**
 * Callback to handle notes.
 * @param[in]  n_namesz size of name data
 * @param[in]  n_descsz size of desc data
 * @param[in]  n_type type of note
 * @param[in]  name zero-terminated string, n_namesz bytes plus alignment padding
 * @param[in]  desc note-specific data, n_descsz bytes plus alignment padding
 * @param[in]  arg user-supplied callback argument
 *
 * Add additional note types here for fun and frolicks. Right now the only note
 * type handled is the CORE/NT_FILE note used on GNU/Linux. FreeBSD uses a
 * FreeBSD/NT_PROCSTAT_VMMAP note and QNX uses a QNX/QNT_DEBUG_LINK_MAP note for
 * similar purposes. Other target OSes probably use something else.
 *
 * Note interpretation requires both name and type.
 */
static int
_handle_pt_note_segment (uint32_t  n_namesz UNUSED,
                         uint32_t  n_descsz UNUSED,
                         uint32_t  n_type,
                         char     *name,
                         uint8_t  *desc,
                         void     *arg)
{
#ifdef NT_FILE
  if (n_type == NT_FILE && strcmp (name, "CORE") == 0)
    {
      return _handle_nt_file_note (desc, arg);
    }
#endif
  return UNW_ESUCCESS;
}


/**
 * Get filemap info from core file (Linux and similar)
 *
 * If there is a mapinfo not in the core file, map its contents to the phdrs.
 *
 * Since there may or may not be any mapinfo notes it's OK for this function to
 * fail.
 */
int
_UCD_get_mapinfo (struct UCD_info *ui, coredump_phdr_t *phdrs, unsigned phdr_size)
{
  int ret = UNW_ESUCCESS; /* it's OK if there are no file mappings */

  for (unsigned i = 0; i < phdr_size; ++i)
    {
      if (phdrs[i].p_type == PT_NOTE)
        {
          uint8_t *segment;
          size_t segment_size;
          ret = _UCD_elf_read_segment (ui, &phdrs[i], &segment, &segment_size);

          if (ret == UNW_ESUCCESS)
            {
              _UCD_elf_visit_notes (segment, segment_size, _handle_pt_note_segment, ui);
              free (segment);
            }
        }
    }

  return ret;
}
