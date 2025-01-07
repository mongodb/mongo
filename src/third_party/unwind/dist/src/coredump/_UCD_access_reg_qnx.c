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

 The above copyright notice and this permission notice shall be
         included in all
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
 * Access scalar CPU register from core file
 * @param[in]  as     Pointer to unwind address space structure.
 * @param[in]  regnum Arch-specific index of register to access.
 * @param[out] valp   Pointer to value to write to to be read to.
 * @param[in]  write  Direction of operation (1 == write, 0 == read register).
 * @param[in]  arg    Arg passed through from back end (pointer to UCD_info).
 *
 * @returns 0 on success, <0 on error.
 *
 * Reads a value from an architecture-specific, OS-specific structure retrieved
 * from the core file under analysis.
 *
 * This is the QNX-specific implementation.
 */
int
_UCD_access_reg (unw_addr_space_t  as,
                 unw_regnum_t      regnum,
                 unw_word_t       *valp,
                 int               write,
                 void             *arg)
{
  if (write)
    {
      Debug(0, "write is not supported\n");
      return -UNW_EINVAL;
    }

  struct UCD_info *ui = arg;

#if defined(UNW_TARGET_X86)
  switch (regnum) {
  case UNW_X86_EAX:
     *valp = ui->prstatus->greg.x86.eax;
     break;
  case UNW_X86_EDX:
     *valp = ui->prstatus->greg.x86.edx;
     break;
  case UNW_X86_ECX:
     *valp = ui->prstatus->greg.x86.ecx;
     break;
  case UNW_X86_EBX:
     *valp = ui->prstatus->greg.x86.ebx;
     break;
  case UNW_X86_ESI:
     *valp = ui->prstatus->greg.x86.esi;
     break;
  case UNW_X86_EDI:
     *valp = ui->prstatus->greg.x86.edi;
     break;
  case UNW_X86_EBP:
     *valp = ui->prstatus->greg.x86.ebp;
     break;
  case UNW_X86_ESP:
     *valp = ui->prstatus->greg.x86.esp;
     break;
  case UNW_X86_EIP:
     *valp = ui->prstatus->greg.x86.eip;
     break;
  case UNW_X86_EFLAGS:
     *valp = ui->prstatus->greg.x86.efl;
     break;
  default:
      Debug(0, "bad regnum:%d\n", regnum);
      return -UNW_EINVAL;
  }
#elif defined(UNW_TARGET_X86_64)
  switch (regnum) {
  case UNW_X86_64_RAX:
     *valp = ui->prstatus->greg.x86_64.rax;
     break;
  case UNW_X86_64_RDX:
     *valp = ui->prstatus->greg.x86_64.rdx;
     break;
  case UNW_X86_64_RCX:
     *valp = ui->prstatus->greg.x86_64.rcx;
     break;
  case UNW_X86_64_RBX:
     *valp = ui->prstatus->greg.x86_64.rbx;
     break;
  case UNW_X86_64_RSI:
     *valp = ui->prstatus->greg.x86_64.rsi;
     break;
  case UNW_X86_64_RDI:
     *valp = ui->prstatus->greg.x86_64.rdi;
     break;
  case UNW_X86_64_RBP:
     *valp = ui->prstatus->greg.x86_64.rbp;
     break;
  case UNW_X86_64_RSP:
     *valp = ui->prstatus->greg.x86_64.rsp;
     break;
  case UNW_X86_64_RIP:
     *valp = ui->prstatus->greg.x86_64.rip;
     break;
  default:
      Debug(0, "bad regnum:%d\n", regnum);
      return -UNW_EINVAL;
  }
#elif defined(UNW_TARGET_ARM)
  if (regnum >= UNW_ARM_R0 && regnum <= UNW_ARM_R16) {
     *valp = ui->prstatus->greg.arm.gpr[regnum];
  } else {
       Debug(0, "bad regnum:%d\n", regnum);
       return -UNW_EINVAL;
  }
#elif defined(UNW_TARGET_AARCH64)
  if (regnum >= UNW_AARCH64_X0 && regnum <= UNW_AARCH64_X30) {
     *valp = ui->prstatus->greg.aarch64.gpr[regnum];
  } else {
     switch (regnum) {
     case UNW_AARCH64_SP:
       *valp = ui->prstatus->greg.aarch64.gpr[AARCH64_REG_SP];
       break;
     case UNW_AARCH64_PC:
       *valp = ui->prstatus->greg.aarch64.elr;
       break;
     default:
       Debug(0, "bad regnum:%d\n", regnum);
       return -UNW_EINVAL;
     }
  }

#else
#error Port me
#endif

  return 0;
}

