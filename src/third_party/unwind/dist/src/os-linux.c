/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2005 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

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

#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

#include "libunwind_i.h"
#include "os-linux.h"

int
tdep_get_elf_image (struct elf_image *ei, pid_t pid, unw_word_t ip,
                    unsigned long *segbase, unsigned long *mapoff,
                    char *path, size_t pathlen)
{
  struct map_iterator mi;
  int found = 0, rc;
  unsigned long hi;
  char root[sizeof ("/proc/0123456789/root")], *cp;
  char *full_path;
  struct stat st;


  if (maps_init (&mi, pid) < 0)
    return -1;

  while (maps_next (&mi, segbase, &hi, mapoff, NULL))
    if (ip >= *segbase && ip < hi)
      {
        found = 1;
        break;
      }

  if (!found)
    {
      maps_close (&mi);
      return -1;
    }

  full_path = mi.path;

  /* Get process root */
  memcpy (root, "/proc/", 6);
  cp = unw_ltoa (root + 6, pid);
  assert (cp + 6 < root + sizeof (root));
  memcpy (cp, "/root", 6);

  if (!stat(root, &st) && S_ISDIR(st.st_mode))
    {
      full_path = (char*) malloc (strlen (root) + strlen (mi.path) + 1);
      if (!full_path)
        full_path = mi.path;
      else
        {
          strcpy (full_path, root);
          strcat (full_path, mi.path);
        }
    }

  if (path)
    {
      strncpy(path, full_path, pathlen);
    }
  rc = elf_map_image (ei, full_path);

  if (full_path && full_path != mi.path)
    free (full_path);

  maps_close (&mi);
  return rc;
}

#ifndef UNW_REMOTE_ONLY

void
tdep_get_exe_image_path (char *path)
{
  strcpy(path, "/proc/self/exe");
}

#endif /* !UNW_REMOTE_ONLY */
