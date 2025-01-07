/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2005 Hewlett-Packard Co
   Copyright (C) 2007 David Mosberger-Tang
        Contributed by David Mosberger-Tang <dmosberger@gmail.com>

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

#include "libunwind_i.h"

#include <stdio.h>
#include <sys/param.h>
#include <limits.h>

#if HAVE_LZMA
#include <lzma.h>
#endif /* HAVE_LZMA */

struct symbol_info
{
  const char *strtab;
  const Elf_W (Sym) *sym;
  Elf_W (Addr) start_ip;
};

struct symbol_lookup_context
{
  unw_addr_space_t as;
  unw_word_t ip;
  struct elf_image *ei;
  Elf_W (Addr) load_offset;
  Elf_W (Addr) *min_dist;
};

struct symbol_callback_data
{
  char *buf;
  size_t buf_len;
};

struct ip_range_callback_data
{
  Elf_W (Addr) *start_ip;
  Elf_W (Addr) *end_ip;
};

static Elf_W (Shdr)*
elf_w (section_table) (const struct elf_image *ei)
{
  Elf_W (Ehdr) *ehdr = ei->image;
  Elf_W (Off) soff;

  soff = ehdr->e_shoff;
  if (soff + ehdr->e_shnum * ehdr->e_shentsize > ei->size)
    {
      Debug (1, "section table outside of image? (%lu > %lu)\n",
             (unsigned long) (soff + ehdr->e_shnum * ehdr->e_shentsize),
             (unsigned long) ei->size);
      return NULL;
    }

  return (Elf_W (Shdr) *) ((char *) ei->image + soff);
}

static char*
elf_w (string_table) (const struct elf_image *ei, int section)
{
  Elf_W (Ehdr) *ehdr = ei->image;
  Elf_W (Off) soff, str_soff;
  Elf_W (Shdr) *str_shdr;

  /* this offset is assumed to be OK */
  soff = ehdr->e_shoff;

  str_soff = soff + (section * ehdr->e_shentsize);
  if (str_soff + ehdr->e_shentsize > ei->size)
    {
      Debug (1, "string shdr table outside of image? (%lu > %lu)\n",
             (unsigned long) (str_soff + ehdr->e_shentsize),
             (unsigned long) ei->size);
      return NULL;
    }
  str_shdr = (Elf_W (Shdr) *) ((char *) ei->image + str_soff);

  if (str_shdr->sh_offset + str_shdr->sh_size > ei->size)
    {
      Debug (1, "string table outside of image? (%lu > %lu)\n",
             (unsigned long) (str_shdr->sh_offset + str_shdr->sh_size),
             (unsigned long) ei->size);
      return NULL;
    }

  Debug (16, "strtab=0x%lx\n", (long) str_shdr->sh_offset);
  return ei->image + str_shdr->sh_offset;
}

static int
elf_w (lookup_symbol_from_dynamic) (unw_addr_space_t as UNUSED,
                                    const struct symbol_lookup_context *context,
                                    int (*callback)(const struct symbol_lookup_context *context,
                                                    const struct symbol_info *syminfo, void *data),
                                    void *data)

{
  struct elf_image *ei = context->ei;
  Elf_W (Addr) load_offset = context->load_offset;
  Elf_W (Addr) file_offset = 0;
  Elf_W (Ehdr) *ehdr = ei->image;
  Elf_W (Sym) *sym = NULL, *symtab = NULL;
  Elf_W (Phdr) *phdr;
  Elf_W (Word) sym_num;
  Elf_W (Word) *hash = NULL, *gnu_hash = NULL;
  Elf_W (Addr) val;
  const char *strtab = NULL;
  int ret = -UNW_ENOINFO;
  size_t i;
  Elf_W(Dyn) *dyn = NULL;

  phdr = (Elf_W (Phdr) *) ((char *) ei->image + ehdr->e_phoff);
  for (i = 0; i < ehdr->e_phnum; ++i)
    if (phdr[i].p_type == PT_PHDR)
      {
        file_offset = phdr[i].p_vaddr - phdr[i].p_offset;
      }
    else if (phdr[i].p_type == PT_DYNAMIC)
      {
        dyn = (Elf_W (Dyn) *) ((char *)ei->image + phdr[i].p_offset);
        break;
      }

  if (!dyn)
    return -UNW_ENOINFO;

  for (; dyn->d_tag != DT_NULL; ++dyn)
    {
      switch (dyn->d_tag)
        {
        case DT_SYMTAB:
          symtab = (Elf_W (Sym) *) ((char *) ei->image + dyn->d_un.d_ptr - file_offset);
          break;
        case DT_STRTAB:
          strtab = (const char *) ((char *) ei->image + dyn->d_un.d_ptr - file_offset);
          break;
        case DT_HASH:
          hash = (Elf_W (Word) *) ((char *) ei->image + dyn->d_un.d_ptr - file_offset);
          break;
        case DT_GNU_HASH:
          gnu_hash = (Elf_W (Word) *) ((char *) ei->image + dyn->d_un.d_ptr - file_offset);
          break;
        default:
          break;
        }
    }

  if (!symtab || !strtab || (!hash && !gnu_hash))
      return -UNW_ENOINFO;

  if (gnu_hash)
    {
        uint32_t *buckets = gnu_hash + 4 + (gnu_hash[2] * sizeof(size_t)/4);
        uint32_t *hashval;
        for (i = sym_num = 0; i < gnu_hash[0]; i++)
          if (buckets[i] > sym_num)
            sym_num = buckets[i];

        if (sym_num)
          {
            hashval = buckets + gnu_hash[0] + (sym_num - gnu_hash[1]);
            do sym_num++;
            while (!(*hashval++ & 1));
          }
    }
  else
    {
      sym_num = hash[1];
    }

  for (i = 0; i < sym_num; ++i)
    {
      sym = &symtab[i];
      if (ELF_W (ST_TYPE) (sym->st_info) == STT_FUNC
          && sym->st_shndx != SHN_UNDEF)
        {
          val = sym->st_value;
          if (sym->st_shndx != SHN_ABS)
            val += load_offset;
          if (tdep_get_func_addr (as, val, &val) < 0)
            continue;
          Debug (16, "0x%016lx info=0x%02x %s\n",
                 (long) val, sym->st_info, strtab + sym->st_name);

          /* as long as found one, the return will be success*/
          struct symbol_info syminfo =
            {
              .strtab = strtab,
              .sym = sym,
              .start_ip = val
            };
          if ((*callback) (context, &syminfo, data) == UNW_ESUCCESS)
            {
              if (ret != UNW_ESUCCESS)
                ret = UNW_ESUCCESS;
            }
        }
    }

  return ret;
}

static int
elf_w (lookup_symbol_closeness) (unw_addr_space_t as UNUSED,
                                 const struct symbol_lookup_context *context,
                                 int (*callback)(const struct symbol_lookup_context *context,
                                                 const struct symbol_info *syminfo, void *data),
                                 void *data)
{
  struct elf_image *ei = context->ei;
  Elf_W (Addr) load_offset = context->load_offset;
  size_t syment_size;
  Elf_W (Ehdr) *ehdr = ei->image;
  Elf_W (Sym) *sym, *symtab, *symtab_end;
  Elf_W (Shdr) *shdr;
  Elf_W (Addr) val;
  int i, ret = -UNW_ENOINFO;
  char *strtab;

  if (!elf_w (valid_object) (ei))
    return -UNW_ENOINFO;

  shdr = elf_w (section_table) (ei);
  if (!shdr)
    return -UNW_ENOINFO;

  for (i = 0; i < ehdr->e_shnum; ++i)
    {
      switch (shdr->sh_type)
        {
        case SHT_SYMTAB:
        case SHT_DYNSYM:
          symtab = (Elf_W (Sym) *) ((char *) ei->image + shdr->sh_offset);
          symtab_end = (Elf_W (Sym) *) ((char *) symtab + shdr->sh_size);
          syment_size = shdr->sh_entsize;

          strtab = elf_w (string_table) (ei, shdr->sh_link);
          if (!strtab)
            break;

          Debug (16, "symtab=0x%lx[%d]\n",
                 (long) shdr->sh_offset, shdr->sh_type);

          for (sym = symtab;
               sym < symtab_end;
               sym = (Elf_W (Sym) *) ((char *) sym + syment_size))
            {
              if (ELF_W (ST_TYPE) (sym->st_info) == STT_FUNC
                  && sym->st_shndx != SHN_UNDEF)
                {
                  val = sym->st_value;
                  if (sym->st_shndx != SHN_ABS)
                    val += load_offset;
                  if (tdep_get_func_addr (as, val, &val) < 0)
                    continue;
                  Debug (16, "0x%016lx info=0x%02x %s\n",
                         (long) val, sym->st_info, strtab + sym->st_name);

                  /* as long as found one, the return will be success*/
                  struct symbol_info syminfo =
                    {
                      .strtab = strtab,
                      .sym = sym,
                      .start_ip = val
                    };
                  if ((*callback) (context, &syminfo, data) == UNW_ESUCCESS)
                    {
                      if (ret != UNW_ESUCCESS)
                        ret = UNW_ESUCCESS;
                    }
                }
            }
          break;

        default:
          break;
        }
      shdr = (Elf_W (Shdr) *) (((char *) shdr) + ehdr->e_shentsize);
    }

  if (ret != UNW_ESUCCESS)
    ret = elf_w (lookup_symbol_from_dynamic) (as, context, callback, data);

  return ret;
}

static int
elf_w (lookup_symbol_callback)(const struct symbol_lookup_context *context,
                               const struct symbol_info *syminfo, void *data)
{
  int ret = -UNW_ENOINFO;
  struct symbol_callback_data *d = data;

  if (context->ip < syminfo->start_ip ||
      context->ip >= (syminfo->start_ip + syminfo->sym->st_size))
    return -UNW_ENOINFO;

  if ((Elf_W (Addr)) (context->ip - syminfo->start_ip) < *(context->min_dist))
    {
      *(context->min_dist) = (Elf_W (Addr)) (context->ip - syminfo->start_ip);
      Debug (1, "candidate sym: %s@0x%lx\n", syminfo->strtab + syminfo->sym->st_name, syminfo->start_ip);
      strncpy (d->buf, syminfo->strtab + syminfo->sym->st_name, d->buf_len);
      d->buf[d->buf_len - 1] = '\0';
      ret = (strlen (syminfo->strtab + syminfo->sym->st_name) >= d->buf_len
             ? -UNW_ENOMEM : UNW_ESUCCESS);
    }

  return ret;
}

static int
elf_w (lookup_symbol) (unw_addr_space_t as,
                       unw_word_t ip, struct elf_image *ei,
                       Elf_W (Addr) load_offset,
                       char *buf, size_t buf_len, Elf_W (Addr) *min_dist)
{
  struct symbol_lookup_context context =
    {
      .as = as,
      .ip = ip, 
      .ei = ei,
      .load_offset = load_offset,
      .min_dist = min_dist,
    };
  struct symbol_callback_data data =
    {
      .buf = buf, 
      .buf_len = buf_len,
    };
  return elf_w (lookup_symbol_closeness) (as,
                                          &context,
                                          elf_w (lookup_symbol_callback),
                                          &data);
}

static int
elf_w (lookup_ip_range_callback)(const struct symbol_lookup_context *context,
                                 const struct symbol_info *syminfo, void *data)
{
  int ret = -UNW_ENOINFO;
  struct ip_range_callback_data *d = data;

  if (context->ip < syminfo->start_ip ||
      context->ip >= (syminfo->start_ip + syminfo->sym->st_size))
    return -UNW_ENOINFO;

  if ((Elf_W (Addr)) (context->ip - syminfo->start_ip) < *(context->min_dist))
    {
      *(context->min_dist) = (Elf_W (Addr)) (context->ip - syminfo->start_ip);
      *(d->start_ip) = syminfo->start_ip;
      *(d->end_ip) = syminfo->start_ip + syminfo->sym->st_size;

      ret = UNW_ESUCCESS;
    }

  return ret;
}

static int
elf_w (lookup_ip_range)(unw_addr_space_t as,
                        unw_word_t ip, struct elf_image *ei,
                        Elf_W (Addr) load_offset, Elf_W (Addr) *start_ip,
                        Elf_W (Addr) *end_ip, Elf_W (Addr) *min_dist)
{
  struct symbol_lookup_context context =
    {
      .as = as,
      .ip = ip, 
      .ei = ei,
      .load_offset = load_offset,
      .min_dist = min_dist
    };
  struct ip_range_callback_data data =
    {
      .start_ip = start_ip,
      .end_ip = end_ip
    };
  return elf_w (lookup_symbol_closeness) (as,
                                          &context,
                                          elf_w (lookup_ip_range_callback),
                                          &data);
}

static Elf_W (Addr)
elf_w (get_load_offset) (struct elf_image *ei, unsigned long segbase)
{
  Elf_W (Addr) offset = 0;
  Elf_W (Ehdr) *ehdr;
  Elf_W (Phdr) *phdr;
  int i;
  // mapoff is obtained from mmap information, so it is always aligned on a page size.
  // PT_LOAD program headers p_offset however is not guaranteed to be aligned on a
  // page size, ld.lld generate libraries where this is not the case. So we must
  // make sure we compare both values with the same alignment.
  unsigned long pagesize_alignment_mask = ~(unw_page_size - 1UL);

  ehdr = ei->image;
  phdr = (Elf_W (Phdr) *) ((char *) ei->image + ehdr->e_phoff);

  for (i = 0; i < ehdr->e_phnum; ++i)
    if (phdr[i].p_type == PT_LOAD && phdr[i].p_flags & PF_X)
      {
        offset = segbase - phdr[i].p_vaddr + (phdr[i].p_offset & (~pagesize_alignment_mask));
        break;
      }

  return offset;
}

#if HAVE_LZMA

#define XZ_MAX_ALLOCS 16
struct xz_allocator_data {
  struct {
    void   *ptr;
    size_t  size;
  } allocations[XZ_MAX_ALLOCS];
  uint8_t n_allocs;
};

static void*
xz_alloc (void *opaque, size_t nmemb, size_t size)
{
  struct xz_allocator_data *data = opaque;
  if (XZ_MAX_ALLOCS == data->n_allocs)
    return NULL;
  size = UNW_ALIGN(size * nmemb, unw_page_size);
  void *ptr;
  GET_MEMORY (ptr, size);
  if (!ptr) return ptr;
  data->allocations[data->n_allocs].ptr  = ptr;
  data->allocations[data->n_allocs].size = size;
  ++data->n_allocs;
  return ptr;
}

static void
xz_free (void *opaque, void *ptr)
{
  struct xz_allocator_data *data = opaque;
  for (uint8_t i = data->n_allocs; i-- > 0;)
    {
      if (data->allocations[i].ptr == ptr)
        {
          mi_munmap (ptr, data->allocations[i].size);
          --data->n_allocs;
          if (i != data->n_allocs)
            {
              data->allocations[i] = data->allocations[data->n_allocs];
            }
          return;
        }
    }
}

static void
xz_free_all (struct xz_allocator_data *data)
{
  while (data->n_allocs-- > 0)
    {
      mi_munmap (data->allocations[data->n_allocs].ptr, data->allocations[data->n_allocs].size);
    }
}

static size_t
xz_uncompressed_size (lzma_allocator *xz_allocator, uint8_t *compressed, size_t length)
{
  uint64_t memlimit = UINT64_MAX;
  size_t ret = 0, pos = 0;
  lzma_stream_flags options;
  lzma_index *index;

  if (length < LZMA_STREAM_HEADER_SIZE)
    return 0;

  uint8_t *footer = compressed + length - LZMA_STREAM_HEADER_SIZE;
  if (lzma_stream_footer_decode (&options, footer) != LZMA_OK)
    return 0;

  if (length < LZMA_STREAM_HEADER_SIZE + options.backward_size)
    return 0;

  uint8_t *indexdata = footer - options.backward_size;
  if (lzma_index_buffer_decode (&index, &memlimit, xz_allocator, indexdata,
                                &pos, options.backward_size) != LZMA_OK)
    return 0;

  if (lzma_index_size (index) == options.backward_size)
    {
      ret = lzma_index_uncompressed_size (index);
    }

  lzma_index_end (index, xz_allocator);
  return ret;
}

static int
elf_w (extract_minidebuginfo) (struct elf_image *ei, struct elf_image *mdi)
{
  Elf_W (Shdr) *shdr;
  uint8_t *compressed = NULL;
  uint64_t memlimit = UINT64_MAX; /* no memory limit */
  size_t compressed_len, uncompressed_len;

  struct xz_allocator_data allocator_data;
  lzma_allocator xz_allocator =
  {
    .alloc  = xz_alloc,
    .free   = xz_free,
    .opaque = &allocator_data
  };

  shdr = elf_w (find_section) (ei, ".gnu_debugdata");
  if (!shdr)
    return 0;

  compressed = ((uint8_t *) ei->image) + shdr->sh_offset;
  compressed_len = shdr->sh_size;

  uncompressed_len = xz_uncompressed_size (&xz_allocator, compressed, compressed_len);
  if (uncompressed_len == 0)
    {
      xz_free_all (&allocator_data);
      Debug (1, "invalid .gnu_debugdata contents\n");
      return 0;
    }

  mdi->size = uncompressed_len;
  GET_MEMORY (mdi->image, uncompressed_len);

  if (!mdi->image)
    {
      xz_free_all (&allocator_data);
      return 0;
    }

  size_t in_pos = 0, out_pos = 0;
  lzma_ret lret;
  lret = lzma_stream_buffer_decode (&memlimit, 0, &xz_allocator,
                                    compressed, &in_pos, compressed_len,
                                    mdi->image, &out_pos, mdi->size);
  xz_free_all (&allocator_data);

  if (lret != LZMA_OK)
    {
      Debug (1, "LZMA decompression failed: %d\n", lret);
      mi_munmap (mdi->image, mdi->size);
      return 0;
    }

  return 1;
}
#else
static int
elf_w (extract_minidebuginfo) (struct elf_image *ei UNUSED, struct elf_image *mdi UNUSED)
{
  return 0;
}
#endif /* !HAVE_LZMA */

/* Find the ELF image that contains IP and return the "closest"
   procedure name, if there is one.  With some caching, this could be
   sped up greatly, but until an application materializes that's
   sensitive to the performance of this routine, why bother...  */

HIDDEN int
elf_w (get_proc_name_in_image) (unw_addr_space_t as, struct elf_image *ei,
                       unsigned long segbase,
                       unw_word_t ip,
                       char *buf, size_t buf_len, unw_word_t *offp)
{
  Elf_W (Addr) load_offset;
  Elf_W (Addr) min_dist = ~(Elf_W (Addr))0;
  int ret;

  load_offset = elf_w (get_load_offset) (ei, segbase);
  ret = elf_w (lookup_symbol) (as, ip, ei, load_offset, buf, buf_len, &min_dist);

  /* If the ELF image has MiniDebugInfo embedded in it, look up the symbol in
     there as well and replace the previously found if it is closer. */
  struct elf_image mdi;
  if (elf_w (extract_minidebuginfo) (ei, &mdi))
    {
      int ret_mdi = elf_w (lookup_symbol) (as, ip, &mdi, load_offset, buf,
                                           buf_len, &min_dist);

      /* Closer symbol was found (possibly truncated). */
      if (ret_mdi == 0 || ret_mdi == -UNW_ENOMEM)
        {
          ret = ret_mdi;
        }

      mi_munmap (mdi.image, mdi.size);
    }

  if (min_dist >= ei->size)
    return -UNW_ENOINFO;                /* not found */
  if (offp)
    *offp = min_dist;
  return ret;
}

HIDDEN int
elf_w (get_proc_name) (unw_addr_space_t as, pid_t pid, unw_word_t ip,
                       char *buf, size_t buf_len, unw_word_t *offp)
{
  unsigned long segbase, mapoff;
  struct elf_image ei;
  int ret;
  char file[PATH_MAX];

  ret = tdep_get_elf_image (&ei, pid, ip, &segbase, &mapoff, file, PATH_MAX);
  if (ret < 0)
    return ret;

  ret = elf_w (load_debuginfo) (file, &ei, 1);
  if (ret < 0)
    return ret;

  ret = elf_w (get_proc_name_in_image) (as, &ei, segbase, ip, buf, buf_len, offp);

  mi_munmap (ei.image, ei.size);
  ei.image = NULL;

  return ret;
}

HIDDEN int
elf_w (get_proc_ip_range_in_image) (unw_addr_space_t as, struct elf_image *ei,
                       unsigned long segbase,
                       unw_word_t ip,
                       unw_word_t *start, unw_word_t *end)
{
  Elf_W (Addr) load_offset;
  Elf_W (Addr) min_dist = ~(Elf_W (Addr))0;
  int ret;

  load_offset = elf_w (get_load_offset) (ei, segbase);
  ret = elf_w (lookup_ip_range) (as, ip, ei, load_offset, start, end, &min_dist);

  /* If the ELF image has MiniDebugInfo embedded in it, look up the symbol in
     there as well and replace the previously found if it is closer. */
  struct elf_image mdi;
  if (elf_w (extract_minidebuginfo) (ei, &mdi))
    {
      int ret_mdi = elf_w (lookup_ip_range) (as, ip, &mdi, load_offset, start,
                                             end, &min_dist);

      /* Closer symbol was found (possibly truncated). */
      if (ret_mdi == 0 || ret_mdi == -UNW_ENOMEM)
        {
          ret = ret_mdi;
        }

      mi_munmap (mdi.image, mdi.size);
    }

  if (min_dist >= ei->size)
    return -UNW_ENOINFO;                /* not found */
  return ret;
}

HIDDEN int
elf_w (get_proc_ip_range) (unw_addr_space_t as, pid_t pid, unw_word_t ip,
                           unw_word_t *start, unw_word_t *end)
{
  unsigned long segbase, mapoff;
  struct elf_image ei;
  int ret;
  char file[PATH_MAX];

  ret = tdep_get_elf_image (&ei, pid, ip, &segbase, &mapoff, file, PATH_MAX);
  if (ret < 0)
    return ret;

  ret = elf_w (load_debuginfo) (file, &ei, 1);
  if (ret < 0)
    return ret;

  ret = elf_w (get_proc_ip_range_in_image) (as, &ei, segbase, ip, start, end);

  mi_munmap (ei.image, ei.size);
  ei.image = NULL;

  return ret;
}

HIDDEN int
elf_w (get_elf_filename) (unw_addr_space_t as, pid_t pid, unw_word_t ip,
                          char *buf, size_t buf_len, unw_word_t *offp)
{
  unsigned long segbase, mapoff;
  int ret = UNW_ESUCCESS;

  // use NULL to no map elf image
  ret = tdep_get_elf_image (NULL, pid, ip, &segbase, &mapoff, buf, buf_len);
  if (ret < 0)
    return ret;

  if (offp)
      *offp = ip - segbase + mapoff;
  return ret;
}

HIDDEN Elf_W (Shdr)*
elf_w (find_section) (const struct elf_image *ei, const char* secname)
{
  Elf_W (Ehdr) *ehdr = ei->image;
  Elf_W (Shdr) *shdr;
  char *strtab;
  int i;

  if (!elf_w (valid_object) (ei))
    return 0;

  shdr = elf_w (section_table) (ei);
  if (!shdr)
    return 0;

  strtab = elf_w (string_table) (ei, ehdr->e_shstrndx);
  if (!strtab)
    return 0;

  for (i = 0; i < ehdr->e_shnum; ++i)
    {
      if (strcmp (strtab + shdr->sh_name, secname) == 0)
        {
          if (shdr->sh_offset + shdr->sh_size > ei->size)
            {
              Debug (1, "section \"%s\" outside image? (0x%lu > 0x%lu)\n",
                     secname,
                     (unsigned long) shdr->sh_offset + shdr->sh_size,
                     (unsigned long) ei->size);
              return 0;
            }

          Debug (16, "found section \"%s\" at 0x%lx\n",
                 secname, (unsigned long) shdr->sh_offset);
          return shdr;
        }

      shdr = (Elf_W (Shdr) *) (((char *) shdr) + ehdr->e_shentsize);
    }

  /* section not found */
  return 0;
}


static char *
elf_w (add_hex_byte) (char *str, uint8_t byte)
{
  const char hex[] = "0123456789abcdef";

  *str++ = hex[byte >> 4];
  *str++ = hex[byte & 0xf];
  *str = 0;

  return str;
}


static int
elf_w (find_build_id_path) (const struct elf_image *ei, char *path, unsigned path_len)
{
/*
 * build-id is only available on GNU plaforms. So on non-GNU platforms this
 * function just returns fail (-1).
 */
#if defined(ELF_NOTE_GNU) && defined(NT_GNU_BUILD_ID)
  const Elf_W (Ehdr) *ehdr = ei->image;
  const Elf_W (Phdr) *phdr;
  unsigned i;

  if (!elf_w (valid_object) (ei))
    return -1;

  phdr = (Elf_W (Phdr) *) ((uint8_t *) ehdr + ehdr->e_phoff);

  for (i = 0; i < ehdr->e_phnum; ++i, phdr = (const Elf_W (Phdr) *) (((const uint8_t *) phdr) + ehdr->e_phentsize))
    {
      const uint8_t *notes;
      const uint8_t *notes_end;

      /* The build-id is in a note section */
      if (phdr->p_type != PT_NOTE)
        continue;

      notes = ((const uint8_t *) ehdr) + phdr->p_offset;
      notes_end = notes + phdr->p_memsz;

      while(notes < notes_end)
        {
          const char prefix[] = "/usr/lib/debug/.build-id/";

          /* See "man 5 elf" for notes about alignment in Nhdr */
          const Elf_W(Nhdr) *nhdr = (const ElfW(Nhdr) *) notes;
          const ElfW(Word) namesz = nhdr->n_namesz;
          const ElfW(Word) descsz = nhdr->n_descsz;
          const ElfW(Word) nameasz = UNW_ALIGN(namesz, 4); /* Aligned size */
          const char *name = (const char *) (nhdr + 1);
          const uint8_t *desc = (const uint8_t *) name + nameasz;
          unsigned j;

          notes += sizeof(*nhdr) + nameasz + UNW_ALIGN(descsz, 4);

          if ((namesz != sizeof(ELF_NOTE_GNU)) ||  /* Spec says must be "GNU" with a NULL */
              (nhdr->n_type != NT_GNU_BUILD_ID) || /* Spec says must be NT_GNU_BUILD_ID   */
              (strcmp(name, ELF_NOTE_GNU) != 0))   /* Must be "GNU" with NULL termination */
            continue;

          /* Validate that we have enough space */
          if (path_len < (sizeof(prefix) +     /* Path prefix inc NULL */
                          2 +                  /* Subdirectory         */
                          1 +                  /* Directory separator  */
                          (2 * (descsz - 1)) + /* Leaf filename        */
                          6))                  /* .debug extension     */
            return -1;

          memcpy(path, prefix, sizeof(prefix));

          path = elf_w (add_hex_byte) (path + sizeof(prefix) - 1, *desc);
          *path++ = '/';

          for(j = 1, ++desc; j < descsz; ++j, ++desc)
            path = elf_w (add_hex_byte) (path, *desc);

          strcat(path, ".debug");

          return 0;
        }
    }
#endif /* defined(ELF_NOTE_GNU) */

  return -1;
}

/* Load a debug section, following .gnu_debuglink if appropriate
 * Loads ei from file if not already mapped.
 * If is_local, will also search sys directories /usr/local/dbg
 *
 * Returns 0 on success, failure otherwise.
 * ei will be mapped to file or the located .gnu_debuglink from file
 */
HIDDEN int
elf_w (load_debuginfo) (const char* file, struct elf_image *ei, int is_local)
{
  int ret;
  Elf_W (Shdr) *shdr;
  Elf_W (Ehdr) *prev_image;
  off_t prev_size;
  char path[PATH_MAX];

  if (!ei->image)
    {
      ret = elf_map_image(ei, file);
      if (ret)
        return ret;
    }

  prev_image = ei->image;
  prev_size = ei->size;

  /* Ignore separate debug files which contain a .gnu_debuglink section. */
  if (is_local == -1) {
    return 0;
  }

  ret = elf_w (find_build_id_path) (ei, path, sizeof(path));
  if (ret == 0)
    {
      ei->image = NULL;

      ret = elf_w (load_debuginfo) (path, ei, -1);
      if (ret == 0)
        {
          mi_munmap (prev_image, prev_size);
          return 0;
        }

      ei->image = prev_image;
      ei->size  = prev_size;
    }

  shdr = elf_w (find_section) (ei, ".gnu_debuglink");
  if (shdr) {
    if (shdr->sh_size >= PATH_MAX ||
	(shdr->sh_offset + shdr->sh_size > ei->size))
      return 0;

    {
      char linkbuf[shdr->sh_size];
      char *link = ((char *) ei->image) + shdr->sh_offset;
      char *p;
      static const char *debugdir = "/usr/lib/debug";
      char basedir[strlen(file) + 1];
      char newname[shdr->sh_size + strlen (debugdir) + strlen (file) + 9];

      memcpy(linkbuf, link, shdr->sh_size);

      if (memchr (linkbuf, 0, shdr->sh_size) == NULL)
	return 0;

      ei->image = NULL;

      Debug(1, "Found debuglink section, following %s\n", linkbuf);

      p = strrchr (file, '/');
      if (p != NULL)
	{
	  memcpy (basedir, file, p - file);
	  basedir[p - file] = '\0';
	}
      else
	basedir[0] = 0;

      strcpy (newname, basedir);
      strcat (newname, "/");
      strcat (newname, linkbuf);
      ret = elf_w (load_debuginfo) (newname, ei, -1);

      if (ret == -1)
	{
	  strcpy (newname, basedir);
	  strcat (newname, "/.debug/");
	  strcat (newname, linkbuf);
	  ret = elf_w (load_debuginfo) (newname, ei, -1);
	}

      if (ret == -1 && is_local == 1)
	{
	  strcpy (newname, debugdir);
	  strcat (newname, basedir);
	  strcat (newname, "/");
	  strcat (newname, linkbuf);
	  ret = elf_w (load_debuginfo) (newname, ei, -1);
	}

      if (ret == -1)
        {
          /* No debuglink file found even though .gnu_debuglink existed */
          ei->image = prev_image;
          ei->size = prev_size;

          return 0;
        }
      else
        {
          mi_munmap (prev_image, prev_size);
        }

      return ret;
    }
  }

  return 0;
}
