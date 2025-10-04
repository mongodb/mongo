/*
 * Copyright 2022, 2023 BlackBerry Limited.
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
#include "config.h"

#include "unwind_i.h"
#include "ucontext_i.h"
#include <sys/neutrino.h>


/**
 * @brief Predicate to check if current IP is a signal trampoline.
 * @param[in]  cursor The current unwinding state.
 *
 * This function assumes the IP points to the first instruction after the call
 * to the signal handler, and the bytes checked are the opcodes dumped by
 * objdump between the call and the syscall that returns from the signal.
 *
 * This is not 100% robust but it's unlikely any other syscall setup is
 * identical.
 *
 * @returns 0 if it does not detect the current function is a signal trampoline,
 * 1 if it detects the trampoline.
 */
int
unw_is_signal_frame (unw_cursor_t *cursor)
{
  const unsigned char sig[] =
    {
      0x4c, 0x89, 0xef,					// mov    %r13,%rdi
      0x49, 0x8b, 0x74, 0x24, 0x30,			// mov    0x30(%r12),%rsi
      0x49, 0x8b, 0x54, 0x24, 0x38,			// mov    0x38(%r12),%rdx
      0x4d, 0x8b, 0x44, 0x24, 0x48,			// mov    0x48(%r12),%r8
      0x4d, 0x8b, 0x4c, 0x24, 0x50,			// mov    0x50(%r12),%r9
      0x49, 0x8b, 0x5c, 0x24, 0x60,			// mov    0x60(%r12),%rbx
      0x49, 0x8b, 0x6c, 0x24, 0x68,			// mov    0x68(%r12),%rbp
      0x4d, 0x8b, 0xac, 0x24, 0x88, 0x00, 0x00, 0x00,	// mov    0x88(%r12),%r13
      0x4d, 0x8b, 0xb4, 0x24, 0x90, 0x00, 0x00, 0x00,	// mov    0x90(%r12),%r14
      0x4d, 0x8b, 0xbc, 0x24, 0x98, 0x00, 0x00, 0x00,	// mov    0x98(%r12),%r15
      0x4d, 0x8b, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00,	// mov    0x80(%r12),%r12
      0x48, 0xc7, 0xc0, 0x1b, 0x00, 0x00, 0x00,		// mov    $0x1b,%rax
      0x0f, 0x05,					// syscall
    };

  struct cursor *c = (struct cursor *) cursor;
  unw_addr_space_t as = c->dwarf.as;
  unw_accessors_t *a = unw_get_accessors_int (as);
  unw_word_t ip = c->dwarf.ip;
  int retval = 1;

#if CONSERVATIVE_CHECKS
  int val = 0;
  if (c->dwarf.as == unw_local_addr_space) {
    val = dwarf_get_validate(&c->dwarf);
    dwarf_set_validate(&c->dwarf, 1);
  }
#endif

  unw_word_t w = 0;
  for (size_t i = 0; i != sizeof(sig)/sizeof(sig[0]); ++i)
    {
      size_t byte_index = i % sizeof(unw_word_t);
      if (0 == byte_index)
        {
          int ret = a->access_mem (as, ip, &w, 0, c->dwarf.as_arg);
          if (ret < 0)
            {
              retval = 0;
              break;
            }
          ip += sizeof(w);
        }

      if (sig[i] != (w&0xff))
        {
          retval = 0;
          break;
        }
      w >>= 8;
    }

#if CONSERVATIVE_CHECKS
  if (c->dwarf.as == unw_local_addr_space) {
    dwarf_set_validate(&c->dwarf, val);
  }
#endif

  return retval;
}


/**
 * @brief Special handling when a signal trampoline is detected
 * @param[in]  cursor The current unwinding state.
 *
 * If this is a signal trampoline then %rsp points directly at the kernel
 * sighandler info so it's easy to get the ucontext.
 *
 * @returns < 0 on failure, 0 on success.
 */
HIDDEN int
x86_64_handle_signal_frame (unw_cursor_t *cursor)
{
  struct cursor *   c  = (struct cursor *) cursor;
  unw_addr_space_t  as = c->dwarf.as;
  unw_accessors_t * a  = unw_get_accessors_int (as);
  unw_word_t        sp = c->dwarf.cfa;
  unw_word_t        uc_ptr_addr = sp + offsetof(struct _sighandler_info, context);

  ucontext_t *context = NULL;
  int ret = a->access_mem (as, uc_ptr_addr, (unw_word_t*)&context, 0, c->dwarf.as_arg);
  if (ret < 0)
    {
      return -UNW_EBADFRAME;
    }
  Debug(3, "unwind context at %#010lx\n", (long)context);

  for (size_t i = 0; i < DWARF_NUM_PRESERVED_REGS; ++i)
    {
      c->dwarf.loc[i] = DWARF_NULL_LOC;
    }

  c->dwarf.loc[RAX] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rax);
  c->dwarf.loc[RDX] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rdx);
  c->dwarf.loc[RCX] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rcx);
  c->dwarf.loc[RBX] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rbx);
  c->dwarf.loc[RSI] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rsi);
  c->dwarf.loc[RDI] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rdi);
  c->dwarf.loc[RBP] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rbp);
  c->dwarf.loc[RSP] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rsp);
  c->dwarf.loc[ R8] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r8);
  c->dwarf.loc[ R9] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r9);
  c->dwarf.loc[R10] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r10);
  c->dwarf.loc[R11] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r11);
  c->dwarf.loc[R12] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r12);
  c->dwarf.loc[R13] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r13);
  c->dwarf.loc[R14] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r14);
  c->dwarf.loc[R15] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.r15);
  c->dwarf.loc[RIP] = DWARF_VAL_LOC (&c->dwarf, context->uc_mcontext.cpu.rip);

  dwarf_get (&c->dwarf, c->dwarf.loc[RSP], &c->dwarf.cfa);
  dwarf_get (&c->dwarf, c->dwarf.loc[RIP], &c->dwarf.ip);

  return 0;
}


#ifndef UNW_REMOTE_ONLY
HIDDEN void *
x86_64_r_uc_addr (ucontext_t *uc, int reg)
{
  void *addr;

  switch (reg)
    {
    case UNW_X86_64_R8:  addr = &uc->uc_mcontext.cpu.r8;  break;
    case UNW_X86_64_R9:  addr = &uc->uc_mcontext.cpu.r9;  break;
    case UNW_X86_64_R10: addr = &uc->uc_mcontext.cpu.r10; break;
    case UNW_X86_64_R11: addr = &uc->uc_mcontext.cpu.r11; break;
    case UNW_X86_64_R12: addr = &uc->uc_mcontext.cpu.r12; break;
    case UNW_X86_64_R13: addr = &uc->uc_mcontext.cpu.r13; break;
    case UNW_X86_64_R14: addr = &uc->uc_mcontext.cpu.r14; break;
    case UNW_X86_64_R15: addr = &uc->uc_mcontext.cpu.r15; break;
    case UNW_X86_64_RDI: addr = &uc->uc_mcontext.cpu.rdi; break;
    case UNW_X86_64_RSI: addr = &uc->uc_mcontext.cpu.rsi; break;
    case UNW_X86_64_RBP: addr = &uc->uc_mcontext.cpu.rbp; break;
    case UNW_X86_64_RBX: addr = &uc->uc_mcontext.cpu.rbx; break;
    case UNW_X86_64_RDX: addr = &uc->uc_mcontext.cpu.rdx; break;
    case UNW_X86_64_RAX: addr = &uc->uc_mcontext.cpu.rax; break;
    case UNW_X86_64_RCX: addr = &uc->uc_mcontext.cpu.rcx; break;
    case UNW_X86_64_RSP: addr = &uc->uc_mcontext.cpu.rsp; break;
    case UNW_X86_64_RIP: addr = &uc->uc_mcontext.cpu.rip; break;

    default:
      addr = NULL;
    }
  return addr;
}

HIDDEN NORETURN void
x86_64_sigreturn (unw_cursor_t *cursor)
{
  Debug(0, "Unsupported function.\n");
  abort();
}

#endif

HIDDEN int
x86_64_os_step(struct cursor *c)
{
  return (0);
}
