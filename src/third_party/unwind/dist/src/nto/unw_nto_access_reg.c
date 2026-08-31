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
#include <sys/procfs.h>
#include <sys/neutrino.h>
#include <ucontext.h>


int unw_nto_access_reg (unw_addr_space_t as,
                        unw_regnum_t regnum,
                        unw_word_t *valp,
                        int write,
                        void *arg)
{
  unw_nto_internal_t *uni = (unw_nto_internal_t *)arg;
  int                 ret = -UNW_EUNSPEC;
  procfs_greg         greg;
  int ctl_fd = unw_nto_procfs_open_ctl (uni->pid);

  if (ctl_fd < 0)
    {
      Debug (0, "error %d opening ctl file: %s", errno, strerror (errno));
      return ret;
    }

  int err = devctl (ctl_fd, DCMD_PROC_CURTHREAD, & (uni->tid), sizeof (uni->tid), 0);

  if (err != EOK)
    {
      Debug (0, "error %d in devctl(DCMD_PROC_CURTHREAD): %s", err, strerror (err));
      close (ctl_fd);
      return ret;
    }

  if (write)
    {
      Debug (0, "write not supported\n");
    }

  else
    {
      err = devctl (ctl_fd, DCMD_PROC_GETGREG, &greg, sizeof (greg), NULL);

      if (err != EOK)
        {
          Debug (0, "error %d in devctl(DCMD_PROC_GETGREG): %s", err, strerror (err));
          close (ctl_fd);
          return ret;
        }

      switch (regnum)
        {
        case UNW_REG_IP:
#if defined(__X86_64__)
          *valp = GET_REGIP (&greg.x86_64);
#elif defined(__ARM__)
          *valp = GET_REGIP (&greg.arm);
#elif defined(__aarch64__)
          *valp = greg.aarch64.gpr[AARCH64_REG_X30];
#else
# error Unsupported architecture
#endif
          break;

        case UNW_REG_SP:
#if defined(__X86_64__)
          *valp = GET_REGSP (&greg.x86_64);
#elif defined(__ARM__)
          *valp = GET_REGSP (&greg.arm);
#elif defined(__aarch64__)
          *valp = GET_REGSP (&greg.aarch64);
#else
# error Unsupported architecture
#endif
          break;
#if defined(__aarch64__)

        case UNW_AARCH64_PC:
          *valp = GET_REGIP (&greg.aarch64);
          break;

        case UNW_AARCH64_X29:
          *valp = greg.aarch64.gpr[AARCH64_REG_X29];
          break;
#endif
#if defined(__X86_64__)

        case UNW_X86_64_RAX:
          Debug (15, "request for UNW_X86_64_RAX (%d)\n", UNW_X86_64_RAX);
          *valp = greg.x86_64.rax;
          break;

        case UNW_X86_64_RDX:
          Debug (15, "request for UNW_X86_64_RDX (%d)\n", UNW_X86_64_RDX);
          *valp = greg.x86_64.rdx;
          break;

        case UNW_X86_64_RCX:
          Debug (15, "request for UNW_X86_64_RCX (%d)\n", UNW_X86_64_RCX);
          *valp = greg.x86_64.rcx;
          break;

        case UNW_X86_64_RBX:
          Debug (15, "request for UNW_X86_64_RBX (%d)\n", UNW_X86_64_RBX);
          *valp = greg.x86_64.rbx;
          break;

        case UNW_X86_64_RSI:
          Debug (15, "request for UNW_X86_64_RSI (%d)\n", UNW_X86_64_RSI);
          *valp = greg.x86_64.rsi;
          break;

        case UNW_X86_64_RDI:
          Debug (15, "request for UNW_X86_64_RDI (%d)\n", UNW_X86_64_RDI);
          *valp = greg.x86_64.rdi;
          break;

        case UNW_X86_64_RBP:
          Debug (15, "request for UNW_X86_64_RBP (%d)\n", UNW_X86_64_RBP);
          *valp = greg.x86_64.rbp;
          break;

        case UNW_X86_64_R8:
          Debug (15, "request for UNW_X86_64_R8 (%d)\n", UNW_X86_64_R8);
          *valp = greg.x86_64.r8;
          break;

        case UNW_X86_64_R9:
          Debug (15, "request for UNW_X86_64_R9 (%d)\n", UNW_X86_64_R9);
          *valp = greg.x86_64.r9;
          break;

        case UNW_X86_64_R10:
          Debug (15, "request for UNW_X86_64_R10 (%d)\n", UNW_X86_64_R10);
          *valp = greg.x86_64.r10;
          break;

        case UNW_X86_64_R11:
          Debug (15, "request for UNW_X86_64_R11 (%d)\n", UNW_X86_64_R11);
          *valp = greg.x86_64.r11;
          break;

        case UNW_X86_64_R12:
          Debug (15, "request for UNW_X86_64_R12 (%d)\n", UNW_X86_64_R12);
          *valp = greg.x86_64.r12;
          break;

        case UNW_X86_64_R13:
          Debug (15, "request for UNW_X86_64_R13 (%d)\n", UNW_X86_64_R13);
          *valp = greg.x86_64.r13;
          break;

        case UNW_X86_64_R14:
          Debug (15, "request for UNW_X86_64_R14 (%d)\n", UNW_X86_64_R14);
          *valp = greg.x86_64.r14;
          break;

        case UNW_X86_64_R15:
          Debug (15, "request for UNW_X86_64_R15 (%d)\n", UNW_X86_64_R15);
          *valp = greg.x86_64.r15;
          break;
#endif

        default:
          Debug (0, "as=%p, regnum=%d, valp=%p, write=%d uni=%p\n", as, regnum, valp, write, uni);
          break;
        }

      ret = 0;
    }

  close (ctl_fd);
  return ret;
}

