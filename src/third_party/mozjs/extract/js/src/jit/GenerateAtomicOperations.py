# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This script generates jit/AtomicOperationsGenerated.h
#
# See the big comment in jit/AtomicOperations.h for an explanation.

import buildconfig

is_64bit = "JS_64BIT" in buildconfig.defines
cpu_arch = buildconfig.substs["CPU_ARCH"]
is_gcc = buildconfig.substs["CC_TYPE"] == "gcc"
is_windows = buildconfig.substs["OS_ARCH"] == "WINNT"


def fmt_insn(s):
    return '"' + s + '\\n\\t"\n'


def gen_seqcst(fun_name):
    if cpu_arch in ("x86", "x86_64"):
        return r"""
            INLINE_ATTR void %(fun_name)s() {
                asm volatile ("mfence\n\t" ::: "memory");
            }""" % {
            "fun_name": fun_name,
        }
    if cpu_arch == "aarch64":
        return r"""
            INLINE_ATTR void %(fun_name)s() {
                asm volatile ("dmb ish\n\t" ::: "memory");
            }""" % {
            "fun_name": fun_name,
        }
    if cpu_arch == "arm":
        return r"""
            INLINE_ATTR void %(fun_name)s() {
                asm volatile ("dmb sy\n\t" ::: "memory");
            }""" % {
            "fun_name": fun_name,
        }
    raise Exception("Unexpected arch")


def gen_load(fun_name, cpp_type, size, barrier):
    # NOTE: the assembly code must match the generated code in:
    # - CacheIRCompiler::emitAtomicsLoadResult
    # - LIRGenerator::visitLoadUnboxedScalar
    # - CodeGenerator::visitAtomicLoad64 (on 64-bit platforms)
    # - MacroAssembler::wasmLoad
    if cpu_arch in ("x86", "x86_64"):
        insns = ""
        if barrier:
            insns += fmt_insn("mfence")
        if size == 8:
            insns += fmt_insn("movb (%[arg]), %[res]")
        elif size == 16:
            insns += fmt_insn("movw (%[arg]), %[res]")
        elif size == 32:
            insns += fmt_insn("movl (%[arg]), %[res]")
        else:
            assert size == 64
            insns += fmt_insn("movq (%[arg]), %[res]")
        if barrier:
            insns += fmt_insn("mfence")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(const %(cpp_type)s* arg) {
                %(cpp_type)s res;
                asm volatile (%(insns)s
                    : [res] "=r" (res)
                    : [arg] "r" (arg)
                    : "memory");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "aarch64":
        insns = ""
        if barrier:
            insns += fmt_insn("dmb ish")
        if size == 8:
            insns += fmt_insn("ldrb %w[res], [%x[arg]]")
        elif size == 16:
            insns += fmt_insn("ldrh %w[res], [%x[arg]]")
        elif size == 32:
            insns += fmt_insn("ldr %w[res], [%x[arg]]")
        else:
            assert size == 64
            insns += fmt_insn("ldr %x[res], [%x[arg]]")
        if barrier:
            insns += fmt_insn("dmb ish")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(const %(cpp_type)s* arg) {
                %(cpp_type)s res;
                asm volatile (%(insns)s
                    : [res] "=r" (res)
                    : [arg] "r" (arg)
                    : "memory");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "arm":
        insns = ""
        if barrier:
            insns += fmt_insn("dmb sy")
        if size == 8:
            insns += fmt_insn("ldrb %[res], [%[arg]]")
        elif size == 16:
            insns += fmt_insn("ldrh %[res], [%[arg]]")
        else:
            assert size == 32
            insns += fmt_insn("ldr %[res], [%[arg]]")
        if barrier:
            insns += fmt_insn("dmb sy")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(const %(cpp_type)s* arg) {
                %(cpp_type)s res;
                asm volatile (%(insns)s
                    : [res] "=r" (res)
                    : [arg] "r" (arg)
                    : "memory");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    raise Exception("Unexpected arch")


def gen_store(fun_name, cpp_type, size, barrier):
    # NOTE: the assembly code must match the generated code in:
    # - CacheIRCompiler::emitAtomicsStoreResult
    # - LIRGenerator::visitStoreUnboxedScalar
    # - CodeGenerator::visitAtomicStore64 (on 64-bit platforms)
    # - MacroAssembler::wasmStore
    if cpu_arch in ("x86", "x86_64"):
        insns = ""
        if barrier:
            insns += fmt_insn("mfence")
        if size == 8:
            insns += fmt_insn("movb %[val], (%[addr])")
        elif size == 16:
            insns += fmt_insn("movw %[val], (%[addr])")
        elif size == 32:
            insns += fmt_insn("movl %[val], (%[addr])")
        else:
            assert size == 64
            insns += fmt_insn("movq %[val], (%[addr])")
        if barrier:
            insns += fmt_insn("mfence")
        return """
            INLINE_ATTR void %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                asm volatile (%(insns)s
                    :
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory");
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "aarch64":
        insns = ""
        if barrier:
            insns += fmt_insn("dmb ish")
        if size == 8:
            insns += fmt_insn("strb %w[val], [%x[addr]]")
        elif size == 16:
            insns += fmt_insn("strh %w[val], [%x[addr]]")
        elif size == 32:
            insns += fmt_insn("str %w[val], [%x[addr]]")
        else:
            assert size == 64
            insns += fmt_insn("str %x[val], [%x[addr]]")
        if barrier:
            insns += fmt_insn("dmb ish")
        return """
            INLINE_ATTR void %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                asm volatile (%(insns)s
                    :
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory");
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "arm":
        insns = ""
        if barrier:
            insns += fmt_insn("dmb sy")
        if size == 8:
            insns += fmt_insn("strb %[val], [%[addr]]")
        elif size == 16:
            insns += fmt_insn("strh %[val], [%[addr]]")
        else:
            assert size == 32
            insns += fmt_insn("str %[val], [%[addr]]")
        if barrier:
            insns += fmt_insn("dmb sy")
        return """
            INLINE_ATTR void %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                asm volatile (%(insns)s
                    :
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory");
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    raise Exception("Unexpected arch")


def gen_exchange(fun_name, cpp_type, size):
    # NOTE: the assembly code must match the generated code in:
    # - MacroAssembler::atomicExchange
    # - MacroAssembler::atomicExchange64 (on 64-bit platforms)
    if cpu_arch in ("x86", "x86_64"):
        # Request an input/output register for `val` so that we can simply XCHG it
        # with *addr.
        insns = ""
        if size == 8:
            insns += fmt_insn("xchgb %[val], (%[addr])")
        elif size == 16:
            insns += fmt_insn("xchgw %[val], (%[addr])")
        elif size == 32:
            insns += fmt_insn("xchgl %[val], (%[addr])")
        else:
            assert size == 64
            insns += fmt_insn("xchgq %[val], (%[addr])")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                asm volatile (%(insns)s
                    : [val] "+r" (val)
                    : [addr] "r" (addr)
                    : "memory");
                return val;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "aarch64":
        insns = ""
        insns += fmt_insn("dmb ish")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("ldxrb %w[res], [%x[addr]]")
            insns += fmt_insn("stxrb %w[scratch], %w[val], [%x[addr]]")
        elif size == 16:
            insns += fmt_insn("ldxrh %w[res], [%x[addr]]")
            insns += fmt_insn("stxrh %w[scratch], %w[val], [%x[addr]]")
        elif size == 32:
            insns += fmt_insn("ldxr %w[res], [%x[addr]]")
            insns += fmt_insn("stxr %w[scratch], %w[val], [%x[addr]]")
        else:
            assert size == 64
            insns += fmt_insn("ldxr %x[res], [%x[addr]]")
            insns += fmt_insn("stxr %w[scratch], %x[val], [%x[addr]]")
        insns += fmt_insn("cbnz %w[scratch], 0b")
        insns += fmt_insn("dmb ish")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                %(cpp_type)s res;
                uint32_t scratch;
                asm volatile (%(insns)s
                    : [res] "=&r"(res), [scratch] "=&r"(scratch)
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "arm":
        insns = ""
        insns += fmt_insn("dmb sy")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("ldrexb %[res], [%[addr]]")
            insns += fmt_insn("strexb %[scratch], %[val], [%[addr]]")
        elif size == 16:
            insns += fmt_insn("ldrexh %[res], [%[addr]]")
            insns += fmt_insn("strexh %[scratch], %[val], [%[addr]]")
        else:
            assert size == 32
            insns += fmt_insn("ldrex %[res], [%[addr]]")
            insns += fmt_insn("strex %[scratch], %[val], [%[addr]]")
        insns += fmt_insn("cmp %[scratch], #1")
        insns += fmt_insn("beq 0b")
        insns += fmt_insn("dmb sy")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                %(cpp_type)s res;
                uint32_t scratch;
                asm volatile (%(insns)s
                    : [res] "=&r"(res), [scratch] "=&r"(scratch)
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    raise Exception("Unexpected arch")


def gen_cmpxchg(fun_name, cpp_type, size):
    # NOTE: the assembly code must match the generated code in:
    # - MacroAssembler::compareExchange
    # - MacroAssembler::compareExchange64
    if cpu_arch == "x86" and size == 64:
        # Use a +A constraint to load `oldval` into EDX:EAX as input/output.
        # `newval` is loaded into ECX:EBX.
        return r"""
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr,
                                             %(cpp_type)s oldval,
                                             %(cpp_type)s newval) {
                asm volatile ("lock; cmpxchg8b (%%[addr])\n\t"
                : "+A" (oldval)
                : [addr] "r" (addr),
                  "b" (uint32_t(newval & 0xffff'ffff)),
                  "c" (uint32_t(newval >> 32))
                : "memory", "cc");
                return oldval;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
        }
    if cpu_arch == "arm" and size == 64:
        return r"""
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr,
                                             %(cpp_type)s oldval,
                                             %(cpp_type)s newval) {
                uint32_t oldval0 = oldval & 0xffff'ffff;
                uint32_t oldval1 = oldval >> 32;
                uint32_t newval0 = newval & 0xffff'ffff;
                uint32_t newval1 = newval >> 32;
                asm volatile (
                    "dmb sy\n\t"
                    "0: ldrexd r0, r1, [%%[addr]]\n\t"
                    "cmp r0, %%[oldval0]\n\t"
                    "bne 1f\n\t"
                    "cmp r1, %%[oldval1]\n\t"
                    "bne 1f\n\t"
                    "mov r2, %%[newval0]\n\t"
                    "mov r3, %%[newval1]\n\t"
                    "strexd r4, r2, r3, [%%[addr]]\n\t"
                    "cmp r4, #1\n\t"
                    "beq 0b\n\t"
                    "1: dmb sy\n\t"
                    "mov %%[oldval0], r0\n\t"
                    "mov %%[oldval1], r1\n\t"
                    : [oldval0] "+&r" (oldval0), [oldval1] "+&r"(oldval1)
                    : [addr] "r" (addr), [newval0] "r" (newval0), [newval1] "r" (newval1)
                    : "memory", "cc", "r0", "r1", "r2", "r3", "r4");
                return uint64_t(oldval0) | (uint64_t(oldval1) << 32);
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
        }
    if cpu_arch in ("x86", "x86_64"):
        # Use a +a constraint to load `oldval` into RAX as input/output register.
        insns = ""
        if size == 8:
            insns += fmt_insn("lock; cmpxchgb %[newval], (%[addr])")
        elif size == 16:
            insns += fmt_insn("lock; cmpxchgw %[newval], (%[addr])")
        elif size == 32:
            insns += fmt_insn("lock; cmpxchgl %[newval], (%[addr])")
        else:
            assert size == 64
            insns += fmt_insn("lock; cmpxchgq %[newval], (%[addr])")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr,
                                             %(cpp_type)s oldval,
                                             %(cpp_type)s newval) {
                asm volatile (%(insns)s
                    : [oldval] "+a" (oldval)
                    : [addr] "r" (addr), [newval] "r" (newval)
                    : "memory", "cc");
                return oldval;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "aarch64":
        insns = ""
        insns += fmt_insn("dmb ish")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("uxtb %w[scratch], %w[oldval]")
            insns += fmt_insn("ldxrb %w[res], [%x[addr]]")
            insns += fmt_insn("cmp %w[res], %w[scratch]")
            insns += fmt_insn("b.ne 1f")
            insns += fmt_insn("stxrb %w[scratch], %w[newval], [%x[addr]]")
        elif size == 16:
            insns += fmt_insn("uxth %w[scratch], %w[oldval]")
            insns += fmt_insn("ldxrh %w[res], [%x[addr]]")
            insns += fmt_insn("cmp %w[res], %w[scratch]")
            insns += fmt_insn("b.ne 1f")
            insns += fmt_insn("stxrh %w[scratch], %w[newval], [%x[addr]]")
        elif size == 32:
            insns += fmt_insn("mov %w[scratch], %w[oldval]")
            insns += fmt_insn("ldxr %w[res], [%x[addr]]")
            insns += fmt_insn("cmp %w[res], %w[scratch]")
            insns += fmt_insn("b.ne 1f")
            insns += fmt_insn("stxr %w[scratch], %w[newval], [%x[addr]]")
        else:
            assert size == 64
            insns += fmt_insn("mov %x[scratch], %x[oldval]")
            insns += fmt_insn("ldxr %x[res], [%x[addr]]")
            insns += fmt_insn("cmp %x[res], %x[scratch]")
            insns += fmt_insn("b.ne 1f")
            insns += fmt_insn("stxr %w[scratch], %x[newval], [%x[addr]]")
        insns += fmt_insn("cbnz %w[scratch], 0b")
        insns += fmt_insn("1: dmb ish")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr,
                                             %(cpp_type)s oldval,
                                             %(cpp_type)s newval) {
                %(cpp_type)s res, scratch;
                asm volatile (%(insns)s
                    : [res] "=&r" (res), [scratch] "=&r" (scratch)
                    : [addr] "r" (addr), [oldval] "r"(oldval), [newval] "r" (newval)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "arm":
        insns = ""
        insns += fmt_insn("dmb sy")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("uxtb %[scratch], %[oldval]")
            insns += fmt_insn("ldrexb %[res], [%[addr]]")
            insns += fmt_insn("cmp %[res], %[scratch]")
            insns += fmt_insn("bne 1f")
            insns += fmt_insn("strexb %[scratch], %[newval], [%[addr]]")
        elif size == 16:
            insns += fmt_insn("uxth %[scratch], %[oldval]")
            insns += fmt_insn("ldrexh %[res], [%[addr]]")
            insns += fmt_insn("cmp %[res], %[scratch]")
            insns += fmt_insn("bne 1f")
            insns += fmt_insn("strexh %[scratch], %[newval], [%[addr]]")
        else:
            assert size == 32
            insns += fmt_insn("mov %[scratch], %[oldval]")
            insns += fmt_insn("ldrex %[res], [%[addr]]")
            insns += fmt_insn("cmp %[res], %[scratch]")
            insns += fmt_insn("bne 1f")
            insns += fmt_insn("strex %[scratch], %[newval], [%[addr]]")
        insns += fmt_insn("cmp %[scratch], #1")
        insns += fmt_insn("beq 0b")
        insns += fmt_insn("1: dmb sy")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr,
                                             %(cpp_type)s oldval,
                                             %(cpp_type)s newval) {
                %(cpp_type)s res, scratch;
                asm volatile (%(insns)s
                    : [res] "=&r" (res), [scratch] "=&r" (scratch)
                    : [addr] "r" (addr), [oldval] "r"(oldval), [newval] "r" (newval)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    raise Exception("Unexpected arch")


def gen_fetchop(fun_name, cpp_type, size, op):
    # NOTE: the assembly code must match the generated code in:
    # - MacroAssembler::atomicFetchOp
    # - MacroAssembler::atomicFetchOp64 (on 64-bit platforms)
    if cpu_arch in ("x86", "x86_64"):
        # The `add` operation can be optimized with XADD.
        if op == "add":
            insns = ""
            if size == 8:
                insns += fmt_insn("lock; xaddb %[val], (%[addr])")
            elif size == 16:
                insns += fmt_insn("lock; xaddw %[val], (%[addr])")
            elif size == 32:
                insns += fmt_insn("lock; xaddl %[val], (%[addr])")
            else:
                assert size == 64
                insns += fmt_insn("lock; xaddq %[val], (%[addr])")
            return """
                INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                    asm volatile (%(insns)s
                        : [val] "+&r" (val)
                        : [addr] "r" (addr)
                        : "memory", "cc");
                    return val;
                }""" % {
                "cpp_type": cpp_type,
                "fun_name": fun_name,
                "insns": insns,
            }
        # Use a +a constraint to ensure `res` is stored in RAX. This is required
        # for the CMPXCHG instruction.
        insns = ""
        if size == 8:
            insns += fmt_insn("movb (%[addr]), %[res]")
            insns += fmt_insn("0: movb %[res], %[scratch]")
            insns += fmt_insn("OPb %[val], %[scratch]")
            insns += fmt_insn("lock; cmpxchgb %[scratch], (%[addr])")
        elif size == 16:
            insns += fmt_insn("movw (%[addr]), %[res]")
            insns += fmt_insn("0: movw %[res], %[scratch]")
            insns += fmt_insn("OPw %[val], %[scratch]")
            insns += fmt_insn("lock; cmpxchgw %[scratch], (%[addr])")
        elif size == 32:
            insns += fmt_insn("movl (%[addr]), %[res]")
            insns += fmt_insn("0: movl %[res], %[scratch]")
            insns += fmt_insn("OPl %[val], %[scratch]")
            insns += fmt_insn("lock; cmpxchgl %[scratch], (%[addr])")
        else:
            assert size == 64
            insns += fmt_insn("movq (%[addr]), %[res]")
            insns += fmt_insn("0: movq %[res], %[scratch]")
            insns += fmt_insn("OPq %[val], %[scratch]")
            insns += fmt_insn("lock; cmpxchgq %[scratch], (%[addr])")
        insns = insns.replace("OP", op)
        insns += fmt_insn("jnz 0b")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                %(cpp_type)s res, scratch;
                asm volatile (%(insns)s
                    : [res] "=&a" (res), [scratch] "=&r" (scratch)
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "aarch64":
        insns = ""
        insns += fmt_insn("dmb ish")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("ldxrb %w[res], [%x[addr]]")
            insns += fmt_insn("OP %x[scratch1], %x[res], %x[val]")
            insns += fmt_insn("stxrb %w[scratch2], %w[scratch1], [%x[addr]]")
        elif size == 16:
            insns += fmt_insn("ldxrh %w[res], [%x[addr]]")
            insns += fmt_insn("OP %x[scratch1], %x[res], %x[val]")
            insns += fmt_insn("stxrh %w[scratch2], %w[scratch1], [%x[addr]]")
        elif size == 32:
            insns += fmt_insn("ldxr %w[res], [%x[addr]]")
            insns += fmt_insn("OP %x[scratch1], %x[res], %x[val]")
            insns += fmt_insn("stxr %w[scratch2], %w[scratch1], [%x[addr]]")
        else:
            assert size == 64
            insns += fmt_insn("ldxr %x[res], [%x[addr]]")
            insns += fmt_insn("OP %x[scratch1], %x[res], %x[val]")
            insns += fmt_insn("stxr %w[scratch2], %x[scratch1], [%x[addr]]")
        cpu_op = op
        if cpu_op == "or":
            cpu_op = "orr"
        if cpu_op == "xor":
            cpu_op = "eor"
        insns = insns.replace("OP", cpu_op)
        insns += fmt_insn("cbnz %w[scratch2], 0b")
        insns += fmt_insn("dmb ish")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                %(cpp_type)s res;
                uintptr_t scratch1, scratch2;
                asm volatile (%(insns)s
                    : [res] "=&r" (res), [scratch1] "=&r" (scratch1), [scratch2] "=&r"(scratch2)
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    if cpu_arch == "arm":
        insns = ""
        insns += fmt_insn("dmb sy")
        insns += fmt_insn("0:")
        if size == 8:
            insns += fmt_insn("ldrexb %[res], [%[addr]]")
            insns += fmt_insn("OP %[scratch1], %[res], %[val]")
            insns += fmt_insn("strexb %[scratch2], %[scratch1], [%[addr]]")
        elif size == 16:
            insns += fmt_insn("ldrexh %[res], [%[addr]]")
            insns += fmt_insn("OP %[scratch1], %[res], %[val]")
            insns += fmt_insn("strexh %[scratch2], %[scratch1], [%[addr]]")
        else:
            assert size == 32
            insns += fmt_insn("ldrex %[res], [%[addr]]")
            insns += fmt_insn("OP %[scratch1], %[res], %[val]")
            insns += fmt_insn("strex %[scratch2], %[scratch1], [%[addr]]")
        cpu_op = op
        if cpu_op == "or":
            cpu_op = "orr"
        if cpu_op == "xor":
            cpu_op = "eor"
        insns = insns.replace("OP", cpu_op)
        insns += fmt_insn("cmp %[scratch2], #1")
        insns += fmt_insn("beq 0b")
        insns += fmt_insn("dmb sy")
        return """
            INLINE_ATTR %(cpp_type)s %(fun_name)s(%(cpp_type)s* addr, %(cpp_type)s val) {
                %(cpp_type)s res;
                uintptr_t scratch1, scratch2;
                asm volatile (%(insns)s
                    : [res] "=&r" (res), [scratch1] "=&r" (scratch1), [scratch2] "=&r"(scratch2)
                    : [addr] "r" (addr), [val] "r"(val)
                    : "memory", "cc");
                return res;
            }""" % {
            "cpp_type": cpp_type,
            "fun_name": fun_name,
            "insns": insns,
        }
    raise Exception("Unexpected arch")


def gen_copy(fun_name, cpp_type, size, unroll, direction):
    assert direction in ("down", "up")
    offset = 0
    if direction == "up":
        offset = unroll - 1
    insns = ""
    for i in range(unroll):
        if cpu_arch in ("x86", "x86_64"):
            if size == 1:
                insns += fmt_insn("movb OFFSET(%[src]), %[scratch]")
                insns += fmt_insn("movb %[scratch], OFFSET(%[dst])")
            elif size == 4:
                insns += fmt_insn("movl OFFSET(%[src]), %[scratch]")
                insns += fmt_insn("movl %[scratch], OFFSET(%[dst])")
            else:
                assert size == 8
                insns += fmt_insn("movq OFFSET(%[src]), %[scratch]")
                insns += fmt_insn("movq %[scratch], OFFSET(%[dst])")
        elif cpu_arch == "aarch64":
            if size == 1:
                insns += fmt_insn("ldrb %w[scratch], [%x[src], OFFSET]")
                insns += fmt_insn("strb %w[scratch], [%x[dst], OFFSET]")
            else:
                assert size == 8
                insns += fmt_insn("ldr %x[scratch], [%x[src], OFFSET]")
                insns += fmt_insn("str %x[scratch], [%x[dst], OFFSET]")
        elif cpu_arch == "arm":
            if size == 1:
                insns += fmt_insn("ldrb %[scratch], [%[src], #OFFSET]")
                insns += fmt_insn("strb %[scratch], [%[dst], #OFFSET]")
            else:
                assert size == 4
                insns += fmt_insn("ldr %[scratch], [%[src], #OFFSET]")
                insns += fmt_insn("str %[scratch], [%[dst], #OFFSET]")
        else:
            raise Exception("Unexpected arch")
        insns = insns.replace("OFFSET", str(offset * size))

        if direction == "down":
            offset += 1
        else:
            offset -= 1

    return """
        INLINE_ATTR void %(fun_name)s(uint8_t* dst, const uint8_t* src) {
            %(cpp_type)s* dst_ = reinterpret_cast<%(cpp_type)s*>(dst);
            const %(cpp_type)s* src_ = reinterpret_cast<const %(cpp_type)s*>(src);
            %(cpp_type)s scratch;
            asm volatile (%(insns)s
                : [scratch] "=&r" (scratch)
                : [dst] "r" (dst_), [src] "r"(src_)
                : "memory");
        }""" % {
        "cpp_type": cpp_type,
        "fun_name": fun_name,
        "insns": insns,
    }


HEADER_TEMPLATE = """\
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AtomicOperationsGenerated_h
#define jit_AtomicOperationsGenerated_h

/* This file is generated by jit/GenerateAtomicOperations.py. Do not edit! */

#include "mozilla/Attributes.h"

namespace js {
namespace jit {

%(contents)s

} // namespace jit
} // namespace js

#endif // jit_AtomicOperationsGenerated_h
"""


def generate_atomics_header(c_out):
    # NOTE: This version of the script has been modified so that it _does not_ generate
    # an atomic operations header when targeting Windows. It assumes that the build will
    # use MSVC and the MSVC-specific atomic operations header, which may be unsuitable
    # for some uses.
    contents = ""
    if (cpu_arch in ("x86", "x86_64", "aarch64") and not is_windows) or (
        cpu_arch == "arm" and int(buildconfig.substs["ARM_ARCH"]) >= 7
    ):
        contents += "#define JS_HAVE_GENERATED_ATOMIC_OPS 1"

        # `fence` performs a full memory barrier.
        contents += gen_seqcst("AtomicFenceSeqCst")

        contents += gen_load("AtomicLoad8SeqCst", "uint8_t", 8, True)
        contents += gen_load("AtomicLoad16SeqCst", "uint16_t", 16, True)
        contents += gen_load("AtomicLoad32SeqCst", "uint32_t", 32, True)
        if is_64bit:
            contents += gen_load("AtomicLoad64SeqCst", "uint64_t", 64, True)

        # These are access-atomic up to sizeof(uintptr_t).
        contents += gen_load("AtomicLoad8Unsynchronized", "uint8_t", 8, False)
        contents += gen_load("AtomicLoad16Unsynchronized", "uint16_t", 16, False)
        contents += gen_load("AtomicLoad32Unsynchronized", "uint32_t", 32, False)
        if is_64bit:
            contents += gen_load("AtomicLoad64Unsynchronized", "uint64_t", 64, False)

        contents += gen_store("AtomicStore8SeqCst", "uint8_t", 8, True)
        contents += gen_store("AtomicStore16SeqCst", "uint16_t", 16, True)
        contents += gen_store("AtomicStore32SeqCst", "uint32_t", 32, True)
        if is_64bit:
            contents += gen_store("AtomicStore64SeqCst", "uint64_t", 64, True)

        # These are access-atomic up to sizeof(uintptr_t).
        contents += gen_store("AtomicStore8Unsynchronized", "uint8_t", 8, False)
        contents += gen_store("AtomicStore16Unsynchronized", "uint16_t", 16, False)
        contents += gen_store("AtomicStore32Unsynchronized", "uint32_t", 32, False)
        if is_64bit:
            contents += gen_store("AtomicStore64Unsynchronized", "uint64_t", 64, False)

        # `exchange` takes a cell address and a value.  It stores it in the cell and
        # returns the value previously in the cell.
        contents += gen_exchange("AtomicExchange8SeqCst", "uint8_t", 8)
        contents += gen_exchange("AtomicExchange16SeqCst", "uint16_t", 16)
        contents += gen_exchange("AtomicExchange32SeqCst", "uint32_t", 32)
        if is_64bit:
            contents += gen_exchange("AtomicExchange64SeqCst", "uint64_t", 64)

        # `cmpxchg` takes a cell address, an expected value and a replacement value.
        # If the value in the cell equals the expected value then the replacement value
        # is stored in the cell.  It always returns the value previously in the cell.
        contents += gen_cmpxchg("AtomicCmpXchg8SeqCst", "uint8_t", 8)
        contents += gen_cmpxchg("AtomicCmpXchg16SeqCst", "uint16_t", 16)
        contents += gen_cmpxchg("AtomicCmpXchg32SeqCst", "uint32_t", 32)
        contents += gen_cmpxchg("AtomicCmpXchg64SeqCst", "uint64_t", 64)

        # `add` adds a value atomically to the cell and returns the old value in the
        # cell.  (There is no `sub`; just add the negated value.)
        contents += gen_fetchop("AtomicAdd8SeqCst", "uint8_t", 8, "add")
        contents += gen_fetchop("AtomicAdd16SeqCst", "uint16_t", 16, "add")
        contents += gen_fetchop("AtomicAdd32SeqCst", "uint32_t", 32, "add")
        if is_64bit:
            contents += gen_fetchop("AtomicAdd64SeqCst", "uint64_t", 64, "add")

        # `and` bitwise-ands a value atomically into the cell and returns the old value
        # in the cell.
        contents += gen_fetchop("AtomicAnd8SeqCst", "uint8_t", 8, "and")
        contents += gen_fetchop("AtomicAnd16SeqCst", "uint16_t", 16, "and")
        contents += gen_fetchop("AtomicAnd32SeqCst", "uint32_t", 32, "and")
        if is_64bit:
            contents += gen_fetchop("AtomicAnd64SeqCst", "uint64_t", 64, "and")

        # `or` bitwise-ors a value atomically into the cell and returns the old value
        # in the cell.
        contents += gen_fetchop("AtomicOr8SeqCst", "uint8_t", 8, "or")
        contents += gen_fetchop("AtomicOr16SeqCst", "uint16_t", 16, "or")
        contents += gen_fetchop("AtomicOr32SeqCst", "uint32_t", 32, "or")
        if is_64bit:
            contents += gen_fetchop("AtomicOr64SeqCst", "uint64_t", 64, "or")

        # `xor` bitwise-xors a value atomically into the cell and returns the old value
        # in the cell.
        contents += gen_fetchop("AtomicXor8SeqCst", "uint8_t", 8, "xor")
        contents += gen_fetchop("AtomicXor16SeqCst", "uint16_t", 16, "xor")
        contents += gen_fetchop("AtomicXor32SeqCst", "uint32_t", 32, "xor")
        if is_64bit:
            contents += gen_fetchop("AtomicXor64SeqCst", "uint64_t", 64, "xor")

        # See comment in jit/AtomicOperations-shared-jit.cpp for an explanation.
        wordsize = 8 if is_64bit else 4
        words_in_block = 8
        blocksize = words_in_block * wordsize

        contents += gen_copy(
            "AtomicCopyUnalignedBlockDownUnsynchronized",
            "uint8_t",
            1,
            blocksize,
            "down",
        )
        contents += gen_copy(
            "AtomicCopyUnalignedBlockUpUnsynchronized", "uint8_t", 1, blocksize, "up"
        )

        contents += gen_copy(
            "AtomicCopyUnalignedWordDownUnsynchronized", "uint8_t", 1, wordsize, "down"
        )
        contents += gen_copy(
            "AtomicCopyUnalignedWordUpUnsynchronized", "uint8_t", 1, wordsize, "up"
        )

        contents += gen_copy(
            "AtomicCopyBlockDownUnsynchronized",
            "uintptr_t",
            wordsize,
            words_in_block,
            "down",
        )
        contents += gen_copy(
            "AtomicCopyBlockUpUnsynchronized",
            "uintptr_t",
            wordsize,
            words_in_block,
            "up",
        )

        contents += gen_copy(
            "AtomicCopyWordUnsynchronized", "uintptr_t", wordsize, 1, "down"
        )
        contents += gen_copy("AtomicCopyByteUnsynchronized", "uint8_t", 1, 1, "down")

        contents += "\n"
        contents += (
            "constexpr size_t JS_GENERATED_ATOMICS_BLOCKSIZE = "
            + str(blocksize)
            + ";\n"
        )
        contents += (
            "constexpr size_t JS_GENERATED_ATOMICS_WORDSIZE = " + str(wordsize) + ";\n"
        )

        # Work around a GCC issue on 32-bit x86 by adding MOZ_NEVER_INLINE.
        # See bug 1756347.
        if is_gcc and cpu_arch == "x86":
            contents = contents.replace("INLINE_ATTR", "MOZ_NEVER_INLINE inline")
        else:
            contents = contents.replace("INLINE_ATTR", "inline")

    c_out.write(
        HEADER_TEMPLATE
        % {
            "contents": contents,
        }
    )
