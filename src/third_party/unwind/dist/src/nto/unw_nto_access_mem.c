/*
 * Copyright 2020, 2022 Blackberry Limited.
 *
 * This file is part of libunwind.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "unw_nto_internal.h"
#include "os-qnx.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


/**
 * Read/write data from/to the target address space
 *
 * @todo save opened fd in unw_nto_internal_t
 */
int unw_nto_access_mem (unw_addr_space_t as,
                        unw_word_t addr,
                        unw_word_t *valp,
                        int write,
                        void *arg)
{
  int ret = -UNW_ENOINFO;
  unw_nto_internal_t *uni = (unw_nto_internal_t *)arg;
  int as_fd = unw_nto_procfs_open_as (uni->pid);

  if (as_fd < 0)
    {
      Debug (0, "error %d opening as file: %s\n", errno, strerror (errno));
      return ret;
    }

  if (lseek (as_fd, (off_t)addr, SEEK_SET) == -1)
    {
      Debug (0, "error %d in lseek(%" PRIxPTR "): %s\n", errno, addr, strerror (errno));
      close (as_fd);
      return ret;
    }

  if (!write)
    {
      ssize_t count = read (as_fd, valp, sizeof (*valp));

      if (count != sizeof (*valp))
        {
          Debug (0, "error %d in read(%zu): %s\n", errno, sizeof (*valp), strerror (errno));
          close (as_fd);
          return ret;
        }

      ret = 0;
    }

  close (as_fd);
  return ret;
}

