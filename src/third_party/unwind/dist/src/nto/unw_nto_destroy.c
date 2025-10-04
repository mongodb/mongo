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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/neutrino.h>
#include <unistd.h>


/**
 * Continues a (held) thread.
 * @param[in] ctl_fd  File descriptor for /proc process ctl file
 * @param[in] tid     Identifies thread to continue
 *
 * @returns 0 on error, otherwise on success.
 */
static void _cont_thread (int ctl_fd, pthread_t tid)
{
  procfs_threadctl tctl =
  {
    .cmd = _NTO_TCTL_ONE_THREAD_CONT,
    .tid = tid
  };
  memcpy (tctl.data, & (tid), sizeof (tid));
  int ret = devctl (ctl_fd, DCMD_PROC_THREADCTL, &tctl, sizeof (tctl), NULL);

  if (ret != EOK)
    {
      Debug (0, "error %d continuing thread %d: %s\n",
             ret, tid, strerror (ret));
    }
}


/**
 * Tears down an NTO unwind context.
 */
void unw_nto_destroy (void *arg)
{
  unw_nto_internal_t *uni = (unw_nto_internal_t *)arg;
  int ctl_fd = unw_nto_procfs_open_ctl (uni->pid);

  if (ctl_fd < 0)
    {
      Debug (0, "error %d opening procfs ctl file for pid %d: %s\n",
             errno, uni->pid, strerror (errno));
      free (uni);
      return;
    }

  _cont_thread (ctl_fd, uni->tid);
  close (ctl_fd);
  free (uni);
}

