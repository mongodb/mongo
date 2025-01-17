/**
 * Extract filemap info from a coredump (QNX)
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

#include <sys/qnx_linkmap_note.h>


#define MAP_PATH_MAX 512


/** Number of bytes for rounding up addresses. */
static inline const size_t _roundup(size_t size)
{
#if UNW_ELF_CLASS == UNW_ELFCLASS32
  static const size_t _roundup_sz = 4;
#else
  static const size_t _roundup_sz = 8;
#endif
  return (((size) + ((_roundup_sz)-1)) & ~((_roundup_sz)-1));
}


/**
 * Handle the QNX/QNT_LINK_MAP note type.
 * @param[in] desc  The note-specific data
 * @param[in] arg   The user-supplied callback argument
 *
 * The QNX/QNT_LINK_MAP note type contains a list of start/end virtual addresses
 * within the core file and an associated filename. The purpose is to map
 * various segments loaded into memory from ELF files with the ELF file from
 * which those segments were loaded.
 *
 * This function links the file names mapped in the QNX/QNT_LINK_MAP note with
 * the program headers in the core file through the UCD_info file table.
 */
static int
_handle_nt_file_note (uint8_t *desc, void *arg)
{
  struct UCD_info *ui = (struct UCD_info *)arg;
  struct qnx_linkmap_note* linkmap_note = (struct qnx_linkmap_note*)desc;
  uintptr_t data = (uintptr_t)&linkmap_note->data[0];

  const size_t r_debug_size = _roundup(sizeof(struct qnx_r_debug));
  const struct qnx_link_map* linkmap = (struct qnx_link_map*)(data + r_debug_size);
  const size_t link_count = (linkmap_note->header.linkmapsz - r_debug_size) / sizeof(struct qnx_link_map);
  const char *const strtab = (const char *const)(data + _roundup(linkmap_note->header.linkmapsz));

  for (size_t i = 0; i < link_count; ++i)
  	{
	  const struct qnx_link_map *map = &linkmap[i];
      for (size_t p = 0; p < ui->phdrs_count; ++p)
		{
		  coredump_phdr_t *phdr = &ui->phdrs[p];
		  if (phdr->p_type == PT_LOAD 
		  	  && linkmap[i].l_addr >= phdr->p_vaddr
		  	  && linkmap[i].l_addr < phdr->p_vaddr + phdr->p_memsz)
		  	{
		  	  char libpath[MAP_PATH_MAX];
		  	  if ((0 == strncmp(strtab + linkmap[i].l_name, "PIE", 3))
		  	      || (0 == strncmp(strtab + linkmap[i].l_name, "EXE", 3)))
		  	    {
		  	      snprintf (libpath, MAP_PATH_MAX, "%s", strtab + map->l_path);
				}
		  	  else
		  	    {
		  	      snprintf (libpath, MAP_PATH_MAX, "%s%s", strtab + map->l_path, strtab + map->l_name);
				}
		  	  phdr->p_backing_file_index = ucd_file_table_insert (&ui->ucd_file_table, libpath);
		  	  Debug(2, "added '%s' at index %d for phdr %zu\n", libpath, phdr->p_backing_file_index, p);
		  	  break;
		  	}
		}
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
 */
static int
_handle_pt_note_segment (uint32_t  n_namesz UNUSED,
						 uint32_t  n_descsz UNUSED,
						 uint32_t  n_type,
						 char     *name,
						 uint8_t  *desc,
						 void     *arg)
{
  if ((strcmp(name, QNX_NOTE_NAME) == 0) && n_type == QNT_LINK_MAP)
    {
      return _handle_nt_file_note (desc, arg);
    }
  return UNW_ESUCCESS;
}


/**
 * Get filemap info from core file (QNX)
 * @param[in]  ui
 * @param[in]  phdrs
 * @param[in]  phdr_size
 *
 * If there is a mapinfo note in the core file, map its contents to the phdrs.
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
