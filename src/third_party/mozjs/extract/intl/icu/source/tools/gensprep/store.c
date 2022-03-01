// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 1999-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  store.c
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003-02-06
*   created by: Ram Viswanadha
*
*/

#include <stdio.h>
#include <stdlib.h>
#include "unicode/utypes.h"
#include "cmemory.h"
#include "cstring.h"
#include "filestrm.h"
#include "toolutil.h"
#include "unicode/udata.h"
#include "unicode/utf16.h"
#include "utrie.h"
#include "unewdata.h"
#include "gensprep.h"
#include "uhash.h"


#define DO_DEBUG_OUT 0


/*
 * StringPrep profile file format ------------------------------------
 *
 * The file format prepared and written here contains a 16-bit trie and a mapping table.
 *
 * Before the data contents described below, there are the headers required by
 * the udata API for loading ICU data. Especially, a UDataInfo structure
 * precedes the actual data. It contains platform properties values and the
 * file format version.
 *
 * The following is a description of format version 2.
 *
 * Data contents:
 *
 * The contents is a parsed, binary form of RFC3454 and possibly
 * NormalizationCorrections.txt depending on the options specified on the profile.
 *
 * Any Unicode code point from 0 to 0x10ffff can be looked up to get
 * the trie-word, if any, for that code point. This means that the input
 * to the lookup are 21-bit unsigned integers, with not all of the
 * 21-bit range used.
 *
 * *.spp files customarily begin with a UDataInfo structure, see udata.h and .c.
 * After that there are the following structures:
 *
 * int32_t indexes[_SPREP_INDEX_TOP];           -- _SPREP_INDEX_TOP=16, see enum in sprpimpl.h file
 *
 * UTrie stringPrepTrie;                        -- size in bytes=indexes[_SPREP_INDEX_TRIE_SIZE]
 *
 * uint16_t mappingTable[];                     -- Contains the sequecence of code units that the code point maps to
 *                                                 size in bytes = indexes[_SPREP_INDEX_MAPPING_DATA_SIZE]
 *
 * The indexes array contains the following values:
 *  indexes[_SPREP_INDEX_TRIE_SIZE]                  -- The size of the StringPrep trie in bytes
 *  indexes[_SPREP_INDEX_MAPPING_DATA_SIZE]          -- The size of the mappingTable in bytes
 *  indexes[_SPREP_NORM_CORRECTNS_LAST_UNI_VERSION]  -- The index of Unicode version of last entry in NormalizationCorrections.txt
 *  indexes[_SPREP_ONE_UCHAR_MAPPING_INDEX_START]    -- The starting index of 1 UChar  mapping index in the mapping table
 *  indexes[_SPREP_TWO_UCHARS_MAPPING_INDEX_START]   -- The starting index of 2 UChars mapping index in the mapping table
 *  indexes[_SPREP_THREE_UCHARS_MAPPING_INDEX_START] -- The starting index of 3 UChars mapping index in the mapping table
 *  indexes[_SPREP_FOUR_UCHARS_MAPPING_INDEX_START]  -- The starting index of 4 UChars mapping index in the mapping table
 *  indexes[_SPREP_OPTIONS]                          -- Bit set of options to turn on in the profile, e.g: USPREP_NORMALIZATION_ON, USPREP_CHECK_BIDI_ON
 *
 *
 * StringPrep Trie :
 *
 * The StringPrep tries is a 16-bit trie that contains data for the profile.
 * Each code point is associated with a value (trie-word) in the trie.
 *
 * - structure of data words from the trie
 *
 *  i)  A value greater than or equal to _SPREP_TYPE_THRESHOLD (0xFFF0)
 *      represents the type associated with the code point
 *      if(trieWord >= _SPREP_TYPE_THRESHOLD){
 *          type = trieWord - 0xFFF0;
 *      }
 *      The type can be :
 *             USPREP_UNASSIGNED
 *             USPREP_PROHIBITED
 *             USPREP_DELETE
 *
 *  ii) A value less than _SPREP_TYPE_THRESHOLD means the type is USPREP_MAP and
 *      contains distribution described below
 *
 *      0       -  ON : The code point is prohibited (USPREP_PROHIBITED). This is to allow for codepoint that are both prohibited and mapped.
 *      1       -  ON : The value in the next 14 bits is an index into the mapping table
 *                 OFF: The value in the next 14 bits is an delta value from the code point
 *      2..15   -  Contains data as described by bit 1. If all bits are set
 *                 (value = _SPREP_MAX_INDEX_VALUE) then the type is USPREP_DELETE
 *
 *
 * Mapping Table:
 * The data in mapping table is sorted according to the length of the mapping sequence.
 * If the type of the code point is USPREP_MAP and value in trie word is an index, the index
 * is compared with start indexes of sequence length start to figure out the length according to
 * the following algorithm:
 *
 *              if(       index >= indexes[_SPREP_ONE_UCHAR_MAPPING_INDEX_START] &&
 *                        index < indexes[_SPREP_TWO_UCHARS_MAPPING_INDEX_START]){
 *                   length = 1;
 *               }else if(index >= indexes[_SPREP_TWO_UCHARS_MAPPING_INDEX_START] &&
 *                        index < indexes[_SPREP_THREE_UCHARS_MAPPING_INDEX_START]){
 *                   length = 2;
 *               }else if(index >= indexes[_SPREP_THREE_UCHARS_MAPPING_INDEX_START] &&
 *                        index < indexes[_SPREP_FOUR_UCHARS_MAPPING_INDEX_START]){
 *                   length = 3;
 *               }else{
 *                   // The first position in the mapping table contains the length
 *                   // of the sequence
 *                   length = mappingTable[index++];
 *
 *               }
 *
 */

/* file data ---------------------------------------------------------------- */
/* indexes[] value names */

#if UCONFIG_NO_IDNA

/* dummy UDataInfo cf. udata.h */
static UDataInfo dataInfo = {
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    U_SIZEOF_UCHAR,
    0,

    { 0, 0, 0, 0 },                 /* dummy dataFormat */
    { 0, 0, 0, 0 },                 /* dummy formatVersion */
    { 0, 0, 0, 0 }                  /* dummy dataVersion */
};

#else

static int32_t indexes[_SPREP_INDEX_TOP]={ 0 };

static uint16_t* mappingData= NULL;
static int32_t mappingDataCapacity = 0; /* we skip the first index in mapping data */
static int16_t currentIndex = 0; /* the current index into the data trie */
static int32_t maxLength = 0;  /* maximum length of mapping string */


/* UDataInfo cf. udata.h */
static UDataInfo dataInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    U_SIZEOF_UCHAR,
    0,

    { 0x53, 0x50, 0x52, 0x50 },                 /* dataFormat="SPRP" */
    { 3, 2, UTRIE_SHIFT, UTRIE_INDEX_SHIFT },   /* formatVersion */
    { 3, 2, 0, 0 }                              /* dataVersion (Unicode version) */
};
void
setUnicodeVersion(const char *v) {
    UVersionInfo version;
    u_versionFromString(version, v);
    uprv_memcpy(dataInfo.dataVersion, version, 4);
}

void
setUnicodeVersionNC(UVersionInfo version){
    uint32_t univer = version[0] << 24;
    univer += version[1] << 16;
    univer += version[2] << 8;
    univer += version[3];
    indexes[_SPREP_NORM_CORRECTNS_LAST_UNI_VERSION] = univer;
}
static UNewTrie *sprepTrie;

#define MAX_DATA_LENGTH 11500


#define SPREP_DELTA_RANGE_POSITIVE_LIMIT              8191
#define SPREP_DELTA_RANGE_NEGATIVE_LIMIT              -8192


extern void
init() {

    sprepTrie = (UNewTrie *)uprv_calloc(1, sizeof(UNewTrie));

    /* initialize the two tries */
    if(NULL==utrie_open(sprepTrie, NULL, MAX_DATA_LENGTH, 0, 0, FALSE)) {
        fprintf(stderr, "error: failed to initialize tries\n");
        exit(U_MEMORY_ALLOCATION_ERROR);
    }
}

static UHashtable* hashTable = NULL;


typedef struct ValueStruct {
    UChar* mapping;
    int16_t length;
    UStringPrepType type;
} ValueStruct;

/* Callback for deleting the value from the hashtable */
static void U_CALLCONV valueDeleter(void* obj){
    ValueStruct* value = (ValueStruct*) obj;
    uprv_free(value->mapping);
    uprv_free(value);
}

/* Callback for hashing the entry */
static int32_t U_CALLCONV hashEntry(const UHashTok parm) {
    return  parm.integer;
}

/* Callback for comparing two entries */
static UBool U_CALLCONV compareEntries(const UHashTok p1, const UHashTok p2) {
    return (UBool)(p1.integer != p2.integer);
}


static void
storeMappingData(void){

    int32_t pos = UHASH_FIRST;
    const UHashElement* element = NULL;
    ValueStruct* value  = NULL;
    int32_t codepoint = 0;
    int32_t elementCount = 0;
    int32_t writtenElementCount = 0;
    int32_t mappingLength = 1; /* minimum mapping length */
    int32_t oldMappingLength = 0;
    uint16_t trieWord =0;
    int32_t limitIndex = 0;

    if (hashTable == NULL) {
        return;
    }
    elementCount = uhash_count(hashTable);

	/*initialize the mapping data */
    mappingData = (uint16_t*) uprv_calloc(mappingDataCapacity, U_SIZEOF_UCHAR);

    while(writtenElementCount < elementCount){

        while( (element = uhash_nextElement(hashTable, &pos))!=NULL){

            codepoint = element->key.integer;
            value = (ValueStruct*)element->value.pointer;

            /* store the start of indexes */
            if(oldMappingLength != mappingLength){
                /* Assume that index[] is used according to the enums defined */
                if(oldMappingLength <=_SPREP_MAX_INDEX_TOP_LENGTH){
                    indexes[_SPREP_NORM_CORRECTNS_LAST_UNI_VERSION+mappingLength] = currentIndex;
                }
                if(oldMappingLength <= _SPREP_MAX_INDEX_TOP_LENGTH &&
                   mappingLength == _SPREP_MAX_INDEX_TOP_LENGTH +1){

                    limitIndex = currentIndex;

                }
                oldMappingLength = mappingLength;
            }

            if(value->length == mappingLength){
                uint32_t savedTrieWord = 0;
                trieWord = currentIndex << 2;
                /* turn on the 2nd bit to signal that the following bits contain an index */
                trieWord += 0x02;

                if(trieWord > _SPREP_TYPE_THRESHOLD){
                    fprintf(stderr,"trieWord cannot contain value greater than 0x%04X.\n",_SPREP_TYPE_THRESHOLD);
                    exit(U_ILLEGAL_CHAR_FOUND);
                }
                /* figure out if the code point has type already stored */
                savedTrieWord= utrie_get32(sprepTrie,codepoint,NULL);
                if(savedTrieWord!=0){
                    if((savedTrieWord- _SPREP_TYPE_THRESHOLD) == USPREP_PROHIBITED){
                        /* turn on the first bit in trie word */
                        trieWord += 0x01;
                    }else{
                        /*
                         * the codepoint has value something other than prohibited
                         * and a mapping .. error!
                         */
                        fprintf(stderr,"Type for codepoint \\U%08X already set!.\n", (int)codepoint);
                        exit(U_ILLEGAL_ARGUMENT_ERROR);
                    }
                }

                /* now set the value in the trie */
                if(!utrie_set32(sprepTrie,codepoint,trieWord)){
                    fprintf(stderr,"Could not set the value for code point.\n");
                    exit(U_ILLEGAL_ARGUMENT_ERROR);
                }

                /* written the trie word for the codepoint... increment the count*/
                writtenElementCount++;

                /* sanity check are we exceeding the max number allowed */
                if(currentIndex+value->length+1 > _SPREP_MAX_INDEX_VALUE){
                    fprintf(stderr, "Too many entries in the mapping table %i. Maximum allowed is %i\n",
                        currentIndex+value->length, _SPREP_MAX_INDEX_VALUE);
                    exit(U_INDEX_OUTOFBOUNDS_ERROR);
                }

                /* copy the mapping data */
                /* write the length */
                if(mappingLength > _SPREP_MAX_INDEX_TOP_LENGTH ){
                     /* the cast here is safe since we donot expect the length to be > 65535 */
                     mappingData[currentIndex++] = (uint16_t) mappingLength;
                }
                /* copy the contents to mappindData array */
                u_memmove(mappingData+currentIndex, value->mapping, value->length);
                currentIndex += value->length;
                if (currentIndex > mappingDataCapacity) {
                    /* If this happens there is a bug in the computation of the mapping data size in storeMapping() */
                    fprintf(stderr, "gensprep, fatal error at %s, %d.  Aborting.\n", __FILE__, __LINE__);
                    exit(U_INTERNAL_PROGRAM_ERROR);
                }
            }
        }
        mappingLength++;
        pos = -1;
    }
    /* set the last length for range check */
    if(mappingLength <= _SPREP_MAX_INDEX_TOP_LENGTH){
        indexes[_SPREP_NORM_CORRECTNS_LAST_UNI_VERSION+mappingLength] = currentIndex+1;
    }else{
        indexes[_SPREP_FOUR_UCHARS_MAPPING_INDEX_START] = limitIndex;
    }

}

extern void setOptions(int32_t options){
    indexes[_SPREP_OPTIONS] = options;
}
extern void
storeMapping(uint32_t codepoint, uint32_t* mapping,int32_t length,
             UStringPrepType type, UErrorCode* status){


    UChar* map = NULL;
    int16_t adjustedLen=0, i, j;
    uint16_t trieWord = 0;
    ValueStruct *value = NULL;
    uint32_t savedTrieWord = 0;

    /* initialize the hashtable */
    if(hashTable==NULL){
        hashTable = uhash_open(hashEntry, compareEntries, NULL, status);
        uhash_setValueDeleter(hashTable, valueDeleter);
    }

    /* figure out if the code point has type already stored */
    savedTrieWord= utrie_get32(sprepTrie,codepoint,NULL);
    if(savedTrieWord!=0){
        if((savedTrieWord- _SPREP_TYPE_THRESHOLD) == USPREP_PROHIBITED){
            /* turn on the first bit in trie word */
            trieWord += 0x01;
        }else{
            /*
             * the codepoint has value something other than prohibited
             * and a mapping .. error!
             */
            fprintf(stderr,"Type for codepoint \\U%08X already set!.\n", (int)codepoint);
            exit(U_ILLEGAL_ARGUMENT_ERROR);
        }
    }

    /* figure out the real length */
    for(i=0; i<length; i++){
        adjustedLen += U16_LENGTH(mapping[i]);
    }

    if(adjustedLen == 0){
        trieWord = (uint16_t)(_SPREP_MAX_INDEX_VALUE << 2);
        /* make sure that the value of trieWord is less than the threshold */
        if(trieWord < _SPREP_TYPE_THRESHOLD){
            /* now set the value in the trie */
            if(!utrie_set32(sprepTrie,codepoint,trieWord)){
                fprintf(stderr,"Could not set the value for code point.\n");
                exit(U_ILLEGAL_ARGUMENT_ERROR);
            }
            /* value is set so just return */
            return;
        }else{
            fprintf(stderr,"trieWord cannot contain value greater than threshold 0x%04X.\n",_SPREP_TYPE_THRESHOLD);
            exit(U_ILLEGAL_CHAR_FOUND);
        }
    }

    if(adjustedLen == 1){
        /* calculate the delta */
        int16_t delta = (int16_t)((int32_t)codepoint - (int16_t) mapping[0]);
        if(delta >= SPREP_DELTA_RANGE_NEGATIVE_LIMIT && delta <= SPREP_DELTA_RANGE_POSITIVE_LIMIT){

            trieWord = delta;
            trieWord <<= 2;


            /* make sure that the second bit is OFF */
            if((trieWord & 0x02) != 0 ){
                fprintf(stderr,"The second bit in the trie word is not zero while storing a delta.\n");
                exit(U_INTERNAL_PROGRAM_ERROR);
            }
            /* make sure that the value of trieWord is less than the threshold */
            if(trieWord < _SPREP_TYPE_THRESHOLD){
                /* now set the value in the trie */
                if(!utrie_set32(sprepTrie,codepoint,trieWord)){
                    fprintf(stderr,"Could not set the value for code point.\n");
                    exit(U_ILLEGAL_ARGUMENT_ERROR);
                }
                /* value is set so just return */
                return;
            }
        }
        /*
         * if the delta is not in the given range or if the trieWord is larger than the threshold
         * just fall through for storing the mapping in the mapping table
         */
    }

    map = (UChar*) uprv_calloc(adjustedLen + 1, U_SIZEOF_UCHAR);

    for (i=0, j=0; i<length; i++) {
        U16_APPEND_UNSAFE(map, j, mapping[i]);
    }

    value = (ValueStruct*) uprv_malloc(sizeof(ValueStruct));
    value->mapping = map;
    value->type    = type;
    value->length  = adjustedLen;
    if(value->length > _SPREP_MAX_INDEX_TOP_LENGTH){
        mappingDataCapacity++;
    }
    if(maxLength < value->length){
        maxLength = value->length;
    }
    uhash_iput(hashTable,codepoint,value,status);
    mappingDataCapacity += adjustedLen;

    if(U_FAILURE(*status)){
        fprintf(stderr, "Failed to put entries into the hastable. Error: %s\n", u_errorName(*status));
        exit(*status);
    }
}


extern void
storeRange(uint32_t start, uint32_t end, UStringPrepType type, UErrorCode* status){
    (void)status; // suppress compiler warnings about unused variable
    uint16_t trieWord = 0;

    if((int)(_SPREP_TYPE_THRESHOLD + type) > 0xFFFF){
        fprintf(stderr,"trieWord cannot contain value greater than 0xFFFF.\n");
        exit(U_ILLEGAL_CHAR_FOUND);
    }
    trieWord = (_SPREP_TYPE_THRESHOLD + type); /* the top 4 bits contain the value */
    if(start == end){
        uint32_t savedTrieWord = utrie_get32(sprepTrie, start, NULL);
        if(savedTrieWord>0){
            if(savedTrieWord < _SPREP_TYPE_THRESHOLD && type == USPREP_PROHIBITED){
                /*
                 * A mapping is stored in the trie word
                 * and the only other possible type that a
                 * code point can have is USPREP_PROHIBITED
                 *
                 */

                /* turn on the 0th bit in the savedTrieWord */
                savedTrieWord += 0x01;

                /* the downcast is safe since we only save 16 bit values */
                trieWord = (uint16_t)savedTrieWord;

                /* make sure that the value of trieWord is less than the threshold */
                if(trieWord < _SPREP_TYPE_THRESHOLD){
                    /* now set the value in the trie */
                    if(!utrie_set32(sprepTrie,start,trieWord)){
                        fprintf(stderr,"Could not set the value for code point.\n");
                        exit(U_ILLEGAL_ARGUMENT_ERROR);
                    }
                    /* value is set so just return */
                    return;
                }else{
                    fprintf(stderr,"trieWord cannot contain value greater than threshold 0x%04X.\n",_SPREP_TYPE_THRESHOLD);
                    exit(U_ILLEGAL_CHAR_FOUND);
                }

            }else if(savedTrieWord != trieWord){
                fprintf(stderr,"Value for codepoint \\U%08X already set!.\n", (int)start);
                exit(U_ILLEGAL_ARGUMENT_ERROR);
            }
            /* if savedTrieWord == trieWord .. fall through and set the value */
        }
        if(!utrie_set32(sprepTrie,start,trieWord)){
            fprintf(stderr,"Could not set the value for code point \\U%08X.\n", (int)start);
            exit(U_ILLEGAL_ARGUMENT_ERROR);
        }
    }else{
        if(!utrie_setRange32(sprepTrie, start, end+1, trieWord, FALSE)){
            fprintf(stderr,"Value for certain codepoint already set.\n");
            exit(U_ILLEGAL_CHAR_FOUND);
        }
    }

}

/* folding value: just store the offset (16 bits) if there is any non-0 entry */
static uint32_t U_CALLCONV
getFoldedValue(UNewTrie *trie, UChar32 start, int32_t offset) {
    uint32_t value;
    UChar32 limit=0;
    UBool inBlockZero;

    limit=start+0x400;
    while(start<limit) {
        value=utrie_get32(trie, start, &inBlockZero);
        if(inBlockZero) {
            start+=UTRIE_DATA_BLOCK_LENGTH;
        } else if(value!=0) {
            return (uint32_t)offset;
        } else {
            ++start;
        }
    }
    return 0;

}

#endif /* #if !UCONFIG_NO_IDNA */

extern void
generateData(const char *dataDir, const char* bundleName) {
    static uint8_t sprepTrieBlock[100000];

    UNewDataMemory *pData;
    UErrorCode errorCode=U_ZERO_ERROR;
    int32_t size, dataLength;
    char* fileName = (char*) uprv_malloc(uprv_strlen(bundleName) +100);

#if UCONFIG_NO_IDNA

    size=0;

#else

    int32_t sprepTrieSize;

    /* sort and add mapping data */
    storeMappingData();

    sprepTrieSize=utrie_serialize(sprepTrie, sprepTrieBlock, sizeof(sprepTrieBlock), getFoldedValue, TRUE, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "error: utrie_serialize(sprep trie) failed, %s\n", u_errorName(errorCode));
        exit(errorCode);
    }

    size = sprepTrieSize + mappingDataCapacity*U_SIZEOF_UCHAR + sizeof(indexes);
    if(beVerbose) {
        printf("size of sprep trie              %5u bytes\n", (int)sprepTrieSize);
        printf("size of " U_ICUDATA_NAME "_%s." DATA_TYPE " contents: %ld bytes\n", bundleName,(long)size);
        printf("size of mapping data array %5u bytes\n",(int)mappingDataCapacity * U_SIZEOF_UCHAR);
        printf("Number of code units in mappingData (currentIndex) are: %i \n", currentIndex);
        printf("Maximum length of the mapping string is : %i \n", (int)maxLength);
    }

#endif

    fileName[0]=0;
    uprv_strcat(fileName,bundleName);
    /* write the data */
    pData=udata_create(dataDir, DATA_TYPE, fileName, &dataInfo,
                       haveCopyright ? U_COPYRIGHT_STRING : NULL, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "gensprep: unable to create the output file, error %d\n", errorCode);
        exit(errorCode);
    }

#if !UCONFIG_NO_IDNA

    indexes[_SPREP_INDEX_TRIE_SIZE]=sprepTrieSize;
    indexes[_SPREP_INDEX_MAPPING_DATA_SIZE]=mappingDataCapacity*U_SIZEOF_UCHAR;

    udata_writeBlock(pData, indexes, sizeof(indexes));
    udata_writeBlock(pData, sprepTrieBlock, sprepTrieSize);
    udata_writeBlock(pData, mappingData, indexes[_SPREP_INDEX_MAPPING_DATA_SIZE]);


#endif

    /* finish up */
    dataLength=udata_finish(pData, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "gensprep: error %d writing the output file\n", errorCode);
        exit(errorCode);
    }

    if(dataLength!=size) {
        fprintf(stderr, "gensprep error: data length %ld != calculated size %ld\n",
            (long)dataLength, (long)size);
        exit(U_INTERNAL_PROGRAM_ERROR);
    }

#if !UCONFIG_NO_IDNA
    /* done with writing the data .. close the hashtable */
    if (hashTable != NULL) {
        uhash_close(hashTable);
    }
#endif

    uprv_free(fileName);
}

#if !UCONFIG_NO_IDNA

extern void
cleanUpData(void) {
    uprv_free(mappingData);
    utrie_close(sprepTrie);
    uprv_free(sprepTrie);
}

#endif /* #if !UCONFIG_NO_IDNA */

/*
 * Hey, Emacs, please set the following:
 *
 * Local Variables:
 * indent-tabs-mode: nil
 * End:
 *
 */
