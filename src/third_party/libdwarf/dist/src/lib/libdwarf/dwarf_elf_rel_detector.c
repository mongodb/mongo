/*
Copyright (c) 2019, David Anderson
All rights reserved.
cc
Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>

#include "dwarf_elf_defines.h"
#include "dwarf_elf_rel_detector.h"

unsigned
_dwarf_is_32bit_abs_reloc(unsigned int type, unsigned machine)
{
    unsigned r = 0;

    switch (machine) {
#if defined(EM_MIPS) && defined (R_MIPS_32)
    case EM_MIPS:
        r =  (0
#if defined (R_MIPS_32)
            | (type == R_MIPS_32)
#endif
#if defined (R_MIPS_TLS_DTPREL32)
            | (type == R_MIPS_TLS_DTPREL32)
#endif /* DTPREL32 */
            );
        break;
#endif /* MIPS case */
#if defined(EM_SPARC32PLUS)  && defined (R_SPARC_UA32)
    case EM_SPARC32PLUS:
        r = (0
#if defined(R_SPARC_UA32)
            | (type == R_SPARC_UA32)
#endif
#if defined(R_SPARC_TLS_DTPOFF32)
            | (type == R_SPARC_TLS_DTPOFF32)
#endif
            );
        break;
#endif
#if defined(EM_SPARCV9)  && defined (R_SPARC_UA32)
    case EM_SPARCV9:
        r =  (type == R_SPARC_UA32);
        break;
#endif
#if defined(EM_SPARC) && defined (R_SPARC_UA32)
    case EM_SPARC:
        r =  (0
#if defined(R_SPARC_UA32)
            | (type == R_SPARC_UA32)
#endif
#if (R_SPARC_TLS_DTPOFF32)
            | (type == R_SPARC_TLS_DTPOFF32)
#endif
            );
        break;
#endif /* EM_SPARC */
#if defined(EM_386) && defined (R_386_32) && defined (R_386_PC32)
    case EM_386:
        r = (0
#if defined (R_386_32)
            |  (type == R_386_32)
#endif
#if defined (R_386_GOTOFF)
            |  (type == R_386_GOTOFF)
#endif
#if defined (R_386_GOTPC)
            |  (type == R_386_GOTPC)
#endif
#if defined (R_386_PC32)
            |  (type == R_386_PC32)
#endif
#if defined (R_386_TLS_LDO_32)
            | (type == R_386_TLS_LDO_32)
#endif
#if defined (R_386_TLS_DTPOFF32)
            | (type == R_386_TLS_DTPOFF32)
#endif
            );
        break;
#endif /* EM_386 */

#if defined (EM_SH) && defined (R_SH_DIR32)
    case EM_SH:
        r = (0
#if defined (R_SH_DIR32)
            | (type == R_SH_DIR32)
#endif
#if defined (R_SH_DTPOFF32)
            | (type == R_SH_TLS_DTPOFF32)
#endif
            );
        break;
#endif /* SH */

#if defined(EM_IA_64) && defined (R_IA64_SECREL32LSB)
    case EM_IA_64:  /* 32bit? ! */
        r = (0
#if defined (R_IA64_SECREL32LSB)
            | (type == R_IA64_SECREL32LSB)
#endif
#if defined (R_IA64_DIR32LSB)
            | (type == R_IA64_DIR32LSB)
#endif
#if defined (R_IA64_DTPREL32LSB)
            | (type == R_IA64_DTPREL32LSB)
#endif
            );
        break;
#endif /* EM_IA_64 */

#if defined(EM_ARM) && defined (R_ARM_ABS32)
    case EM_ARM:
    case EM_AARCH64:
        r = (0
#if defined (R_ARM_ABS32)
            | ( type == R_ARM_ABS32)
#endif
#if defined (R_AARCH64_ABS32)
            | ( type == R_AARCH64_ABS32)
#endif
#if defined (R_ARM_TLS_LDO32)
            | ( type == R_ARM_TLS_LDO32)
#endif
            );
        break;
#endif /* EM_ARM */

/*  On FreeBSD xR_PPC64_ADDR32 not defined
    so we use the xR_PPC_ names which
    have the proper value.
    Our headers have:
    xR_PPC64_ADDR64   38
    xR_PPC_ADDR32     1 so we use this one
    xR_PPC64_ADDR32   R_PPC_ADDR32

    xR_PPC64_DTPREL32 110  which may be wrong/unavailable
    xR_PPC64_DTPREL64 78
    xR_PPC_DTPREL32   78
    */
#if defined(EM_PPC64) && defined (R_PPC_ADDR32)
    case EM_PPC64:
        r = (0
#if defined(R_PPC_ADDR32)
            | (type == R_PPC_ADDR32)
#endif
#if defined(R_PPC64_DTPREL32)
            | (type == R_PPC64_DTPREL32)
#endif
            );
        break;
#endif /* EM_PPC64 */

#if defined(EM_PPC) && defined (R_PPC_ADDR32)
    case EM_PPC:
        r = (0
#if defined (R_PPC_ADDR32)
            | (type == R_PPC_ADDR32)
#endif
#if defined (R_PPC_DTPREL32)
            | (type == R_PPC_DTPREL32)
#endif
            );
        break;
#endif /* EM_PPC */

#if defined(EM_S390) && defined (R_390_32)
    case EM_S390:
        r = (0
#if defined (R_390_32)
            | (type == R_390_32)
#endif
#if defined (R_390_TLS_LDO32)
            | (type == R_390_TLS_LDO32)
#endif
            );
        break;
#endif /* EM_S390 */

#if defined(EM_X86_64) && \
    ( defined(R_X86_64_32) || defined(R_X86_64_PC32) ||\
    defined(R_X86_64_DTPOFF32) )
#if defined(EM_K10M)
    case EM_K10M:
#endif
#if defined(EM_L10M)
    case EM_L10M:
#endif
    case EM_X86_64:
        r = (0
#if defined (R_X86_64_PC32)
            | (type == R_X86_64_PC32)
#endif
#if defined (R_X86_64_32)
            | (type == R_X86_64_32)
#endif
#if defined (R_X86_64_DTPOFF32)
            | (type ==  R_X86_64_DTPOFF32)
#endif
            );
        break;
#endif /* EM_X86_64 */

    case  EM_QUALCOMM_DSP6:
        r = (type == R_QUALCOMM_REL32);
        break;
    default: break;
    }
    return r;
}

unsigned
_dwarf_is_64bit_abs_reloc(unsigned int type, unsigned machine)
{
    unsigned r = 0;

    switch (machine) {
#if defined(EM_MIPS) && defined (R_MIPS_64)
    case EM_MIPS:
        r = (0
#if defined (R_MIPS_64)
            | (type == R_MIPS_64)
#endif
#if defined (R_MIPS_32)
            | (type == R_MIPS_32)
#endif
#if defined(R_MIPS_TLS_DTPREL64)
            | (type == R_MIPS_TLS_DTPREL64)
#endif
            );
        break;
#endif /* EM_MIPS */
#if defined(EM_SPARC32PLUS) && defined (R_SPARC_UA64)
    case EM_SPARC32PLUS:
        r =  (type == R_SPARC_UA64);
        break;
#endif
#if defined(EM_SPARCV9) && defined (R_SPARC_UA64)
    case EM_SPARCV9:
        r = (0
#if defined (R_SPARC_UA64)
            | (type == R_SPARC_UA64)
#endif
#if defined (R_SPARC_TLS_DTPOFF64)
            | (type == R_SPARC_TLS_DTPOFF64)
#endif
            );
        break;
#endif
#if defined(EM_SPARC) && defined (R_SPARC_UA64)
    case EM_SPARC:
        r = (0
#if defined(R_SPARC_UA64)
            | (type == R_SPARC_UA64)
#endif
#if defined (R_SPARC_TLS_DTPOFF64)
            | (type == R_SPARC_TLS_DTPOFF64)
#endif
            );
        break;
#endif /* EM_SPARC */

#if defined(EM_IA_64) && defined (R_IA64_SECREL64LSB)
    case EM_IA_64: /* 64bit */
        r = (0
#if defined (R_IA64_SECREL64LSB)
            | (type == R_IA64_SECREL64LSB)
#endif
#if defined (R_IA64_SECREL32LSB)
            | (type == R_IA64_SECREL32LSB)
#endif
#if defined (R_IA64_DIR64LSB)
            | (type == R_IA64_DIR64LSB)
#endif
#if defined (R_IA64_DTPREL64LSB)
            | (type == R_IA64_DTPREL64LSB)
#endif
#if defined (R_IA64_REL32LSB)
            | (type == R_IA64_REL32LSB)
#endif
            );
        break;
#endif /* EM_IA_64 */

#if defined(EM_PPC64) && defined (R_PPC64_ADDR64)
    case EM_PPC64:
        r = (0
#if defined(R_PPC64_ADDR64)
            | (type == R_PPC64_ADDR64)
#endif
#if defined(R_PPC64_DTPREL64)
            | (type == R_PPC64_DTPREL64)
#endif
            );
        break;
#endif /* EM_PPC64 */

#if defined(EM_S390) && defined (R_390_64)
    case EM_S390:
        r = (0
#if defined(R_390_64)
            | (type == R_390_64)
#endif
#if defined(R_390_TLS_LDO64)
            | (type == R_390_TLS_LDO64)
#endif
            );
        break;
#endif /* EM_390 */

#if defined(EM_X86_64) && defined (R_X86_64_64)
#if defined(EM_K10M)
    case EM_K10M:
#endif
#if defined(EM_L10M)
    case EM_L10M:
#endif
    case EM_X86_64:
        r = (0
#if defined (R_X86_64_64)
            | (type == R_X86_64_64)
#endif
#if defined (R_X86_64_PC64)
            | (type == R_X86_64_PC64)
#endif
#if defined (R_X86_64_DTPOFF32)
            | (type == R_X86_64_DTPOFF64)
#endif
            );
        break;
#endif /* EM_X86_64 */
#if defined(EM_AARCH64) && defined (R_AARCH64_ABS64)
    case EM_AARCH64:
        r = (0
#if defined (R_AARCH64_ABS64)
            | ( type == R_AARCH64_ABS64)
#endif
            );
        break;
#endif /* EM_AARCH64 */
    default: break;
    }
    return r;
}
