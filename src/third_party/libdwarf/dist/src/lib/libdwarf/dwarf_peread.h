#ifndef PE_GENERIC_H
#define PE_GENERIC_H
/*
Copyright (c) 2018-2023, David Anderson All rights reserved.

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
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dwarf_pe_generic_file_header
{
    Dwarf_Unsigned Machine;
    Dwarf_Unsigned NumberOfSections;
    Dwarf_Unsigned TimeDateStamp;
    Dwarf_Unsigned PointerToSymbolTable;
    Dwarf_Unsigned NumberOfSymbols;
    Dwarf_Unsigned SizeOfOptionalHeader; /* in object file */
    Dwarf_Unsigned Characteristics;
};

struct dwarf_pe_generic_data_directory
{
    Dwarf_Unsigned VirtualAddress;
    Dwarf_Unsigned Size;
};

#define DWARF_PE_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES 16
struct dwarf_pe_generic_optional_header
{
    Dwarf_Unsigned Magic;
    unsigned char MajorLinkerVersion;
    unsigned char MinorLinkerVersion;
    Dwarf_Unsigned SizeOfCode;
    Dwarf_Unsigned SizeOfInitializedData;
    Dwarf_Unsigned SizeOfUninitializedData;
    Dwarf_Unsigned AddressOfEntryPoint;
    Dwarf_Unsigned BaseOfCode;
    Dwarf_Unsigned BaseOfData;
    Dwarf_Unsigned ImageBase;
    Dwarf_Unsigned SectionAlignment;
    Dwarf_Unsigned FileAlignment;
    Dwarf_Unsigned MajorOperatingSystemVersion;
    Dwarf_Unsigned MinorOperatingSystemVersion;
    Dwarf_Unsigned MajorImageVersion;
    Dwarf_Unsigned MinorImageVersion;
    Dwarf_Unsigned MajorSubsystemVersion;
    Dwarf_Unsigned MinorSubsystemVersion;
    Dwarf_Unsigned Win32VersionValue;
    Dwarf_Unsigned SizeOfImage; /* size in object file */
    Dwarf_Unsigned SizeOfHeaders; /* size in object file */
    Dwarf_Unsigned CheckSum;
    Dwarf_Unsigned Subsystem;
    Dwarf_Unsigned DllCharacteristics;
    Dwarf_Unsigned SizeOfStackReserve;
    Dwarf_Unsigned SizeOfStackCommit;
    Dwarf_Unsigned SizeOfHeapReserve;
    Dwarf_Unsigned SizeOfHeapCommit;
    Dwarf_Unsigned LoaderFlags;
    Dwarf_Unsigned NumberOfRvaAndSizes;
    Dwarf_Unsigned SizeOfDataDirEntry; /* size in object file */
    struct dwarf_pe_generic_data_directory
        DataDirectory[DWARF_PE_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES];
};

struct dwarf_pe_generic_image_section_header
{
    char *name; /* Name must be freed */
    char *dwarfsectname; /* Name must be freed */
    Dwarf_Unsigned SecHeaderOffset; /* offset in object file */
        /* union { */
        /* Dwarf_Unsigned PhysicalAddress; */
    Dwarf_Unsigned VirtualSize;
        /* } Misc; */
    Dwarf_Unsigned VirtualAddress;
    Dwarf_Unsigned SizeOfRawData; /* size we need */
    Dwarf_Unsigned PointerToRawData;
    Dwarf_Unsigned PointerToRelocations;
    Dwarf_Unsigned PointerToLinenumbers;
    Dwarf_Unsigned NumberOfRelocations;
    Dwarf_Unsigned NumberOfLinenumbers;
    Dwarf_Unsigned Characteristics;
    Dwarf_Small *  loaded_data; /* must be freed. */
    Dwarf_Bool     section_irrelevant_to_dwarf;
};

#define DWARF_PE_IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define DWARF_PE_IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b
#define DWARF_PE_IMAGE_ROM_OPTIONAL_HDR_MAGIC 0x107

/*  ident[0] == 'P' means this is a PE header.
    ident[1] will be 1 indicating version 1.
    Other bytes in ident not defined, should be zero. */
typedef struct pe_filedata_s {
    char             pe_ident[8];
    const char *     pe_path; /* must free.*/
    int              pe_fd;
    int              pe_destruct_close_fd; /*aka: lib owns fd */
    int              pe_is_64bit;
    Dwarf_Unsigned   pe_filesize;
    Dwarf_Unsigned   pe_flags;
    Dwarf_Unsigned   pe_machine;
    Dwarf_Small      pe_offsetsize; /* 32 or 64 section data */
    Dwarf_Small      pe_pointersize;
    int              pe_ftype;
    unsigned         pe_endian;
    void (*pe_copy_word) (void *, const void *, unsigned long);
    Dwarf_Unsigned   pe_nt_header_offset;
    Dwarf_Unsigned   pe_optional_header_offset;
    Dwarf_Unsigned   pe_optional_header_size;
    Dwarf_Unsigned   pe_symbol_table_offset;
    Dwarf_Unsigned   pe_string_table_offset;
    Dwarf_Unsigned   pe_section_table_offset;
    Dwarf_Unsigned pe_signature;

    struct dwarf_pe_generic_file_header pe_FileHeader;

    struct dwarf_pe_generic_optional_header pe_OptionalHeader;

    Dwarf_Unsigned pe_section_count;
    struct dwarf_pe_generic_image_section_header *pe_sectionptr;

    Dwarf_Unsigned pe_string_table_size;
    char          *pe_string_table;
} dwarf_pe_object_access_internals_t;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PE_GENERIC_H */
