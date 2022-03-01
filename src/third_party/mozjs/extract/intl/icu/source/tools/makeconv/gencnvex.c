// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  gencnvex.c
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003oct12
*   created by: Markus W. Scherer
*/

#include <stdio.h>
#include "unicode/utypes.h"
#include "unicode/ustring.h"
#include "cstring.h"
#include "cmemory.h"
#include "ucnv_cnv.h"
#include "ucnvmbcs.h"
#include "toolutil.h"
#include "unewdata.h"
#include "ucm.h"
#include "makeconv.h"
#include "genmbcs.h"

static void
CnvExtClose(NewConverter *cnvData);

static UBool
CnvExtIsValid(NewConverter *cnvData,
              const uint8_t *bytes, int32_t length);

static UBool
CnvExtAddTable(NewConverter *cnvData, UCMTable *table, UConverterStaticData *staticData);

static uint32_t
CnvExtWrite(NewConverter *cnvData, const UConverterStaticData *staticData,
            UNewDataMemory *pData, int32_t tableType);

typedef struct CnvExtData {
    NewConverter newConverter;

    UCMFile *ucm;

    /* toUnicode (state table in ucm->states) */
    UToolMemory *toUTable, *toUUChars;

    /* fromUnicode */
    UToolMemory *fromUTableUChars, *fromUTableValues, *fromUBytes;

    uint16_t stage1[MBCS_STAGE_1_SIZE];
    uint16_t stage2[MBCS_STAGE_2_SIZE];
    uint16_t stage3[0x10000<<UCNV_EXT_STAGE_2_LEFT_SHIFT]; /* 0x10000 because of 16-bit stage 2/3 indexes */
    uint32_t stage3b[0x10000];

    int32_t stage1Top, stage2Top, stage3Top, stage3bTop;

    /* for stage3 compaction of <subchar1> |2 mappings */
    uint16_t stage3Sub1Block;

    /* statistics */
    int32_t
        maxInBytes, maxOutBytes, maxBytesPerUChar,
        maxInUChars, maxOutUChars, maxUCharsPerByte;
} CnvExtData;

NewConverter *
CnvExtOpen(UCMFile *ucm) {
    CnvExtData *extData;
    
    extData=(CnvExtData *)uprv_malloc(sizeof(CnvExtData));
    if(extData==NULL) {
        printf("out of memory\n");
        exit(U_MEMORY_ALLOCATION_ERROR);
    }
    uprv_memset(extData, 0, sizeof(CnvExtData));

    extData->ucm=ucm; /* aliased, not owned */

    extData->newConverter.close=CnvExtClose;
    extData->newConverter.isValid=CnvExtIsValid;
    extData->newConverter.addTable=CnvExtAddTable;
    extData->newConverter.write=CnvExtWrite;
    return &extData->newConverter;
}

static void
CnvExtClose(NewConverter *cnvData) {
    CnvExtData *extData=(CnvExtData *)cnvData;
    if(extData!=NULL) {
        utm_close(extData->toUTable);
        utm_close(extData->toUUChars);
        utm_close(extData->fromUTableUChars);
        utm_close(extData->fromUTableValues);
        utm_close(extData->fromUBytes);
        uprv_free(extData);
    }
}

/* we do not expect this to be called */
static UBool
CnvExtIsValid(NewConverter *cnvData,
        const uint8_t *bytes, int32_t length) {
    // suppress compiler warnings about unused variables
    (void)cnvData;
    (void)bytes;
    (void)length;
    return FALSE;
}

static uint32_t
CnvExtWrite(NewConverter *cnvData, const UConverterStaticData *staticData,
            UNewDataMemory *pData, int32_t tableType) {
    (void) staticData; // suppress compiler warnings about unused variable
    CnvExtData *extData=(CnvExtData *)cnvData;
    int32_t length, top, headerSize;

    int32_t indexes[UCNV_EXT_INDEXES_MIN_LENGTH]={ 0 };

    if(tableType&TABLE_BASE) {
        headerSize=0;
    } else {
        _MBCSHeader header={ { 0, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        /* write the header and base table name for an extension-only table */
        length=(int32_t)uprv_strlen(extData->ucm->baseName)+1;
        while(length&3) {
            /* add padding */
            extData->ucm->baseName[length++]=0;
        }

        headerSize=MBCS_HEADER_V4_LENGTH*4+length;

        /* fill the header */
        header.version[0]=4;
        header.version[1]=2;
        header.flags=(uint32_t)((headerSize<<8)|MBCS_OUTPUT_EXT_ONLY);

        /* write the header and the base table name */
        udata_writeBlock(pData, &header, MBCS_HEADER_V4_LENGTH*4);
        udata_writeBlock(pData, extData->ucm->baseName, length);
    }

    /* fill indexes[] - offsets/indexes are in units of the target array */
    top=0;

    indexes[UCNV_EXT_INDEXES_LENGTH]=length=UCNV_EXT_INDEXES_MIN_LENGTH;
    top+=length*4;

    indexes[UCNV_EXT_TO_U_INDEX]=top;
    indexes[UCNV_EXT_TO_U_LENGTH]=length=utm_countItems(extData->toUTable);
    top+=length*4;

    indexes[UCNV_EXT_TO_U_UCHARS_INDEX]=top;
    indexes[UCNV_EXT_TO_U_UCHARS_LENGTH]=length=utm_countItems(extData->toUUChars);
    top+=length*2;

    indexes[UCNV_EXT_FROM_U_UCHARS_INDEX]=top;
    length=utm_countItems(extData->fromUTableUChars);
    top+=length*2;

    if(top&3) {
        /* add padding */
        *((UChar *)utm_alloc(extData->fromUTableUChars))=0;
        *((uint32_t *)utm_alloc(extData->fromUTableValues))=0;
        ++length;
        top+=2;
    }
    indexes[UCNV_EXT_FROM_U_LENGTH]=length;

    indexes[UCNV_EXT_FROM_U_VALUES_INDEX]=top;
    top+=length*4;

    indexes[UCNV_EXT_FROM_U_BYTES_INDEX]=top;
    length=utm_countItems(extData->fromUBytes);
    top+=length;

    if(top&1) {
        /* add padding */
        *((uint8_t *)utm_alloc(extData->fromUBytes))=0;
        ++length;
        ++top;
    }
    indexes[UCNV_EXT_FROM_U_BYTES_LENGTH]=length;

    indexes[UCNV_EXT_FROM_U_STAGE_12_INDEX]=top;
    indexes[UCNV_EXT_FROM_U_STAGE_1_LENGTH]=length=extData->stage1Top;
    indexes[UCNV_EXT_FROM_U_STAGE_12_LENGTH]=length+=extData->stage2Top;
    top+=length*2;

    indexes[UCNV_EXT_FROM_U_STAGE_3_INDEX]=top;
    length=extData->stage3Top;
    top+=length*2;

    if(top&3) {
        /* add padding */
        extData->stage3[extData->stage3Top++]=0;
        ++length;
        top+=2;
    }
    indexes[UCNV_EXT_FROM_U_STAGE_3_LENGTH]=length;

    indexes[UCNV_EXT_FROM_U_STAGE_3B_INDEX]=top;
    indexes[UCNV_EXT_FROM_U_STAGE_3B_LENGTH]=length=extData->stage3bTop;
    top+=length*4;

    indexes[UCNV_EXT_SIZE]=top;

    /* statistics */
    indexes[UCNV_EXT_COUNT_BYTES]=
        (extData->maxInBytes<<16)|
        (extData->maxOutBytes<<8)|
        extData->maxBytesPerUChar;
    indexes[UCNV_EXT_COUNT_UCHARS]=
        (extData->maxInUChars<<16)|
        (extData->maxOutUChars<<8)|
        extData->maxUCharsPerByte;

    indexes[UCNV_EXT_FLAGS]=extData->ucm->ext->unicodeMask;

    /* write the extension data */
    udata_writeBlock(pData, indexes, sizeof(indexes));
    udata_writeBlock(pData, utm_getStart(extData->toUTable), indexes[UCNV_EXT_TO_U_LENGTH]*4);
    udata_writeBlock(pData, utm_getStart(extData->toUUChars), indexes[UCNV_EXT_TO_U_UCHARS_LENGTH]*2);

    udata_writeBlock(pData, utm_getStart(extData->fromUTableUChars), indexes[UCNV_EXT_FROM_U_LENGTH]*2);
    udata_writeBlock(pData, utm_getStart(extData->fromUTableValues), indexes[UCNV_EXT_FROM_U_LENGTH]*4);
    udata_writeBlock(pData, utm_getStart(extData->fromUBytes), indexes[UCNV_EXT_FROM_U_BYTES_LENGTH]);

    udata_writeBlock(pData, extData->stage1, extData->stage1Top*2);
    udata_writeBlock(pData, extData->stage2, extData->stage2Top*2);
    udata_writeBlock(pData, extData->stage3, extData->stage3Top*2);
    udata_writeBlock(pData, extData->stage3b, extData->stage3bTop*4);

#if 0
    {
        int32_t i, j;

        length=extData->stage1Top;
        printf("\nstage1[%x]:\n", length);

        for(i=0; i<length; ++i) {
            if(extData->stage1[i]!=length) {
                printf("stage1[%04x]=%04x\n", i, extData->stage1[i]);
            }
        }

        j=length;
        length=extData->stage2Top;
        printf("\nstage2[%x]:\n", length);

        for(i=0; i<length; ++j, ++i) {
            if(extData->stage2[i]!=0) {
                printf("stage12[%04x]=%04x\n", j, extData->stage2[i]);
            }
        }

        length=extData->stage3Top;
        printf("\nstage3[%x]:\n", length);

        for(i=0; i<length; ++i) {
            if(extData->stage3[i]!=0) {
                printf("stage3[%04x]=%04x\n", i, extData->stage3[i]);
            }
        }

        length=extData->stage3bTop;
        printf("\nstage3b[%x]:\n", length);

        for(i=0; i<length; ++i) {
            if(extData->stage3b[i]!=0) {
                printf("stage3b[%04x]=%08x\n", i, extData->stage3b[i]);
            }
        }
    }
#endif

    if(VERBOSE) {
        printf("size of extension data: %ld\n", (long)top);
    }

    /* return the number of bytes that should have been written */
    return (uint32_t)(headerSize+top);
}

/* to Unicode --------------------------------------------------------------- */

/*
 * Remove fromUnicode fallbacks and SUB mappings which are irrelevant for
 * the toUnicode table.
 * This includes mappings with MBCS_FROM_U_EXT_FLAG which were suitable
 * for the base toUnicode table but not for the base fromUnicode table.
 * The table must be sorted.
 * Modifies previous data in the reverseMap.
 */
static int32_t
reduceToUMappings(UCMTable *table) {
    UCMapping *mappings;
    int32_t *map;
    int32_t i, j, count;
    int8_t flag;

    mappings=table->mappings;
    map=table->reverseMap;
    count=table->mappingsLength;

    /* leave the map alone for the initial mappings with desired flags */
    for(i=j=0; i<count; ++i) {
        flag=mappings[map[i]].f;
        if(flag!=0 && flag!=3) {
            break;
        }
    }

    /* reduce from here to the rest */
    for(j=i; i<count; ++i) {
        flag=mappings[map[i]].f;
        if(flag==0 || flag==3) {
            map[j++]=map[i];
        }
    }

    return j;
}

static uint32_t
getToUnicodeValue(CnvExtData *extData, UCMTable *table, UCMapping *m) {
    UChar32 *u32;
    UChar *u;
    uint32_t value;
    int32_t u16Length, ratio;
    UErrorCode errorCode;

    /* write the Unicode result code point or string index */
    if(m->uLen==1) {
        u16Length=U16_LENGTH(m->u);
        value=(uint32_t)(UCNV_EXT_TO_U_MIN_CODE_POINT+m->u);
    } else {
        /* the parser enforces m->uLen<=UCNV_EXT_MAX_UCHARS */

        /* get the result code point string and its 16-bit string length */
        u32=UCM_GET_CODE_POINTS(table, m);
        errorCode=U_ZERO_ERROR;
        u_strFromUTF32(NULL, 0, &u16Length, u32, m->uLen, &errorCode);
        if(U_FAILURE(errorCode) && errorCode!=U_BUFFER_OVERFLOW_ERROR) {
            exit(errorCode);
        }

        /* allocate it and put its length and index into the value */
        value=
            (((uint32_t)u16Length+UCNV_EXT_TO_U_LENGTH_OFFSET)<<UCNV_EXT_TO_U_LENGTH_SHIFT)|
            ((uint32_t)utm_countItems(extData->toUUChars));
        u=utm_allocN(extData->toUUChars, u16Length);

        /* write the result 16-bit string */
        errorCode=U_ZERO_ERROR;
        u_strFromUTF32(u, u16Length, NULL, u32, m->uLen, &errorCode);
        if(U_FAILURE(errorCode) && errorCode!=U_BUFFER_OVERFLOW_ERROR) {
            exit(errorCode);
        }
    }
    if(m->f==0) {
        value|=UCNV_EXT_TO_U_ROUNDTRIP_FLAG;
    }

    /* update statistics */
    if(m->bLen>extData->maxInBytes) {
        extData->maxInBytes=m->bLen;
    }
    if(u16Length>extData->maxOutUChars) {
        extData->maxOutUChars=u16Length;
    }

    ratio=(u16Length+(m->bLen-1))/m->bLen;
    if(ratio>extData->maxUCharsPerByte) {
        extData->maxUCharsPerByte=ratio;
    }

    return value;
}

/*
 * Recursive toUTable generator core function.
 * Preconditions:
 * - start<limit (There is at least one mapping.)
 * - The mappings are sorted lexically. (Access is through the reverseMap.)
 * - All mappings between start and limit have input sequences that share
 *   the same prefix of unitIndex length, and therefore all of these sequences
 *   are at least unitIndex+1 long.
 * - There are only relevant mappings available through the reverseMap,
 *   see reduceToUMappings().
 *
 * One function invocation generates one section table.
 *
 * Steps:
 * 1. Count the number of unique unit values and get the low/high unit values
 *    that occur at unitIndex.
 * 2. Allocate the section table with possible optimization for linear access.
 * 3. Write temporary version of the section table with start indexes of
 *    subsections, each corresponding to one unit value at unitIndex.
 * 4. Iterate through the table once more, and depending on the subsection length:
 *    0: write 0 as a result value (unused byte in linear-access section table)
 *   >0: if there is one mapping with an input unit sequence of unitIndex+1
 *       then defaultValue=compute the mapping result for this whole sequence
 *       else defaultValue=0
 *
 *       recurse into the subsection
 */
static UBool
generateToUTable(CnvExtData *extData, UCMTable *table,
                 int32_t start, int32_t limit, int32_t unitIndex,
                 uint32_t defaultValue) {
    UCMapping *mappings, *m;
    int32_t *map;
    int32_t i, j, uniqueCount, count, subStart, subLimit;

    uint8_t *bytes;
    int32_t low, high, prev;

    uint32_t *section;

    mappings=table->mappings;
    map=table->reverseMap;

    /* step 1: examine the input units; set low, high, uniqueCount */
    m=mappings+map[start];
    bytes=UCM_GET_BYTES(table, m);
    low=bytes[unitIndex];
    uniqueCount=1;

    prev=high=low;
    for(i=start+1; i<limit; ++i) {
        m=mappings+map[i];
        bytes=UCM_GET_BYTES(table, m);
        high=bytes[unitIndex];

        if(high!=prev) {
            prev=high;
            ++uniqueCount;
        }
    }

    /* step 2: allocate the section; set count, section */
    count=(high-low)+1;
    if(count<0x100 && (unitIndex==0 || uniqueCount>=(3*count)/4)) {
        /*
         * for the root table and for fairly full tables:
         * allocate for direct, linear array access
         * by keeping count, to write an entry for each unit value
         * from low to high
         * exception: use a compact table if count==0x100 because
         * that cannot be encoded in the length byte
         */
    } else {
        count=uniqueCount;
    }

    if(count>=0x100) {
        fprintf(stderr, "error: toUnicode extension table section overflow: %ld section entries\n", (long)count);
        return FALSE;
    }

    /* allocate the section: 1 entry for the header + count for the items */
    section=(uint32_t *)utm_allocN(extData->toUTable, 1+count);

    /* write the section header */
    *section++=((uint32_t)count<<UCNV_EXT_TO_U_BYTE_SHIFT)|defaultValue;

    /* step 3: write temporary section table with subsection starts */
    prev=low-1; /* just before low to prevent empty subsections before low */
    j=0; /* section table index */
    for(i=start; i<limit; ++i) {
        m=mappings+map[i];
        bytes=UCM_GET_BYTES(table, m);
        high=bytes[unitIndex];

        if(high!=prev) {
            /* start of a new subsection for unit high */
            if(count>uniqueCount) {
                /* write empty subsections for unused units in a linear table */
                while(++prev<high) {
                    section[j++]=((uint32_t)prev<<UCNV_EXT_TO_U_BYTE_SHIFT)|(uint32_t)i;
                }
            } else {
                prev=high;
            }

            /* write the entry with the subsection start */
            section[j++]=((uint32_t)high<<UCNV_EXT_TO_U_BYTE_SHIFT)|(uint32_t)i;
        }
    }
    /* assert(j==count) */

    /* step 4: recurse and write results */
    subLimit=UCNV_EXT_TO_U_GET_VALUE(section[0]);
    for(j=0; j<count; ++j) {
        subStart=subLimit;
        subLimit= (j+1)<count ? UCNV_EXT_TO_U_GET_VALUE(section[j+1]) : limit;

        /* remove the subStart temporary value */
        section[j]&=~UCNV_EXT_TO_U_VALUE_MASK;

        if(subStart==subLimit) {
            /* leave the value zero: empty subsection for unused unit in a linear table */
            continue;
        }

        /* see if there is exactly one input unit sequence of length unitIndex+1 */
        defaultValue=0;
        m=mappings+map[subStart];
        if(m->bLen==unitIndex+1) {
            /* do not include this in generateToUTable() */
            ++subStart;

            if(subStart<subLimit && mappings[map[subStart]].bLen==unitIndex+1) {
                /* print error for multiple same-input-sequence mappings */
                fprintf(stderr, "error: multiple mappings from same bytes\n");
                ucm_printMapping(table, m, stderr);
                ucm_printMapping(table, mappings+map[subStart], stderr);
                return FALSE;
            }

            defaultValue=getToUnicodeValue(extData, table, m);
        }

        if(subStart==subLimit) {
            /* write the result for the input sequence ending here */
            section[j]|=defaultValue;
        } else {
            /* write the index to the subsection table */
            section[j]|=(uint32_t)utm_countItems(extData->toUTable);

            /* recurse */
            if(!generateToUTable(extData, table, subStart, subLimit, unitIndex+1, defaultValue)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

/*
 * Generate the toUTable and toUUChars from the input table.
 * The input table must be sorted, and all precision flags must be 0..3.
 * This function will modify the table's reverseMap.
 */
static UBool
makeToUTable(CnvExtData *extData, UCMTable *table) {
    int32_t toUCount;

    toUCount=reduceToUMappings(table);

    extData->toUTable=utm_open("cnv extension toUTable", 0x10000, UCNV_EXT_TO_U_MIN_CODE_POINT, 4);
    extData->toUUChars=utm_open("cnv extension toUUChars", 0x10000, UCNV_EXT_TO_U_INDEX_MASK+1, 2);

    return generateToUTable(extData, table, 0, toUCount, 0, 0);
}

/* from Unicode ------------------------------------------------------------- */

/*
 * preprocessing:
 * rebuild reverseMap with mapping indexes for mappings relevant for from Unicode
 * change each Unicode string to encode all but the first code point in 16-bit form
 *
 * generation:
 * for each unique code point
 *   write an entry in the 3-stage trie
 *   check that there is only one single-code point sequence
 *   start recursion for following 16-bit input units
 */

/*
 * Remove toUnicode fallbacks and non-<subchar1> SUB mappings
 * which are irrelevant for the fromUnicode extension table.
 * Remove MBCS_FROM_U_EXT_FLAG bits.
 * Overwrite the reverseMap with an index array to the relevant mappings.
 * Modify the code point sequences to a generator-friendly format where
 * the first code points remains unchanged but the following are recoded
 * into 16-bit Unicode string form.
 * The table must be sorted.
 * Destroys previous data in the reverseMap.
 */
static int32_t
prepareFromUMappings(UCMTable *table) {
    UCMapping *mappings, *m;
    int32_t *map;
    int32_t i, j, count;
    int8_t flag;

    mappings=table->mappings;
    map=table->reverseMap;
    count=table->mappingsLength;

    /*
     * we do not go through the map on input because the mappings are
     * sorted lexically
     */
    m=mappings;

    for(i=j=0; i<count; ++m, ++i) {
        flag=m->f;
        if(flag>=0) {
            flag&=MBCS_FROM_U_EXT_MASK;
            m->f=flag;
        }
        if(flag==0 || flag==1 || (flag==2 && m->bLen==1) || flag==4) {
            map[j++]=i;

            if(m->uLen>1) {
                /* recode all but the first code point to 16-bit Unicode */
                UChar32 *u32;
                UChar *u;
                UChar32 c;
                int32_t q, r;

                u32=UCM_GET_CODE_POINTS(table, m);
                u=(UChar *)u32; /* destructive in-place recoding */
                for(r=2, q=1; q<m->uLen; ++q) {
                    c=u32[q];
                    U16_APPEND_UNSAFE(u, r, c);
                }

                /* counts the first code point always at 2 - the first 16-bit unit is at 16-bit index 2 */
                m->uLen=(int8_t)r;
            }
        }
    }

    return j;
}

static uint32_t
getFromUBytesValue(CnvExtData *extData, UCMTable *table, UCMapping *m) {
    uint8_t *bytes, *resultBytes;
    uint32_t value;
    int32_t u16Length, ratio;

    if(m->f==2) {
        /*
         * no mapping, <subchar1> preferred
         *
         * no need to count in statistics because the subchars are already
         * counted for maxOutBytes and maxBytesPerUChar in UConverterStaticData,
         * and this non-mapping does not count for maxInUChars which are always
         * trivially at least two if counting unmappable supplementary code points
         */
        return UCNV_EXT_FROM_U_SUBCHAR1;
    }

    bytes=UCM_GET_BYTES(table, m);
    value=0;
    switch(m->bLen) {
        /* 1..3: store the bytes in the value word */
    case 3:
        value=((uint32_t)*bytes++)<<16;
    case 2:
        value|=((uint32_t)*bytes++)<<8;
    case 1:
        value|=*bytes;
        break;
    default:
        /* the parser enforces m->bLen<=UCNV_EXT_MAX_BYTES */
        /* store the bytes in fromUBytes[] and the index in the value word */
        value=(uint32_t)utm_countItems(extData->fromUBytes);
        resultBytes=utm_allocN(extData->fromUBytes, m->bLen);
        uprv_memcpy(resultBytes, bytes, m->bLen);
        break;
    }
    value|=(uint32_t)m->bLen<<UCNV_EXT_FROM_U_LENGTH_SHIFT;
    if(m->f==0) {
        value|=UCNV_EXT_FROM_U_ROUNDTRIP_FLAG;
    } else if(m->f==4) {
        value|=UCNV_EXT_FROM_U_GOOD_ONE_WAY_FLAG;
    }

    /* calculate the real UTF-16 length (see recoding in prepareFromUMappings()) */
    if(m->uLen==1) {
        u16Length=U16_LENGTH(m->u);
    } else {
        u16Length=U16_LENGTH(UCM_GET_CODE_POINTS(table, m)[0])+(m->uLen-2);
    }

    /* update statistics */
    if(u16Length>extData->maxInUChars) {
        extData->maxInUChars=u16Length;
    }
    if(m->bLen>extData->maxOutBytes) {
        extData->maxOutBytes=m->bLen;
    }

    ratio=(m->bLen+(u16Length-1))/u16Length;
    if(ratio>extData->maxBytesPerUChar) {
        extData->maxBytesPerUChar=ratio;
    }

    return value;
}

/*
 * works like generateToUTable(), except that the
 * output section consists of two arrays, one for input UChars and one
 * for result values
 *
 * also, fromUTable sections are always stored in a compact form for
 * access via binary search
 */
static UBool
generateFromUTable(CnvExtData *extData, UCMTable *table,
                   int32_t start, int32_t limit, int32_t unitIndex,
                   uint32_t defaultValue) {
    UCMapping *mappings, *m;
    int32_t *map;
    int32_t i, j, uniqueCount, count, subStart, subLimit;

    UChar *uchars;
    UChar32 low, high, prev;

    UChar *sectionUChars;
    uint32_t *sectionValues;

    mappings=table->mappings;
    map=table->reverseMap;

    /* step 1: examine the input units; set low, high, uniqueCount */
    m=mappings+map[start];
    uchars=(UChar *)UCM_GET_CODE_POINTS(table, m);
    low=uchars[unitIndex];
    uniqueCount=1;

    prev=high=low;
    for(i=start+1; i<limit; ++i) {
        m=mappings+map[i];
        uchars=(UChar *)UCM_GET_CODE_POINTS(table, m);
        high=uchars[unitIndex];

        if(high!=prev) {
            prev=high;
            ++uniqueCount;
        }
    }

    /* step 2: allocate the section; set count, section */
    /* the fromUTable always stores for access via binary search */
    count=uniqueCount;

    /* allocate the section: 1 entry for the header + count for the items */
    sectionUChars=(UChar *)utm_allocN(extData->fromUTableUChars, 1+count);
    sectionValues=(uint32_t *)utm_allocN(extData->fromUTableValues, 1+count);

    /* write the section header */
    *sectionUChars++=(UChar)count;
    *sectionValues++=defaultValue;

    /* step 3: write temporary section table with subsection starts */
    prev=low-1; /* just before low to prevent empty subsections before low */
    j=0; /* section table index */
    for(i=start; i<limit; ++i) {
        m=mappings+map[i];
        uchars=(UChar *)UCM_GET_CODE_POINTS(table, m);
        high=uchars[unitIndex];

        if(high!=prev) {
            /* start of a new subsection for unit high */
            prev=high;

            /* write the entry with the subsection start */
            sectionUChars[j]=(UChar)high;
            sectionValues[j]=(uint32_t)i;
            ++j;
        }
    }
    /* assert(j==count) */

    /* step 4: recurse and write results */
    subLimit=(int32_t)(sectionValues[0]);
    for(j=0; j<count; ++j) {
        subStart=subLimit;
        subLimit= (j+1)<count ? (int32_t)(sectionValues[j+1]) : limit;

        /* see if there is exactly one input unit sequence of length unitIndex+1 */
        defaultValue=0;
        m=mappings+map[subStart];
        if(m->uLen==unitIndex+1) {
            /* do not include this in generateToUTable() */
            ++subStart;

            if(subStart<subLimit && mappings[map[subStart]].uLen==unitIndex+1) {
                /* print error for multiple same-input-sequence mappings */
                fprintf(stderr, "error: multiple mappings from same Unicode code points\n");
                ucm_printMapping(table, m, stderr);
                ucm_printMapping(table, mappings+map[subStart], stderr);
                return FALSE;
            }

            defaultValue=getFromUBytesValue(extData, table, m);
        }

        if(subStart==subLimit) {
            /* write the result for the input sequence ending here */
            sectionValues[j]=defaultValue;
        } else {
            /* write the index to the subsection table */
            sectionValues[j]=(uint32_t)utm_countItems(extData->fromUTableValues);

            /* recurse */
            if(!generateFromUTable(extData, table, subStart, subLimit, unitIndex+1, defaultValue)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

/*
 * add entries to the fromUnicode trie,
 * assume to be called with code points in ascending order
 * and use that to build the trie in precompacted form
 */
static void
addFromUTrieEntry(CnvExtData *extData, UChar32 c, uint32_t value) {
    int32_t i1, i2, i3, i3b, nextOffset, min, newBlock;

    if(value==0) {
        return;
    }

    /*
     * compute the index for each stage,
     * allocate a stage block if necessary,
     * and write the stage value
     */
    i1=c>>10;
    if(i1>=extData->stage1Top) {
        extData->stage1Top=i1+1;
    }

    nextOffset=(c>>4)&0x3f;

    if(extData->stage1[i1]==0) {
        /* allocate another block in stage 2; overlap with the previous block */
        newBlock=extData->stage2Top;
        min=newBlock-nextOffset; /* minimum block start with overlap */
        while(min<newBlock && extData->stage2[newBlock-1]==0) {
            --newBlock;
        }

        extData->stage1[i1]=(uint16_t)newBlock;
        extData->stage2Top=newBlock+MBCS_STAGE_2_BLOCK_SIZE;
        if(extData->stage2Top>UPRV_LENGTHOF(extData->stage2)) {
            fprintf(stderr, "error: too many stage 2 entries at U+%04x\n", (int)c);
            exit(U_MEMORY_ALLOCATION_ERROR);
        }
    }

    i2=extData->stage1[i1]+nextOffset;
    nextOffset=c&0xf;

    if(extData->stage2[i2]==0) {
        /* allocate another block in stage 3; overlap with the previous block */
        newBlock=extData->stage3Top;
        min=newBlock-nextOffset; /* minimum block start with overlap */
        while(min<newBlock && extData->stage3[newBlock-1]==0) {
            --newBlock;
        }

        /* round up to a multiple of stage 3 granularity >1 (similar to utrie.c) */
        newBlock=(newBlock+(UCNV_EXT_STAGE_3_GRANULARITY-1))&~(UCNV_EXT_STAGE_3_GRANULARITY-1);
        extData->stage2[i2]=(uint16_t)(newBlock>>UCNV_EXT_STAGE_2_LEFT_SHIFT);

        extData->stage3Top=newBlock+MBCS_STAGE_3_BLOCK_SIZE;
        if(extData->stage3Top>UPRV_LENGTHOF(extData->stage3)) {
            fprintf(stderr, "error: too many stage 3 entries at U+%04x\n", (int)c);
            exit(U_MEMORY_ALLOCATION_ERROR);
        }
    }

    i3=((int32_t)extData->stage2[i2]<<UCNV_EXT_STAGE_2_LEFT_SHIFT)+nextOffset;
    /*
     * assume extData->stage3[i3]==0 because we get
     * code points in strictly ascending order
     */

    if(value==UCNV_EXT_FROM_U_SUBCHAR1) {
        /* <subchar1> SUB mapping, see getFromUBytesValue() and prepareFromUMappings() */
        extData->stage3[i3]=1;

        /*
         * precompaction is not optimal for <subchar1> |2 mappings because
         * stage3 values for them are all the same, unlike for other mappings
         * which all have unique values;
         * use a simple compaction of reusing a whole block filled with these
         * mappings
         */

        /* is the entire block filled with <subchar1> |2 mappings? */
        if(nextOffset==MBCS_STAGE_3_BLOCK_SIZE-1) {
            for(min=i3-nextOffset;
                min<i3 && extData->stage3[min]==1;
                ++min) {}

            if(min==i3) {
                /* the entire block is filled with these mappings */
                if(extData->stage3Sub1Block!=0) {
                    /* point to the previous such block and remove this block from stage3 */
                    extData->stage2[i2]=extData->stage3Sub1Block;
                    extData->stage3Top-=MBCS_STAGE_3_BLOCK_SIZE;
                    uprv_memset(extData->stage3+extData->stage3Top, 0, MBCS_STAGE_3_BLOCK_SIZE*2);
                } else {
                    /* remember this block's stage2 entry */
                    extData->stage3Sub1Block=extData->stage2[i2];
                }
            }
        }
    } else {
        if((i3b=extData->stage3bTop++)>=UPRV_LENGTHOF(extData->stage3b)) {
            fprintf(stderr, "error: too many stage 3b entries at U+%04x\n", (int)c);
            exit(U_MEMORY_ALLOCATION_ERROR);
        }

        /* roundtrip or fallback mapping */
        extData->stage3[i3]=(uint16_t)i3b;
        extData->stage3b[i3b]=value;
    }
}

static UBool
generateFromUTrie(CnvExtData *extData, UCMTable *table, int32_t mapLength) {
    UCMapping *mappings, *m;
    int32_t *map;
    uint32_t value;
    int32_t subStart, subLimit;

    UChar32 *codePoints;
    UChar32 c, next;

    if(mapLength==0) {
        return TRUE;
    }

    mappings=table->mappings;
    map=table->reverseMap;

    /*
     * iterate over same-initial-code point mappings,
     * enter the initial code point into the trie,
     * and start a recursion on the corresponding mappings section
     * with generateFromUTable()
     */
    m=mappings+map[0];
    codePoints=UCM_GET_CODE_POINTS(table, m);
    next=codePoints[0];
    subLimit=0;
    while(subLimit<mapLength) {
        /* get a new subsection of mappings starting with the same code point */
        subStart=subLimit;
        c=next;
        while(next==c && ++subLimit<mapLength) {
            m=mappings+map[subLimit];
            codePoints=UCM_GET_CODE_POINTS(table, m);
            next=codePoints[0];
        }

        /*
         * compute the value for this code point;
         * if there is a mapping for this code point alone, it is at subStart
         * because the table is sorted lexically
         */
        value=0;
        m=mappings+map[subStart];
        codePoints=UCM_GET_CODE_POINTS(table, m);
        if(m->uLen==1) {
            /* do not include this in generateFromUTable() */
            ++subStart;

            if(subStart<subLimit && mappings[map[subStart]].uLen==1) {
                /* print error for multiple same-input-sequence mappings */
                fprintf(stderr, "error: multiple mappings from same Unicode code points\n");
                ucm_printMapping(table, m, stderr);
                ucm_printMapping(table, mappings+map[subStart], stderr);
                return FALSE;
            }

            value=getFromUBytesValue(extData, table, m);
        }

        if(subStart==subLimit) {
            /* write the result for this one code point */
            addFromUTrieEntry(extData, c, value);
        } else {
            /* write the index to the subsection table */
            addFromUTrieEntry(extData, c, (uint32_t)utm_countItems(extData->fromUTableValues));

            /* recurse, starting from 16-bit-unit index 2, the first 16-bit unit after c */
            if(!generateFromUTable(extData, table, subStart, subLimit, 2, value)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

/*
 * Generate the fromU data structures from the input table.
 * The input table must be sorted, and all precision flags must be 0..3.
 * This function will modify the table's reverseMap.
 */
static UBool
makeFromUTable(CnvExtData *extData, UCMTable *table) {
    uint16_t *stage1;
    int32_t i, stage1Top, fromUCount;

    fromUCount=prepareFromUMappings(table);

    extData->fromUTableUChars=utm_open("cnv extension fromUTableUChars", 0x10000, UCNV_EXT_FROM_U_DATA_MASK+1, 2);
    extData->fromUTableValues=utm_open("cnv extension fromUTableValues", 0x10000, UCNV_EXT_FROM_U_DATA_MASK+1, 4);
    extData->fromUBytes=utm_open("cnv extension fromUBytes", 0x10000, UCNV_EXT_FROM_U_DATA_MASK+1, 1);

    /* allocate all-unassigned stage blocks */
    extData->stage2Top=MBCS_STAGE_2_FIRST_ASSIGNED;
    extData->stage3Top=MBCS_STAGE_3_FIRST_ASSIGNED;

    /*
     * stage 3b stores only unique values, and in
     * index 0: 0 for "no mapping"
     * index 1: "no mapping" with preference for <subchar1> rather than <subchar>
     */
    extData->stage3b[1]=UCNV_EXT_FROM_U_SUBCHAR1;
    extData->stage3bTop=2;

    /* allocate the first entry in the fromUTable because index 0 means "no result" */
    utm_alloc(extData->fromUTableUChars);
    utm_alloc(extData->fromUTableValues);

    if(!generateFromUTrie(extData, table, fromUCount)) {
        return FALSE;
    }

    /*
     * offset the stage 1 trie entries by stage1Top because they will
     * be stored in a single array
     */
    stage1=extData->stage1;
    stage1Top=extData->stage1Top;
    for(i=0; i<stage1Top; ++i) {
        stage1[i]=(uint16_t)(stage1[i]+stage1Top);
    }

    return TRUE;
}

/* -------------------------------------------------------------------------- */

static UBool
CnvExtAddTable(NewConverter *cnvData, UCMTable *table, UConverterStaticData *staticData) {
    CnvExtData *extData;

    if(table->unicodeMask&UCNV_HAS_SURROGATES) {
        fprintf(stderr, "error: contains mappings for surrogate code points\n");
        return FALSE;
    }

    staticData->conversionType=UCNV_MBCS;

    extData=(CnvExtData *)cnvData;

    /*
     * assume that the table is sorted
     *
     * call the functions in this order because
     * makeToUTable() modifies the original reverseMap,
     * makeFromUTable() writes a whole new mapping into reverseMap
     */
    return
        makeToUTable(extData, table) &&
        makeFromUTable(extData, table);
}
