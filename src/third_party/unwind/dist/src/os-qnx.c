/* libunwind - a platform-independent unwind library
   Copyright (C) 2013 Garmin International
        Contributed by Matt Fischer <matt.fischer@garmin.com>
   Copyright (c) 2022-2023 BlackBerry Limited. All rights reserved.

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
#include "os-qnx.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/link.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>


#if __PTR_BITS__ == 32
typedef Elf32_Ehdr elf_ehdr_t;
typedef Elf32_Phdr elf_phdr_t;
#else
typedef Elf64_Ehdr elf_ehdr_t;
typedef Elf64_Phdr elf_phdr_t;
#endif

struct cb_info
{
  unw_word_t ip;
  unsigned long segbase;
  unsigned long offset;
  const char *path;
};

static int
phdr_callback(const struct dl_phdr_info *info, size_t size, void *data)
{
  int i;
  struct cb_info *cbi = (struct cb_info*)data;
  for(i=0; i<info->dlpi_phnum; i++)
    {
      unw_word_t segbase = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
      if(cbi->ip >= segbase && cbi->ip < segbase + info->dlpi_phdr[i].p_memsz)
        {
          cbi->path = info->dlpi_name;
          cbi->offset = info->dlpi_phdr[i].p_offset;
          cbi->segbase = segbase;
          return 1;
        }
    }

  return 0;
}


/**
 * Gets the number of mapped segments loaded in the target process image.
 *
 * @param[in]  ctl_fd  file descriptor for the process control file
 *
 * @returns the number of mapped segments loaded in the process image.
 */
static int
_get_map_count(int ctl_fd)
{
  int count = 0;
  int err = devctl(ctl_fd, DCMD_PROC_MAPINFO, NULL, 0, &count);
  if (err != EOK)
    {
      fprintf(stderr, "error %d in devctl(DCMD_PROC_MAPINFO): %s\n", err, strerror(err));
      return 0;
    }

  return count;
}


/**
 * Read some bytes from the procfs address space file for the target process.
 *
 * @param[in]  as_fd  file descriptor of te opened address space file
 * @param[in]  pos    offset within the file to start the read
 * @param[in]  sz     number of bytes to read
 * @param[out] buf    destination in which to read the bytes
 *
 * @returns the number of bytes read.  On failure to read, a byte count of 0 is
 * returned and errno is set appropriately.
 */
static int
_read_procfs_as(int        as_fd,
                uintptr_t  pos,
                size_t     sz,
                void      *buf)
{
  if (lseek(as_fd, (off_t)pos, SEEK_SET) == -1)
    {
      fprintf(stderr, "error %d in lseek(%" PRIxPTR "): %s\n", errno, pos, strerror(errno));
      return 0;
    }

  size_t bytes_read = 0;
  for (size_t readn = sz; readn > 0; )
    {
      int ret = read(as_fd, buf + bytes_read, readn);
      if (ret <= 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              continue;
            }
          else if (ret == 0)
            {
              errno = EFAULT;
            }
          return 0;
        }
      bytes_read += ret;
      readn -= ret;
    }

  return sz;
}


/**
 * Indicate of a chunk of memory is a valid ELF header.
 *
 * @param[in] e_ident  A (putative) ELF header
 *
 * @return true if it's a valid ELF header, false otherwise.
 */
static bool
_is_elf_header(unsigned char e_ident[EI_NIDENT])
{
  return e_ident[EI_MAG0] ==  ELFMAG0
         && e_ident[EI_MAG1] ==  ELFMAG1
         && e_ident[EI_MAG2] ==  ELFMAG2
         && e_ident[EI_MAG3] ==  ELFMAG3;
}


static int
_get_remote_elf_image(struct elf_image *ei,
                      pid_t             pid,
                      unw_word_t        ip,
                      unsigned long    *segbase,
                      unsigned long    *mapoff,
                      char             *path,
                      size_t            pathlen)
{
  int ret = -UNW_ENOINFO;

  union
  {
    procfs_debuginfo    i;
    char                path[PATH_MAX];
  } debug_info;

  int ctl_fd = unw_nto_procfs_open_ctl(pid);
  if (ctl_fd < 0)
    {
      fprintf(stderr, "error %d opening procfs ctl file for pid %d: %s\n",
              errno, pid, strerror(errno));
      return ret;
    }

  int as_fd = unw_nto_procfs_open_as(pid);
  if (as_fd < 0)
    {
      fprintf(stderr, "error %d opening procfs as file for pid %d: %s\n",
              errno, pid, strerror(errno));
      close(ctl_fd);
      return -UNW_ENOINFO;
    }

  int map_count = _get_map_count(ctl_fd);
  size_t maps_size = sizeof(procfs_mapinfo) * map_count;
  procfs_mapinfo *maps = malloc(maps_size);
  if (maps == NULL)
    {
      fprintf(stderr, "error %d in malloc(%zu): %s", errno, maps_size, strerror(errno));
      close (as_fd);
      close (ctl_fd);
      return -UNW_ENOINFO;
    }

  int nmaps = 0;
  ret = devctl(ctl_fd, DCMD_PROC_MAPINFO, maps, maps_size, &nmaps);
  if (ret != EOK)
    {
      fprintf(stderr, "error %d in devctl(DCMD_PROC_MAPINFO): %s", ret, strerror(ret));
      free(maps);
      close (as_fd);
      close (ctl_fd);
      return -UNW_ENOINFO;
    }

  int i = 0;
  for (; i < nmaps; ++i)
    {
      if (maps[i].flags & (MAP_ELF | PROT_EXEC))
        {
          uintptr_t vaddr = maps[i].vaddr;

          elf_ehdr_t elf_ehdr;
          ret = _read_procfs_as(as_fd, vaddr, sizeof(elf_ehdr), &elf_ehdr);
          if  (ret != sizeof(elf_ehdr))
            {
              continue;
            }

          /* Skip region if it's not an ELF segment. */
          if (!_is_elf_header(elf_ehdr.e_ident))
            {
              continue;
            }
          size_t size = elf_ehdr.e_ehsize;

          debug_info.i.vaddr = vaddr;
          debug_info.i.path[0]=0;
          ret = devctl(ctl_fd, DCMD_PROC_MAPDEBUG, &debug_info, sizeof(debug_info), 0);
          if (ret != EOK)
            {
              fprintf(stderr, "error %d in devctl(DCMD_PROC_MAPDEBUG): %s", ret, strerror(ret));
              continue;
            }
          uintptr_t reloc = debug_info.i.vaddr;

          elf_phdr_t elf_phdr;
          uintptr_t  phdr_offset = vaddr + elf_ehdr.e_phoff;
          for (int i = 0; i < elf_ehdr.e_phnum; ++i, phdr_offset+=elf_ehdr.e_phentsize)
            {
              ret = _read_procfs_as(as_fd, phdr_offset, sizeof(elf_phdr_t), &elf_phdr);
              if (ret == -1)
                {
                  continue;
                }
              if (elf_phdr.p_type == PT_LOAD && !(elf_phdr.p_flags&PF_W))
                {
                  size += elf_phdr.p_memsz;
                }
            }

          /* Skip segment if the IP is not contained within it. */
          if ((ip < vaddr) || (ip >= (vaddr + size)))
            {
              continue;
            }

          *segbase = vaddr;
          *mapoff  = reloc;
          if (path)
            {
              strncpy(path, debug_info.i.path, pathlen);
              path[pathlen - 1] = '\0';
            }

          if (ei)
            ret = elf_map_image(ei, path);
          else
            ret = strlen (debug_info.i.path) >= pathlen ? -UNW_ENOMEM : UNW_ESUCCESS;

          break;
        }
    }

  free(maps);
  close(as_fd);
  close(ctl_fd);
  return i == nmaps ? -UNW_ENOINFO : ret;
}


int
tdep_get_elf_image(struct elf_image *ei, pid_t pid, unw_word_t ip,
                   unsigned long *segbase, unsigned long *mapoff,
                   char *path, size_t pathlen)
{
  int ret = -UNW_ENOINFO;
  if (pid != getpid())
    {
      ret = _get_remote_elf_image(ei, pid, ip, segbase, mapoff, path, pathlen);
      return ret;
    }
  else
    {
      struct cb_info cbi;
      cbi.ip = ip;
      cbi.segbase = 0;
      cbi.offset = 0;
      cbi.path = NULL;

      if (dl_iterate_phdr (phdr_callback, &cbi) != 0)
        {
          if (path)
            {
              strncpy (path, cbi.path, pathlen);
              path[pathlen - 1] = '\0';
            }

          *mapoff = cbi.offset;
          *segbase = cbi.segbase;

          if (ei)
            ret = elf_map_image (ei, cbi.path);
          else
            ret = strlen (cbi.path) >= pathlen ? -UNW_ENOMEM : UNW_ESUCCESS;
        }
    }

  return ret;
}

#ifndef UNW_REMOTE_ONLY

void
tdep_get_exe_image_path (char *path)
{
  path[0] = 0; /* XXX */
}

#endif
