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
#include "os-qnx.h"
#include "unw_nto_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/neutrino.h>


/**
 * Hold the target thread to avoid race conditions.
 * @param[in] ctl_fd  File descriptor for /proc process ctl file
 * @param[in] tid     Identifies thread to hold
 *
 * @returns 0 on error, otherwise on success.
 */
static int _hold_thread (int ctl_fd, pthread_t tid)
{
  procfs_threadctl tctl =
  {
    .cmd = _NTO_TCTL_ONE_THREAD_HOLD,
    .tid = tid
  };
  memcpy (tctl.data, & (tid), sizeof (tid));
  int err = devctl (ctl_fd, DCMD_PROC_THREADCTL, &tctl, sizeof (tctl), NULL);

  if (err != EOK)
    {
      Debug (0, "error %d in devctl(DCMD_PROC_THREADCTL): %s\n", err, strerror (err));
    }

  return err == EOK;
}


void *unw_nto_create (pid_t pid, pthread_t tid)
{
  unw_nto_internal_t *uni = calloc (1, sizeof (unw_nto_internal_t));
  if (uni == NULL)
    {
      return NULL;
    }

  mi_init ();

  uni->pid = pid;
  uni->tid = tid;
  uni->edi.di_cache.format = -1;
  uni->edi.di_debug.format = -1;
  int ctl_fd = unw_nto_procfs_open_ctl (pid);

  if (ctl_fd < 0)
    {
      Debug (0, "error %d opening procfs ctl file for pid %d: %s\n",
             errno, pid, strerror (errno));
      unw_nto_destroy (uni);
      uni = NULL;
      return uni;
    }

  if (!_hold_thread (ctl_fd, tid))
    {
      close (ctl_fd);
      unw_nto_destroy (uni);
      uni = NULL;
    }

  else
    {
      close (ctl_fd);
    }

  return uni;
}

