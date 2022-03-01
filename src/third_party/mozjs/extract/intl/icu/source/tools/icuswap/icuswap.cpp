// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  icuswap.cpp
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003aug08
*   created by: Markus W. Scherer
*
*   This tool takes an ICU data file and "swaps" it, that is, changes its
*   platform properties between big-/little-endianness and ASCII/EBCDIC charset
*   families.
*   The modified data file is written to a new file.
*   Useful as an install-time tool for shipping only one flavor of ICU data
*   and preparing data files for the target platform.
*   Will not work with data DLLs (shared libraries).
*/

#include "unicode/utypes.h"
#include "unicode/putil.h"
#include "unicode/udata.h"
#include "cmemory.h"
#include "cstring.h"
#include "uinvchar.h"
#include "uarrsort.h"
#include "ucmndata.h"
#include "udataswp.h"
#include "swapimpl.h"
#include "toolutil.h"
#include "uoptions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* definitions */

#define DEFAULT_PADDING_LENGTH 15

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_DEF("type", 't', UOPT_REQUIRES_ARG)
};

enum {
    OPT_HELP_H,
    OPT_HELP_QUESTION_MARK,
    OPT_OUT_TYPE
};

static int32_t
fileSize(FILE *f) {
    int32_t size;

    fseek(f, 0, SEEK_END);
    size=(int32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    return size;
}

/**
 * Swap an ICU .dat package, including swapping of enclosed items.
 */
U_CFUNC int32_t U_CALLCONV
udata_swapPackage(const char *inFilename, const char *outFilename,
                  const UDataSwapper *ds,
                  const void *inData, int32_t length, void *outData,
                  UErrorCode *pErrorCode);

U_CDECL_BEGIN
static void U_CALLCONV
printError(void *context, const char *fmt, va_list args) {
    vfprintf((FILE *)context, fmt, args);
}
U_CDECL_END

static int
printUsage(const char *pname, UBool ishelp) {
    fprintf(stderr,
            "%csage: %s [ -h, -?, --help ] -tl|-tb|-te|--type=b|... infilename outfilename\n",
            ishelp ? 'U' : 'u', pname);
    if(ishelp) {
        fprintf(stderr,
              "\nOptions: -h, -?, --help    print this message and exit\n"
                "         Read the input file, swap its platform properties according\n"
                "         to the -t or --type option, and write the result to the output file.\n"
                "         -tl               change to little-endian/ASCII charset family\n"
                "         -tb               change to big-endian/ASCII charset family\n"
                "         -te               change to big-endian/EBCDIC charset family\n");
    }

    return !ishelp;
}

extern int
main(int argc, char *argv[]) {
    FILE *in, *out;
    const char *pname;
    char *data;
    int32_t length;
    UBool ishelp;
    int rc;

    UDataSwapper *ds;
    const UDataInfo *pInfo;
    UErrorCode errorCode;
    uint8_t outCharset;
    UBool outIsBigEndian;

    U_MAIN_INIT_ARGS(argc, argv);

    fprintf(stderr, "Warning: icuswap is an obsolete tool and it will be removed in the next ICU release.\nPlease use the icupkg tool instead.\n");

    /* get the program basename */
    pname=strrchr(argv[0], U_FILE_SEP_CHAR);
    if(pname==NULL) {
        pname=strrchr(argv[0], '/');
    }
    if(pname!=NULL) {
        ++pname;
    } else {
        pname=argv[0];
    }

    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);
    ishelp=options[OPT_HELP_H].doesOccur || options[OPT_HELP_QUESTION_MARK].doesOccur;
    if(ishelp || argc!=3) {
        return printUsage(pname, ishelp);
    }

    /* parse the output type option */
    data=(char *)options[OPT_OUT_TYPE].value;
    if(data[0]==0 || data[1]!=0) {
        /* the type must be exactly one letter */
        return printUsage(pname, FALSE);
    }
    switch(data[0]) {
    case 'l':
        outIsBigEndian=FALSE;
        outCharset=U_ASCII_FAMILY;
        break;
    case 'b':
        outIsBigEndian=TRUE;
        outCharset=U_ASCII_FAMILY;
        break;
    case 'e':
        outIsBigEndian=TRUE;
        outCharset=U_EBCDIC_FAMILY;
        break;
    default:
        return printUsage(pname, FALSE);
    }

    in=out=NULL;
    data=NULL;

    /* open the input file, get its length, allocate memory for it, read the file */
    in=fopen(argv[1], "rb");
    if(in==NULL) {
        fprintf(stderr, "%s: unable to open input file \"%s\"\n", pname, argv[1]);
        rc=2;
        goto done;
    }

    length=fileSize(in);
    if(length<DEFAULT_PADDING_LENGTH) {
        fprintf(stderr, "%s: empty input file \"%s\"\n", pname, argv[1]);
        rc=2;
        goto done;
    }

    /*
     * +15: udata_swapPackage() may need to add a few padding bytes to the
     * last item if charset swapping is done,
     * because the last item may be resorted into the middle and then needs
     * additional padding bytes
     */
    data=(char *)malloc(length+DEFAULT_PADDING_LENGTH);
    if(data==NULL) {
        fprintf(stderr, "%s: error allocating memory for \"%s\"\n", pname, argv[1]);
        rc=2;
        goto done;
    }

    /* set the last 15 bytes to the usual padding byte, see udata_swapPackage() */
    uprv_memset(data+length-DEFAULT_PADDING_LENGTH, 0xaa, DEFAULT_PADDING_LENGTH);

    if(length!=(int32_t)fread(data, 1, length, in)) {
        fprintf(stderr, "%s: error reading \"%s\"\n", pname, argv[1]);
        rc=3;
        goto done;
    }

    fclose(in);
    in=NULL;

    /* swap the data in-place */
    errorCode=U_ZERO_ERROR;
    ds=udata_openSwapperForInputData(data, length, outIsBigEndian, outCharset, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "%s: udata_openSwapperForInputData(\"%s\") failed - %s\n",
                pname, argv[1], u_errorName(errorCode));
        rc=4;
        goto done;
    }

    ds->printError=printError;
    ds->printErrorContext=stderr;

    /* speculative cast, protected by the following length check */
    pInfo=(const UDataInfo *)((const char *)data+4);

    if( length>=20 &&
        pInfo->dataFormat[0]==0x43 &&   /* dataFormat="CmnD" */
        pInfo->dataFormat[1]==0x6d &&
        pInfo->dataFormat[2]==0x6e &&
        pInfo->dataFormat[3]==0x44
    ) {
        /*
         * swap the .dat package
         * udata_swapPackage() needs to rename ToC name entries from the old package
         * name to the new one.
         * We pass it the filenames, and udata_swapPackage() will extract the
         * package names.
         */
        length=udata_swapPackage(argv[1], argv[2], ds, data, length, data, &errorCode);
        udata_closeSwapper(ds);
        if(U_FAILURE(errorCode)) {
            fprintf(stderr, "%s: udata_swapPackage(\"%s\") failed - %s\n",
                    pname, argv[1], u_errorName(errorCode));
            rc=4;
            goto done;
        }
    } else {
        /* swap the data, which is not a .dat package */
        length=udata_swap(ds, data, length, data, &errorCode);
        udata_closeSwapper(ds);
        if(U_FAILURE(errorCode)) {
            fprintf(stderr, "%s: udata_swap(\"%s\") failed - %s\n",
                    pname, argv[1], u_errorName(errorCode));
            rc=4;
            goto done;
        }
    }

    out=fopen(argv[2], "wb");
    if(out==NULL) {
        fprintf(stderr, "%s: unable to open output file \"%s\"\n", pname, argv[2]);
        rc=5;
        goto done;
    }

    if(length!=(int32_t)fwrite(data, 1, length, out)) {
        fprintf(stderr, "%s: error writing \"%s\"\n", pname, argv[2]);
        rc=6;
        goto done;
    }

    fclose(out);
    out=NULL;

    /* all done */
    rc=0;

done:
    if(in!=NULL) {
        fclose(in);
    }
    if(out!=NULL) {
        fclose(out);
    }
    if(data!=NULL) {
        free(data);
    }
    return rc;
}

/* swap .dat package files -------------------------------------------------- */

static int32_t
extractPackageName(const UDataSwapper *ds, const char *filename,
                   char pkg[], int32_t capacity,
                   UErrorCode *pErrorCode) {
    const char *basename;
    int32_t len;

    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }

    basename=findBasename(filename);
    len=(int32_t)uprv_strlen(basename)-4; /* -4: subtract the length of ".dat" */

    if(len<=0 || 0!=uprv_strcmp(basename+len, ".dat")) {
        udata_printError(ds, "udata_swapPackage(): \"%s\" is not recognized as a package filename (must end with .dat)\n",
                         basename);
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    if(len>=capacity) {
        udata_printError(ds, "udata_swapPackage(): the package name \"%s\" is too long (>=%ld)\n",
                         (long)capacity);
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    uprv_memcpy(pkg, basename, len);
    pkg[len]=0;
    return len;
}

struct ToCEntry {
    uint32_t nameOffset, inOffset, outOffset, length;
};

U_CDECL_BEGIN
static int32_t U_CALLCONV
compareToCEntries(const void *context, const void *left, const void *right) {
    const char *chars=(const char *)context;
    return (int32_t)uprv_strcmp(chars+((const ToCEntry *)left)->nameOffset,
                                chars+((const ToCEntry *)right)->nameOffset);
}
U_CDECL_END

U_CFUNC int32_t U_CALLCONV
udata_swapPackage(const char *inFilename, const char *outFilename,
                  const UDataSwapper *ds,
                  const void *inData, int32_t length, void *outData,
                  UErrorCode *pErrorCode) {
    const UDataInfo *pInfo;
    int32_t headerSize;

    const uint8_t *inBytes;
    uint8_t *outBytes;

    uint32_t itemCount, offset, i;
    int32_t itemLength;

    const UDataOffsetTOCEntry *inEntries;
    UDataOffsetTOCEntry *outEntries;

    ToCEntry *table;

    char inPkgName[32], outPkgName[32];
    int32_t inPkgNameLength, outPkgNameLength;

    /* udata_swapDataHeader checks the arguments */
    headerSize=udata_swapDataHeader(ds, inData, length, outData, pErrorCode);
    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return 0;
    }

    /* check data format and format version */
    pInfo=(const UDataInfo *)((const char *)inData+4);
    if(!(
        pInfo->dataFormat[0]==0x43 &&   /* dataFormat="CmnD" */
        pInfo->dataFormat[1]==0x6d &&
        pInfo->dataFormat[2]==0x6e &&
        pInfo->dataFormat[3]==0x44 &&
        pInfo->formatVersion[0]==1
    )) {
        udata_printError(ds, "udata_swapPackage(): data format %02x.%02x.%02x.%02x (format version %02x) is not recognized as an ICU .dat package\n",
                         pInfo->dataFormat[0], pInfo->dataFormat[1],
                         pInfo->dataFormat[2], pInfo->dataFormat[3],
                         pInfo->formatVersion[0]);
        *pErrorCode=U_UNSUPPORTED_ERROR;
        return 0;
    }

    /*
     * We need to change the ToC name entries so that they have the correct
     * package name prefix.
     * Extract the package names from the in/out filenames.
     */
    inPkgNameLength=extractPackageName(
                        ds, inFilename,
                        inPkgName, (int32_t)sizeof(inPkgName),
                        pErrorCode);
    outPkgNameLength=extractPackageName(
                        ds, outFilename,
                        outPkgName, (int32_t)sizeof(outPkgName),
                        pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }

    /*
     * It is possible to work with inPkgNameLength!=outPkgNameLength,
     * but then the length of the data file would change more significantly,
     * which we are not currently prepared for.
     */
    if(inPkgNameLength!=outPkgNameLength) {
        udata_printError(ds, "udata_swapPackage(): the package names \"%s\" and \"%s\" must have the same length\n",
                         inPkgName, outPkgName);
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    inBytes=(const uint8_t *)inData+headerSize;
    inEntries=(const UDataOffsetTOCEntry *)(inBytes+4);

    if(length<0) {
        /* preflighting */
        itemCount=ds->readUInt32(*(const uint32_t *)inBytes);
        if(itemCount==0) {
            /* no items: count only the item count and return */
            return headerSize+4;
        }

        /* read the last item's offset and preflight it */
        offset=ds->readUInt32(inEntries[itemCount-1].dataOffset);
        itemLength=udata_swap(ds, inBytes+offset, -1, NULL, pErrorCode);

        if(U_SUCCESS(*pErrorCode)) {
            return headerSize+offset+(uint32_t)itemLength;
        } else {
            return 0;
        }
    } else {
        /* check that the itemCount fits, then the ToC table, then at least the header of the last item */
        length-=headerSize;
        if(length<4) {
            /* itemCount does not fit */
            offset=0xffffffff;
            itemCount=0; /* make compilers happy */
        } else {
            itemCount=ds->readUInt32(*(const uint32_t *)inBytes);
            if(itemCount==0) {
                offset=4;
            } else if((uint32_t)length<(4+8*itemCount)) {
                /* ToC table does not fit */
                offset=0xffffffff;
            } else {
                /* offset of the last item plus at least 20 bytes for its header */
                offset=20+ds->readUInt32(inEntries[itemCount-1].dataOffset);
            }
        }
        if((uint32_t)length<offset) {
            udata_printError(ds, "udata_swapPackage(): too few bytes (%d after header) for a .dat package\n",
                             length);
            *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
            return 0;
        }

        outBytes=(uint8_t *)outData+headerSize;

        /* swap the item count */
        ds->swapArray32(ds, inBytes, 4, outBytes, pErrorCode);

        if(itemCount==0) {
            /* no items: just return now */
            return headerSize+4;
        }

        /* swap the item name strings */
        offset=4+8*itemCount;
        itemLength=(int32_t)(ds->readUInt32(inEntries[0].dataOffset)-offset);
        udata_swapInvStringBlock(ds, inBytes+offset, itemLength, outBytes+offset, pErrorCode);
        if(U_FAILURE(*pErrorCode)) {
            udata_printError(ds, "udata_swapPackage() failed to swap the data item name strings\n");
            return 0;
        }
        /* keep offset and itemLength in case we allocate and copy the strings below */

        /* swap the package names into the output charset */
        if(ds->outCharset!=U_CHARSET_FAMILY) {
            UDataSwapper *ds2;
            ds2=udata_openSwapper(TRUE, U_CHARSET_FAMILY, TRUE, ds->outCharset, pErrorCode);
            ds2->swapInvChars(ds2, inPkgName, inPkgNameLength, inPkgName, pErrorCode);
            ds2->swapInvChars(ds2, outPkgName, outPkgNameLength, outPkgName, pErrorCode);
            udata_closeSwapper(ds2);
            if(U_FAILURE(*pErrorCode)) {
                udata_printError(ds, "udata_swapPackage() failed to swap the input/output package names\n");
            }
        }

        /* change the prefix of each ToC entry name from the old to the new package name */
        {
            char *entryName;

            for(i=0; i<itemCount; ++i) {
                entryName=(char *)inBytes+ds->readUInt32(inEntries[i].nameOffset);

                if(0==uprv_memcmp(entryName, inPkgName, inPkgNameLength)) {
                    uprv_memcpy(entryName, outPkgName, inPkgNameLength);
                } else {
                    udata_printError(ds, "udata_swapPackage() failed: ToC item %ld does not have the input package name as a prefix\n",
                                     (long)i);
                    *pErrorCode=U_INVALID_FORMAT_ERROR;
                    return 0;
                }
            }
        }

        /*
         * Allocate the ToC table and, if necessary, a temporary buffer for
         * pseudo-in-place swapping.
         *
         * We cannot swap in-place because:
         *
         * 1. If the swapping of an item fails mid-way, then in-place swapping
         * has destroyed its data.
         * Out-of-place swapping allows us to then copy its original data.
         *
         * 2. If swapping changes the charset family, then we must resort
         * not only the ToC table but also the data items themselves.
         * This requires a permutation and is best done with separate in/out
         * buffers.
         *
         * We swapped the strings above to avoid the malloc below if string swapping fails.
         */
        if(inData==outData) {
            /* +15: prepare for extra padding of a newly-last item */
            table=(ToCEntry *)uprv_malloc(itemCount*sizeof(ToCEntry)+length+DEFAULT_PADDING_LENGTH);
            if(table!=NULL) {
                outBytes=(uint8_t *)(table+itemCount);

                /* copy the item count and the swapped strings */
                uprv_memcpy(outBytes, inBytes, 4);
                uprv_memcpy(outBytes+offset, inBytes+offset, itemLength);
            }
        } else {
            table=(ToCEntry *)uprv_malloc(itemCount*sizeof(ToCEntry));
        }
        if(table==NULL) {
            udata_printError(ds, "udata_swapPackage(): out of memory allocating %d bytes\n",
                             inData==outData ?
                                 itemCount*sizeof(ToCEntry)+length+DEFAULT_PADDING_LENGTH :
                                 itemCount*sizeof(ToCEntry));
            *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
            return 0;
        }
        outEntries=(UDataOffsetTOCEntry *)(outBytes+4);

        /* read the ToC table */
        for(i=0; i<itemCount; ++i) {
            table[i].nameOffset=ds->readUInt32(inEntries[i].nameOffset);
            table[i].inOffset=ds->readUInt32(inEntries[i].dataOffset);
            if(i>0) {
                table[i-1].length=table[i].inOffset-table[i-1].inOffset;
            }
        }
        table[itemCount-1].length=(uint32_t)length-table[itemCount-1].inOffset;

        if(ds->inCharset==ds->outCharset) {
            /* no charset swapping, no resorting: keep item offsets the same */
            for(i=0; i<itemCount; ++i) {
                table[i].outOffset=table[i].inOffset;
            }
        } else {
            /* charset swapping: resort items by their swapped names */

            /*
             * Before the actual sorting, we need to make sure that each item
             * has a length that is a multiple of 16 bytes so that all items
             * are 16-aligned.
             * Only the old last item may be missing up to 15 padding bytes.
             * Add padding bytes for it.
             * Since the icuswap main() function has already allocated enough
             * input buffer space and set the last 15 bytes there to 0xaa,
             * we only need to increase the total data length and the length
             * of the last item here.
             */
            if((length&0xf)!=0) {
                int32_t delta=16-(length&0xf);
                length+=delta;
                table[itemCount-1].length+=(uint32_t)delta;
            }

            /* Save the offset before we sort the TOC. */
            offset=table[0].inOffset;
            /* sort the TOC entries */
            uprv_sortArray(table, (int32_t)itemCount, (int32_t)sizeof(ToCEntry),
                           compareToCEntries, outBytes, FALSE, pErrorCode);

            /*
             * Note: Before sorting, the inOffset values were in order.
             * Now the outOffset values are in order.
             */

            /* assign outOffset values */
            for(i=0; i<itemCount; ++i) {
                table[i].outOffset=offset;
                offset+=table[i].length;
            }
        }

        /* write the output ToC table */
        for(i=0; i<itemCount; ++i) {
            ds->writeUInt32(&outEntries[i].nameOffset, table[i].nameOffset);
            ds->writeUInt32(&outEntries[i].dataOffset, table[i].outOffset);
        }

        /* swap each data item */
        for(i=0; i<itemCount; ++i) {
            /* first copy the item bytes to make sure that unreachable bytes are copied */ 
            uprv_memcpy(outBytes+table[i].outOffset, inBytes+table[i].inOffset, table[i].length);

            /* swap the item */
            udata_swap(ds, inBytes+table[i].inOffset, (int32_t)table[i].length,
                          outBytes+table[i].outOffset, pErrorCode);

            if(U_FAILURE(*pErrorCode)) {
                if(ds->outCharset==U_CHARSET_FAMILY) {
                    udata_printError(ds, "warning: udata_swapPackage() failed to swap item \"%s\"\n"
                                         "    at inOffset 0x%x length 0x%x - %s\n"
                                         "    the data item will be copied, not swapped\n\n",
                                     (char *)outBytes+table[i].nameOffset,
                                     table[i].inOffset, table[i].length, u_errorName(*pErrorCode));
                } else {
                    udata_printError(ds, "warning: udata_swapPackage() failed to swap an item\n"
                                         "    at inOffset 0x%x length 0x%x - %s\n"
                                         "    the data item will be copied, not swapped\n\n",
                                     table[i].inOffset, table[i].length, u_errorName(*pErrorCode));
                }
                /* reset the error code, copy the data item, and continue */
                *pErrorCode=U_ZERO_ERROR;
                uprv_memcpy(outBytes+table[i].outOffset, inBytes+table[i].inOffset, table[i].length);
            }
        }

        if(inData==outData) {
            /* copy the data from the temporary buffer to the in-place buffer */
            uprv_memcpy((uint8_t *)outData+headerSize, outBytes, length);
        }
        uprv_free(table);

        return headerSize+length;
    }
}

/*
 * Hey, Emacs, please set the following:
 *
 * Local Variables:
 * indent-tabs-mode: nil
 * End:
 *
 */
