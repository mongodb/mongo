#ifndef DWARF_PE_DESCR_H
#define DWARF_PE_DESCR_H
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

#define IMAGE_DOS_SIGNATURE_dw    0x5a4d /* le on disk 'M' 'Z' */
#define IMAGE_DOS_REVSIGNATURE_dw 0x4d5a /* be on disk */
#define IMAGE_NT_SIGNATURE_dw     0x00004550

#ifndef TYP
#define TYP(n,l) char (n)[(l)]
#endif /* TYPE */

/*  Data types
    see https://msdn.microsoft.com/en-us/library/\
    windows/desktop/aa383751(v=vs.85).aspx */

/*#define FIELD_OFFSET(type,field) \
    ((LONG)(LONG_PTR)&(((type *)0)->field))*/

#define IMAGE_SIZEOF_SYMBOL 18

struct dos_header_dw {
    TYP(dh_mz,2);
    TYP(dh_dos_data,58);
    TYP(dh_image_offset,4);
};

/*  IMAGE_FILE_HEADER_dw
    see https://msdn.microsoft.com/fr-fr/library/\
    windows/desktop/ms680313(v=vs.85).aspx */

typedef struct
{
    TYP(Machine,2);
    TYP(NumberOfSections,2);
    TYP(TimeDateStamp,4);
    TYP(PointerToSymbolTable,4);
    TYP(NumberOfSymbols,4);
    TYP(SizeOfOptionalHeader,2);
    TYP(Characteristics,2);
} IMAGE_FILE_HEADER_dw;

/*  IMAGE_DATA_DIRECTORY_dw
    see https://msdn.microsoft.com/fr-fr/library/\
    windows/desktop/ms680305(v=vs.85).aspx */

typedef struct
{
    TYP(VirtualAddress,4);
    TYP(Size,4);
} IMAGE_DATA_DIRECTORY_dw;

/*  IMAGE_OPTIONAL_HEADER
    see https://msdn.microsoft.com/en-us/library/\
    windows/desktop/ms680339(v=vs.85).aspx */

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

typedef struct
{
    TYP(Magic,2);
    unsigned char MajorLinkerVersion;
    unsigned char MinorLinkerVersion;
    TYP(SizeOfCode,4);
    TYP(SizeOfInitializedData,4);
    TYP(SizeOfUninitializedData,4);
    TYP(AddressOfEntryPoint,4);
    TYP(BaseOfCode,4);
    TYP(BaseOfData,4);
    TYP(ImageBase,4);
    TYP(SectionAlignment,4);
    TYP(FileAlignment,4);
    TYP(MajorOperatingSystemVersion,2);
    TYP(MinorOperatingSystemVersion,2);
    TYP(MajorImageVersion,2);
    TYP(MinorImageVersion,2);
    TYP(MajorSubsystemVersion,2);
    TYP(MinorSubsystemVersion,2);
    TYP(Win32VersionValue,4);
    TYP(SizeOfImage,4);
    TYP(SizeOfHeaders,4);
    TYP(CheckSum,4);
    TYP(Subsystem,2);
    TYP(DllCharacteristics,2);
    TYP(SizeOfStackReserve,4);
    TYP(SizeOfStackCommit,4);
    TYP(SizeOfHeapReserve,4);
    TYP(SizeOfHeapCommit,4);
    TYP(LoaderFlags,4);
    TYP(NumberOfRvaAndSizes,4);
    IMAGE_DATA_DIRECTORY_dw
        DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32_dw;

typedef struct
{
    TYP(Magic,2);
    unsigned char MajorLinkerVersion;
    unsigned char MinorLinkerVersion;
    TYP(SizeOfCode,4);
    TYP(SizeOfInitializedData,4);
    TYP(SizeOfUninitializedData,4);
    TYP(AddressOfEntryPoint,4);
    TYP(BaseOfCode,4);
    TYP(ImageBase,8);
    TYP(SectionAlignment,4);
    TYP(FileAlignment,4);
    TYP(MajorOperatingSystemVersion,2);
    TYP(MinorOperatingSystemVersion,2);
    TYP(MajorImageVersion,2);
    TYP(MinorImageVersion,2);
    TYP(MajorSubsystemVersion,2);
    TYP(MinorSubsystemVersion,2);
    TYP(Win32VersionValue,4);
    TYP(SizeOfImage,4);
    TYP(SizeOfHeaders,4);
    TYP(CheckSum,4);
    TYP(Subsystem,2);
    TYP(DllCharacteristics,2);
    TYP(SizeOfStackReserve,8);
    TYP(SizeOfStackCommit,8);
    TYP(SizeOfHeapReserve,8);
    TYP(SizeOfHeapCommit,8);
    TYP(LoaderFlags,4);
    TYP(NumberOfRvaAndSizes,4);
    IMAGE_DATA_DIRECTORY_dw
        DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64_dw;

/*  IMAGE_NT_HEADERS
    see https://msdn.microsoft.com/fr-fr/library/\
    windows/desktop/ms680336(v=vs.85).aspx */

#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b
#define IMAGE_ROM_OPTIONAL_HDR_MAGIC 0x107

typedef struct
{
    TYP(Signature,4);
    IMAGE_FILE_HEADER_dw FileHeader;
    IMAGE_OPTIONAL_HEADER64_dw OptionalHeader;
} IMAGE_NT_HEADERS64_dw, *PIMAGE_NT_HEADERS64_dw;

typedef struct
{
    TYP(Signature,4);
    IMAGE_FILE_HEADER_dw FileHeader;
    IMAGE_OPTIONAL_HEADER32_dw OptionalHeader;
} IMAGE_NT_HEADERS32_dw, *PIMAGE_NT_HEADERS32_dw;

/*  IMAGE_SECTION_HEADER_dw
    see:
    https://msdn.microsoft.com/en-us/library/windows/\
    desktop/ms680341(v=vs.85).aspx
    and, for details on VirtualSize and SizeOfRawData:
    https://docs.microsoft.com/en-us/windows/desktop/\
    api/winnt/ns-winnt-_image_section_header */

#define IMAGE_SIZEOF_SHORT_NAME 8

typedef struct
{
    char Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        TYP(PhysicalAddress,4);
        TYP(VirtualSize,4);
    } Misc;
    TYP(VirtualAddress,4);
    TYP(SizeOfRawData,4);
    TYP(PointerToRawData,4);
    TYP(PointerToRelocations,4);
    TYP(PointerToLinenumbers,4);
    TYP(NumberOfRelocations,2);
    TYP(NumberOfLinenumbers,2);
    TYP(Characteristics,4);
} IMAGE_SECTION_HEADER_dw, *PIMAGE_SECTION_HEADER_dw;

#define IMAGE_SCN_SCALE_INDEX            0x00000001
#define IMAGE_SCN_TYPE_NO_PAD            0x00000008
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080

#define IMAGE_SCN_LNK_OTHER              0x00000100
#define IMAGE_SCN_LNK_INFO               0x00000200
#define IMAGE_SCN_LNK_REMOVE             0x00000800
#define IMAGE_SCN_LNK_COMDAT             0x00001000
#define IMAGE_SCN_NO_DEFER_SPEC_EXC      0x00004000
#define IMAGE_SCN_MEM_FARDATA            0x00008000
#define IMAGE_SCN_MEM_PURGEABLE          0x00020000
#define IMAGE_SCN_MEM_LOCKED             0x00040000
#define IMAGE_SCN_MEM_PRELOAD            0x00080000

#define IMAGE_SCN_ALIGN_1BYTES           0x00100000
#define IMAGE_SCN_ALIGN_2BYTES           0x00200000
#define IMAGE_SCN_ALIGN_4BYTES           0x00300000
#define IMAGE_SCN_ALIGN_8BYTES           0x00400000
#define IMAGE_SCN_ALIGN_16BYTES          0x00500000
#define IMAGE_SCN_ALIGN_32BYTES          0x00600000
#define IMAGE_SCN_ALIGN_64BYTES          0x00700000
#define IMAGE_SCN_ALIGN_128BYTES         0x00800000
#define IMAGE_SCN_ALIGN_256BYTES         0x00900000
#define IMAGE_SCN_ALIGN_512BYTES         0x00A00000
#define IMAGE_SCN_ALIGN_1024BYTES        0x00B00000
#define IMAGE_SCN_ALIGN_2048BYTES        0x00C00000
#define IMAGE_SCN_ALIGN_4096BYTES        0x00D00000
#define IMAGE_SCN_ALIGN_8192BYTES        0x00E00000

#define IMAGE_SCN_ALIGN_MASK             0x00F00000
#define IMAGE_SCN_LNK_NRELOC_OVFL        0x01000000
#define IMAGE_SCN_MEM_DISCARDABLE        0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED         0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED          0x08000000
#define IMAGE_SCN_MEM_SHARED             0x10000000
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* DWARF_PE_DESCR_H */
