/*
Copyright (c) 2021-2023, David Anderson
All rights reserved.
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

/*  Typed in from the SystemV Application Binary Interface
    but using char arrays instead of variables as
    for reading we don't need the struct members to be
    variables. This simplifies configure.

    https://www.uclibc.org/docs/elf-64-gen.pdf used as source
    of Elf64 fields.

    It is expected code including this will have included
    an official <elf.h> (for various definitions needed)
    before including this. But that is not strictly necessary
    given other headers.

    The structs were all officially defined so files
    could be mapped in. Fields are arranged so
    there will not be gaps and we need not deal with
    alignment-gaps.
*/

#ifndef DW_ELFSTRUCTS_H
#define DW_ELFSTRUCTS_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef EI_NIDENT
#define EI_NIDENT 16
#endif

#ifndef TYP
#define TYP(n,l) char (n)[(l)]
#endif

typedef struct
{
    unsigned char e_ident[EI_NIDENT];
    TYP(e_type,2);
    TYP(e_machine,2);
    TYP(e_version,4);
    TYP(e_entry,4);
    TYP(e_phoff,4);
    TYP(e_shoff,4);
    TYP(e_flags,4);
    TYP(e_ehsize,2);
    TYP(e_phentsize,2);
    TYP(e_phnum,2);
    TYP(e_shentsize,2);
    TYP(e_shnum,2);
    TYP(e_shstrndx,2);
} dw_elf32_ehdr;

typedef struct
{
    unsigned char e_ident[EI_NIDENT];
    TYP(e_type,2);
    TYP(e_machine,2);
    TYP(e_version,4);
    TYP(e_entry,8);
    TYP(e_phoff,8);
    TYP(e_shoff,8);
    TYP(e_flags,4);
    TYP(e_ehsize,2);
    TYP(e_phentsize,2);
    TYP(e_phnum,2);
    TYP(e_shentsize,2);
    TYP(e_shnum,2);
    TYP(e_shstrndx,2);
} dw_elf64_ehdr;

typedef struct
{
    TYP(p_type,4);
    TYP(p_offset,4);
    TYP(p_vaddr,4);
    TYP(p_paddr,4);
    TYP(p_filesz,4);
    TYP(p_memsz,4);
    TYP(p_flags,4);
    TYP(p_align,4);
} dw_elf32_phdr;

typedef struct
{
    TYP(p_type,4);
    TYP(p_flags,4);
    TYP(p_offset,8);
    TYP(p_vaddr,8);
    TYP(p_paddr,8);
    TYP(p_filesz,8);
    TYP(p_memsz,8);
    TYP(p_align,8);
} dw_elf64_phdr;

typedef struct
{
    TYP(sh_name,4);
    TYP(sh_type,4);
    TYP(sh_flags,4);
    TYP(sh_addr,4);
    TYP(sh_offset,4);
    TYP(sh_size,4);
    TYP(sh_link,4);
    TYP(sh_info,4);
    TYP(sh_addralign,4);
    TYP(sh_entsize,4);
} dw_elf32_shdr;

typedef struct
{
    TYP(sh_name,4);
    TYP(sh_type,4);
    TYP(sh_flags,8);
    TYP(sh_addr,8);
    TYP(sh_offset,8);
    TYP(sh_size,8);
    TYP(sh_link,4);
    TYP(sh_info,4);
    TYP(sh_addralign,8);
    TYP(sh_entsize,8);
} dw_elf64_shdr;

typedef struct
{
    TYP(r_offset,4);
    TYP(r_info,4);
} dw_elf32_rel;

typedef struct
{
    TYP(r_offset,8);
    TYP(r_info,8);
} dw_elf64_rel;

typedef struct
{
    TYP(r_offset,4);
    TYP(r_info,4);
    TYP(r_addend,4); /* signed */
} dw_elf32_rela;

typedef struct
{
    TYP(r_offset,8);
    TYP(r_info,8);
    TYP(r_addend,8); /* signed */
} dw_elf64_rela;

typedef struct {
    TYP(st_name,4);
    TYP(st_value,4);
    TYP(st_size,4);
    unsigned char st_info[1];
    unsigned char st_other[1];
    TYP(st_shndx,2);
} dw_elf32_sym;

typedef struct {
    TYP(st_name,4);
    unsigned char st_info[1];
    unsigned char st_other[1];
    TYP(st_shndx,2);
    TYP(st_value,8);
    TYP(st_size,8);
} dw_elf64_sym;

typedef struct
{
    TYP(d_tag,4); /* signed */
    TYP(d_val,4); /* Union in original */
} dw_elf32_dyn;

typedef struct
{
    TYP(d_tag,8); /* signed */
    TYP(d_val,8); /* Union in original */
} dw_elf64_dyn;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DW_ELFSTRUCTS_H */
