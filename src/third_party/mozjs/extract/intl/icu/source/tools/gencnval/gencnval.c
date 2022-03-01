// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 1999-2016 International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  gencnval.c
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 1999nov05
*   created by: Markus W. Scherer
*
*   This program reads convrtrs.txt and writes a memory-mappable
*   converter name alias table to cnvalias.dat .
*
*   This program currently writes version 2.1 of the data format. See
*   ucnv_io.c for more details on the format. Note that version 2.1
*   is written in such a way that a 2.0 reader will be able to use it,
*   and a 2.1 reader will be able to read 2.0.
*/

#include "unicode/utypes.h"
#include "unicode/putil.h"
#include "unicode/ucnv.h" /* ucnv_compareNames() */
#include "ucnv_io.h"
#include "cmemory.h"
#include "cstring.h"
#include "uinvchar.h"
#include "filestrm.h"
#include "toolutil.h"
#include "unicode/uclean.h"
#include "unewdata.h"
#include "uoptions.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* TODO: Need to check alias name length is less than UCNV_MAX_CONVERTER_NAME_LENGTH */

/* STRING_STORE_SIZE + TAG_STORE_SIZE <= ((2^16 - 1) * 2)
 That is the maximum size for the string stores combined
 because the strings are indexed at 16-bit boundaries by a
 16-bit index, and there is only one section for the 
 strings.
 */
#define STRING_STORE_SIZE 0x1FBFE   /* 130046 */
#define TAG_STORE_SIZE      0x400   /* 1024 */

/* The combined tag and converter count can affect the number of lists
 created.  The size of all lists must be less than (2^17 - 1)
 because the lists are indexed as a 16-bit array with a 16-bit index.
 */
#define MAX_TAG_COUNT 0x3F      /* 63 */
#define MAX_CONV_COUNT UCNV_CONVERTER_INDEX_MASK
#define MAX_ALIAS_COUNT 0xFFFF  /* 65535 */

/* The maximum number of aliases that a standard tag/converter combination can have.
 At this moment 6/18/2002, IANA has 12 names for ASCII. Don't go below 15 for
 this value. I don't recommend more than 31 for this value.
 */
#define MAX_TC_ALIAS_COUNT 0x1F    /* 31 */

#define MAX_LINE_SIZE 0x7FFF    /* 32767 */
#define MAX_LIST_SIZE 0xFFFF    /* 65535 */

#define DATA_NAME "cnvalias"
#define DATA_TYPE "icu" /* ICU alias table */

#define ALL_TAG_STR "ALL"
#define ALL_TAG_NUM 1
#define EMPTY_TAG_NUM 0

/* UDataInfo cf. udata.h */
static const UDataInfo dataInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    sizeof(UChar),
    0,

    {0x43, 0x76, 0x41, 0x6c},     /* dataFormat="CvAl" */
    {3, 0, 1, 0},                 /* formatVersion */
    {1, 4, 2, 0}                  /* dataVersion */
};

typedef struct {
    char *store;
    uint32_t top;
    uint32_t max;
} StringBlock;

static char stringStore[STRING_STORE_SIZE];
static StringBlock stringBlock = { stringStore, 0, STRING_STORE_SIZE };

typedef struct {
    uint16_t    aliasCount;
    uint16_t    *aliases;     /* Index into stringStore */
} AliasList;

typedef struct {
    uint16_t converter;     /* Index into stringStore */
    uint16_t totalAliasCount;    /* Total aliases in this column */
} Converter;

static Converter converters[MAX_CONV_COUNT];
static uint16_t converterCount=0;

static char tagStore[TAG_STORE_SIZE];
static StringBlock tagBlock = { tagStore, 0, TAG_STORE_SIZE };

typedef struct {
    uint16_t    tag;        /* Index into tagStore */
    uint16_t    totalAliasCount; /* Total aliases in this row */
    AliasList   aliasList[MAX_CONV_COUNT];
} Tag;

/* Think of this as a 3D array. It's tagCount by converterCount by aliasCount */
static Tag tags[MAX_TAG_COUNT];
static uint16_t tagCount = 0;

/* Used for storing all aliases  */
static uint16_t knownAliases[MAX_ALIAS_COUNT];
static uint16_t knownAliasesCount = 0;
/*static uint16_t duplicateKnownAliasesCount = 0;*/

/* Used for storing the lists section that point to aliases */
static uint16_t aliasLists[MAX_LIST_SIZE];
static uint16_t aliasListsSize = 0;

/* Were the standard tags declared before the aliases. */
static UBool standardTagsUsed = FALSE;
static UBool verbose = FALSE;
static UBool quiet = FALSE;
static int lineNum = 1;

static UConverterAliasOptions tableOptions = {
    UCNV_IO_STD_NORMALIZED,
    1 /* containsCnvOptionInfo */
};


/**
 * path to convrtrs.txt
 */
const char *path;

/* prototypes --------------------------------------------------------------- */

static void
parseLine(const char *line);

static void
parseFile(FileStream *in);

static int32_t
chomp(char *line);

static void
addOfficialTaggedStandards(char *line, int32_t lineLen);

static uint16_t
addAlias(const char *alias, uint16_t standard, uint16_t converter, UBool defaultName);

static uint16_t
addConverter(const char *converter);

static char *
allocString(StringBlock *block, const char *s, int32_t length);

static uint16_t
addToKnownAliases(const char *alias);

static int
compareAliases(const void *alias1, const void *alias2);

static uint16_t
getTagNumber(const char *tag, uint16_t tagLen);

/*static void
addTaggedAlias(uint16_t tag, const char *alias, uint16_t converter);*/

static void
writeAliasTable(UNewDataMemory *out);

/* -------------------------------------------------------------------------- */

/* Presumes that you used allocString() */
#define GET_ALIAS_STR(index) (stringStore + ((size_t)(index) << 1))
#define GET_TAG_STR(index) (tagStore + ((size_t)(index) << 1))

/* Presumes that you used allocString() */
#define GET_ALIAS_NUM(str) ((uint16_t)((str - stringStore) >> 1))
#define GET_TAG_NUM(str) ((uint16_t)((str - tagStore) >> 1))

enum
{
    HELP1,
    HELP2,
    VERBOSE,
    COPYRIGHT,
    DESTDIR,
    SOURCEDIR,
    QUIET
};

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_VERBOSE,
    UOPTION_COPYRIGHT,
    UOPTION_DESTDIR,
    UOPTION_SOURCEDIR,
    UOPTION_QUIET
};

extern int
main(int argc, char* argv[]) {
    int i, n;
    char pathBuf[512];
    FileStream *in;
    UNewDataMemory *out;
    UErrorCode errorCode=U_ZERO_ERROR;

    U_MAIN_INIT_ARGS(argc, argv);

    /* preset then read command line options */
    options[DESTDIR].value=options[SOURCEDIR].value=u_getDataDirectory();
    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    }
    if(argc<0 || options[HELP1].doesOccur || options[HELP2].doesOccur) {
        fprintf(stderr,
            "usage: %s [-options] [convrtrs.txt]\n"
            "\tread convrtrs.txt and create " U_ICUDATA_NAME "_" DATA_NAME "." DATA_TYPE "\n"
            "options:\n"
            "\t-h or -? or --help  this usage text\n"
            "\t-v or --verbose     prints out extra information about the alias table\n"
            "\t-q or --quiet       do not display warnings and progress\n"
            "\t-c or --copyright   include a copyright notice\n"
            "\t-d or --destdir     destination directory, followed by the path\n"
            "\t-s or --sourcedir   source directory, followed by the path\n",
            argv[0]);
        return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }

    if(options[VERBOSE].doesOccur) {
        verbose = TRUE;
    }

    if(options[QUIET].doesOccur) {
        quiet = TRUE;
    }

    if (argc >= 2) {
        path = argv[1];
    } else {
        path = "convrtrs.txt";
    }

    const char* sourcedir = options[SOURCEDIR].value;
    if (sourcedir != NULL && *sourcedir != 0) {
        char *end;
        uprv_strcpy(pathBuf, sourcedir);
        end = uprv_strchr(pathBuf, 0);
        if(*(end-1)!=U_FILE_SEP_CHAR) {
            *(end++)=U_FILE_SEP_CHAR;
        }
        uprv_strcpy(end, path);
        path = pathBuf;
    }

    uprv_memset(stringStore, 0, sizeof(stringStore));
    uprv_memset(tagStore, 0, sizeof(tagStore));
    uprv_memset(converters, 0, sizeof(converters));
    uprv_memset(tags, 0, sizeof(tags));
    uprv_memset(aliasLists, 0, sizeof(aliasLists));
    uprv_memset(knownAliases, 0, sizeof(aliasLists));


    in=T_FileStream_open(path, "r");
    if(in==NULL) {
        fprintf(stderr, "gencnval: unable to open input file %s\n", path);
        exit(U_FILE_ACCESS_ERROR);
    }
    parseFile(in);
    T_FileStream_close(in);

    /* create the output file */
    out=udata_create(options[DESTDIR].value, DATA_TYPE, DATA_NAME, &dataInfo,
                     options[COPYRIGHT].doesOccur ? U_COPYRIGHT_STRING : NULL, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "gencnval: unable to open output file - error %s\n", u_errorName(errorCode));
        exit(errorCode);
    }

    /* write the table of aliases based on a tag/converter name combination */
    writeAliasTable(out);

    /* finish */
    udata_finish(out, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "gencnval: error finishing output file - %s\n", u_errorName(errorCode));
        exit(errorCode);
    }

    /* clean up tags */
    for (i = 0; i < MAX_TAG_COUNT; i++) {
        for (n = 0; n < MAX_CONV_COUNT; n++) {
            if (tags[i].aliasList[n].aliases!=NULL) {
                uprv_free(tags[i].aliasList[n].aliases);
            }
        }
    }

    return 0;
}

static void
parseFile(FileStream *in) {
    char line[MAX_LINE_SIZE];
    char lastLine[MAX_LINE_SIZE];
    int32_t lineSize = 0;
    int32_t lastLineSize = 0;
    UBool validParse = TRUE;

    lineNum = 0;

    /* Add the empty tag, which is for untagged aliases */
    getTagNumber("", 0);
    getTagNumber(ALL_TAG_STR, 3);
    allocString(&stringBlock, "", 0);

    /* read the list of aliases */
    while (validParse) {
        validParse = FALSE;

        /* Read non-empty lines that don't start with a space character. */
        while (T_FileStream_readLine(in, lastLine, MAX_LINE_SIZE) != NULL) {
            lastLineSize = chomp(lastLine);
            if (lineSize == 0 || (lastLineSize > 0 && isspace((int)*lastLine))) {
                uprv_strcpy(line + lineSize, lastLine);
                lineSize += lastLineSize;
            } else if (lineSize > 0) {
                validParse = TRUE;
                break;
            }
            lineNum++;
        }

        if (validParse || lineSize > 0) {
            if (isspace((int)*line)) {
                fprintf(stderr, "%s:%d: error: cannot start an alias with a space\n", path, lineNum-1);
                exit(U_PARSE_ERROR);
            } else if (line[0] == '{') {
                if (!standardTagsUsed && line[lineSize - 1] != '}') {
                    fprintf(stderr, "%s:%d: error: alias needs to start with a converter name\n", path, lineNum);
                    exit(U_PARSE_ERROR);
                }
                addOfficialTaggedStandards(line, lineSize);
                standardTagsUsed = TRUE;
            } else {
                if (standardTagsUsed) {
                    parseLine(line);
                }
                else {
                    fprintf(stderr, "%s:%d: error: alias table needs to start a list of standard tags\n", path, lineNum);
                    exit(U_PARSE_ERROR);
                }
            }
            /* Was the last line consumed */
            if (lastLineSize > 0) {
                uprv_strcpy(line, lastLine);
                lineSize = lastLineSize;
            }
            else {
                lineSize = 0;
            }
        }
        lineNum++;
    }
}

/* This works almost like the Perl chomp.
 It removes the newlines, comments and trailing whitespace (not preceding whitespace).
*/
static int32_t
chomp(char *line) {
    char *s = line;
    char *lastNonSpace = line;
    while(*s!=0) {
        /* truncate at a newline or a comment */
        if(*s == '\r' || *s == '\n' || *s == '#') {
            *s = 0;
            break;
        }
        if (!isspace((int)*s)) {
            lastNonSpace = s;
        }
        ++s;
    }
    if (lastNonSpace++ > line) {
        *lastNonSpace = 0;
        s = lastNonSpace;
    }
    return (int32_t)(s - line);
}

static void
parseLine(const char *line) {
    uint16_t pos=0, start, limit, length, cnv;
    char *converter, *alias;

    /* skip leading white space */
    /* There is no whitespace at the beginning anymore */
/*    while(line[pos]!=0 && isspace(line[pos])) {
        ++pos;
    }
*/

    /* is there nothing on this line? */
    if(line[pos]==0) {
        return;
    }

    /* get the converter name */
    start=pos;
    while(line[pos]!=0 && !isspace((int)line[pos])) {
        ++pos;
    }
    limit=pos;

    /* store the converter name */
    length=(uint16_t)(limit-start);
    converter=allocString(&stringBlock, line+start, length);

    /* add the converter to the converter table */
    cnv=addConverter(converter);

    /* The name itself may be tagged, so let's added it to the aliases list properly */
    pos = start;

    /* get all the real aliases */
    for(;;) {

        /* skip white space */
        while(line[pos]!=0 && isspace((int)line[pos])) {
            ++pos;
        }

        /* is there no more alias name on this line? */
        if(line[pos]==0) {
            break;
        }

        /* get an alias name */
        start=pos;
        while(line[pos]!=0 && line[pos]!='{' && !isspace((int)line[pos])) {
            ++pos;
        }
        limit=pos;

        /* store the alias name */
        length=(uint16_t)(limit-start);
        if (start == 0) {
            /* add the converter as its own alias to the alias table */
            alias = converter;
            addAlias(alias, ALL_TAG_NUM, cnv, TRUE);
        }
        else {
            alias=allocString(&stringBlock, line+start, length);
            addAlias(alias, ALL_TAG_NUM, cnv, FALSE);
        }
        addToKnownAliases(alias);

        /* add the alias/converter pair to the alias table */
        /* addAlias(alias, 0, cnv, FALSE);*/

        /* skip whitespace */
        while (line[pos] && isspace((int)line[pos])) {
            ++pos;
        }

        /* handle tags if they are present */
        if (line[pos] == '{') {
            ++pos;
            do {
                start = pos;
                while (line[pos] && line[pos] != '}' && !isspace((int)line[pos])) {
                    ++pos;
                }
                limit = pos;

                if (start != limit) {
                    /* add the tag to the tag table */
                    uint16_t tag = getTagNumber(line + start, (uint16_t)(limit - start));
                    addAlias(alias, tag, cnv, (UBool)(line[limit-1] == '*'));
                }

                while (line[pos] && isspace((int)line[pos])) {
                    ++pos;
                }
            } while (line[pos] && line[pos] != '}');

            if (line[pos] == '}') {
                ++pos;
            } else {
                fprintf(stderr, "%s:%d: Unterminated tag list\n", path, lineNum);
                exit(U_UNMATCHED_BRACES);
            }
        } else {
            addAlias(alias, EMPTY_TAG_NUM, cnv, (UBool)(tags[0].aliasList[cnv].aliasCount == 0));
        }
    }
}

static uint16_t
getTagNumber(const char *tag, uint16_t tagLen) {
    char *atag;
    uint16_t t;
    UBool preferredName = ((tagLen > 0) ? (tag[tagLen - 1] == '*') : (FALSE));

    if (tagCount >= MAX_TAG_COUNT) {
        fprintf(stderr, "%s:%d: too many tags\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }

    if (preferredName) {
/*        puts(tag);*/
        tagLen--;
    }

    for (t = 0; t < tagCount; ++t) {
        const char *currTag = GET_TAG_STR(tags[t].tag);
        if (uprv_strlen(currTag) == tagLen && !uprv_strnicmp(currTag, tag, tagLen)) {
            return t;
        }
    }

    /* we need to add this tag */
    if (tagCount >= MAX_TAG_COUNT) {
        fprintf(stderr, "%s:%d: error: too many tags\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }

    /* allocate a new entry in the tag table */
    atag = allocString(&tagBlock, tag, tagLen);

    if (standardTagsUsed) {
        fprintf(stderr, "%s:%d: error: Tag \"%s\" is not declared at the beginning of the alias table.\n",
            path, lineNum, atag);
        exit(1);
    }
    else if (tagLen > 0 && strcmp(tag, ALL_TAG_STR) != 0) {
        fprintf(stderr, "%s:%d: warning: Tag \"%s\" was added to the list of standards because it was not declared at beginning of the alias table.\n",
            path, lineNum, atag);
    }

    /* add the tag to the tag table */
    tags[tagCount].tag = GET_TAG_NUM(atag);
    /* The aliasList should be set to 0's already */

    return tagCount++;
}

/*static void
addTaggedAlias(uint16_t tag, const char *alias, uint16_t converter) {
    tags[tag].aliases[converter] = alias;
}
*/

static void
addOfficialTaggedStandards(char *line, int32_t lineLen) {
    (void) lineLen; // suppress compiler warnings about unused variable
    char *atag;
    char *endTagExp;
    char *tag;
    static const char WHITESPACE[] = " \t";

    if (tagCount > UCNV_NUM_RESERVED_TAGS) {
        fprintf(stderr, "%s:%d: error: official tags already added\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }
    tag = strchr(line, '{');
    if (tag == NULL) {
        /* Why were we called? */
        fprintf(stderr, "%s:%d: error: Missing start of tag group\n", path, lineNum);
        exit(U_PARSE_ERROR);
    }
    tag++;
    endTagExp = strchr(tag, '}');
    if (endTagExp == NULL) {
        fprintf(stderr, "%s:%d: error: Missing end of tag group\n", path, lineNum);
        exit(U_PARSE_ERROR);
    }
    endTagExp[0] = 0;

    tag = strtok(tag, WHITESPACE);
    while (tag != NULL) {
/*        printf("Adding original tag \"%s\"\n", tag);*/

        /* allocate a new entry in the tag table */
        atag = allocString(&tagBlock, tag, -1);

        /* add the tag to the tag table */
        tags[tagCount++].tag = (uint16_t)((atag - tagStore) >> 1);

        /* The aliasList should already be set to 0's */

        /* Get next tag */
        tag = strtok(NULL, WHITESPACE);
    }
}

static uint16_t
addToKnownAliases(const char *alias) {
/*    uint32_t idx; */
    /* strict matching */
/*    for (idx = 0; idx < knownAliasesCount; idx++) {
        uint16_t num = GET_ALIAS_NUM(alias);
        if (knownAliases[idx] != num
            && uprv_strcmp(alias, GET_ALIAS_STR(knownAliases[idx])) == 0)
        {
            fprintf(stderr, "%s:%d: warning: duplicate alias %s and %s found\n", path, 
                lineNum, alias, GET_ALIAS_STR(knownAliases[idx]));
            duplicateKnownAliasesCount++;
            break;
        }
        else if (knownAliases[idx] != num
            && ucnv_compareNames(alias, GET_ALIAS_STR(knownAliases[idx])) == 0)
        {
            if (verbose) {
                fprintf(stderr, "%s:%d: information: duplicate alias %s and %s found\n", path, 
                    lineNum, alias, GET_ALIAS_STR(knownAliases[idx]));
            }
            duplicateKnownAliasesCount++;
            break;
        }
    }
*/
    if (knownAliasesCount >= MAX_ALIAS_COUNT) {
        fprintf(stderr, "%s:%d: warning: Too many aliases defined for all converters\n",
            path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }
    /* TODO: We could try to unlist exact duplicates. */
    return knownAliases[knownAliasesCount++] = GET_ALIAS_NUM(alias);
}

/*
@param standard When standard is 0, then it's the "empty" tag.
*/
static uint16_t
addAlias(const char *alias, uint16_t standard, uint16_t converter, UBool defaultName) {
    uint32_t idx, idx2;
    UBool startEmptyWithoutDefault = FALSE;
    AliasList *aliasList;

    if(standard>=MAX_TAG_COUNT) {
        fprintf(stderr, "%s:%d: error: too many standard tags\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }
    if(converter>=MAX_CONV_COUNT) {
        fprintf(stderr, "%s:%d: error: too many converter names\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }
    aliasList = &tags[standard].aliasList[converter];

    if (strchr(alias, '}')) {
        fprintf(stderr, "%s:%d: error: unmatched } found\n", path, 
            lineNum);
    }

    if(aliasList->aliasCount + 1 >= MAX_TC_ALIAS_COUNT) {
        fprintf(stderr, "%s:%d: error: too many aliases for alias %s and converter %s\n", path, 
            lineNum, alias, GET_ALIAS_STR(converters[converter].converter));
        exit(U_BUFFER_OVERFLOW_ERROR);
    }

    /* Show this warning only once. All aliases are added to the "ALL" tag. */
    if (standard == ALL_TAG_NUM && GET_ALIAS_STR(converters[converter].converter) != alias) {
        /* Normally these option values are parsed at runtime, and they can
           be discarded when the alias is a default converter. Options should
           only be on a converter and not an alias. */
        if (uprv_strchr(alias, UCNV_OPTION_SEP_CHAR) != 0)
        {
            fprintf(stderr, "warning(line %d): alias %s contains a \""UCNV_OPTION_SEP_STRING"\". Options are parsed at run-time and do not need to be in the alias table.\n",
                lineNum, alias);
        }
        if (uprv_strchr(alias, UCNV_VALUE_SEP_CHAR) != 0)
        {
            fprintf(stderr, "warning(line %d): alias %s contains an \""UCNV_VALUE_SEP_STRING"\". Options are parsed at run-time and do not need to be in the alias table.\n",
                lineNum, alias);
        }
    }

    if (standard != ALL_TAG_NUM) {
        /* Check for duplicate aliases for this tag on all converters */
        for (idx = 0; idx < converterCount; idx++) {
            for (idx2 = 0; idx2 < tags[standard].aliasList[idx].aliasCount; idx2++) {
                uint16_t aliasNum = tags[standard].aliasList[idx].aliases[idx2];
                if (aliasNum
                    && ucnv_compareNames(alias, GET_ALIAS_STR(aliasNum)) == 0)
                {
                    if (idx == converter) {
                        /*
                         * (alias, standard) duplicates are harmless if they map to the same converter.
                         * Only print a warning in verbose mode, or if the alias is a precise duplicate,
                         * not just a lenient-match duplicate.
                         */
                        if (verbose || 0 == uprv_strcmp(alias, GET_ALIAS_STR(aliasNum))) {
                            fprintf(stderr, "%s:%d: warning: duplicate aliases %s and %s found for standard %s and converter %s\n", path, 
                                lineNum, alias, GET_ALIAS_STR(aliasNum),
                                GET_TAG_STR(tags[standard].tag),
                                GET_ALIAS_STR(converters[converter].converter));
                        }
                    } else {
                        fprintf(stderr, "%s:%d: warning: duplicate aliases %s and %s found for standard tag %s between converter %s and converter %s\n", path, 
                            lineNum, alias, GET_ALIAS_STR(aliasNum),
                            GET_TAG_STR(tags[standard].tag),
                            GET_ALIAS_STR(converters[converter].converter),
                            GET_ALIAS_STR(converters[idx].converter));
                    }
                    break;
                }
            }
        }

        /* Check for duplicate default aliases for this converter on all tags */
        /* It's okay to have multiple standards prefer the same name */
/*        if (verbose && !dupFound) {
            for (idx = 0; idx < tagCount; idx++) {
                if (tags[idx].aliasList[converter].aliases) {
                    uint16_t aliasNum = tags[idx].aliasList[converter].aliases[0];
                    if (aliasNum
                        && ucnv_compareNames(alias, GET_ALIAS_STR(aliasNum)) == 0)
                    {
                        fprintf(stderr, "%s:%d: warning: duplicate alias %s found for converter %s and standard tag %s\n", path, 
                            lineNum, alias, GET_ALIAS_STR(converters[converter].converter), GET_TAG_STR(tags[standard].tag));
                        break;
                    }
                }
            }
        }*/
    }

    if (aliasList->aliasCount <= 0) {
        aliasList->aliasCount++;
        startEmptyWithoutDefault = TRUE;
    }
    aliasList->aliases = (uint16_t *)uprv_realloc(aliasList->aliases, (aliasList->aliasCount + 1) * sizeof(aliasList->aliases[0]));
    if (startEmptyWithoutDefault) {
        aliasList->aliases[0] = 0;
    }
    if (defaultName) {
        if (aliasList->aliases[0] != 0) {
            fprintf(stderr, "%s:%d: error: Alias %s and %s cannot both be the default alias for standard tag %s and converter %s\n", path, 
                lineNum,
                alias,
                GET_ALIAS_STR(aliasList->aliases[0]),
                GET_TAG_STR(tags[standard].tag),
                GET_ALIAS_STR(converters[converter].converter));
            exit(U_PARSE_ERROR);
        }
        aliasList->aliases[0] = GET_ALIAS_NUM(alias);
    } else {
        aliasList->aliases[aliasList->aliasCount++] = GET_ALIAS_NUM(alias);
    }
/*    aliasList->converter = converter;*/

    converters[converter].totalAliasCount++; /* One more to the column */
    tags[standard].totalAliasCount++; /* One more to the row */

    return aliasList->aliasCount;
}

static uint16_t
addConverter(const char *converter) {
    uint32_t idx;
    if(converterCount>=MAX_CONV_COUNT) {
        fprintf(stderr, "%s:%d: error: too many converters\n", path, lineNum);
        exit(U_BUFFER_OVERFLOW_ERROR);
    }

    for (idx = 0; idx < converterCount; idx++) {
        if (ucnv_compareNames(converter, GET_ALIAS_STR(converters[idx].converter)) == 0) {
            fprintf(stderr, "%s:%d: error: duplicate converter %s found!\n", path, lineNum, converter);
            exit(U_PARSE_ERROR);
            break;
        }
    }

    converters[converterCount].converter = GET_ALIAS_NUM(converter);
    converters[converterCount].totalAliasCount = 0;

    return converterCount++;
}

/* resolve this alias based on the prioritization of the standard tags. */
static void
resolveAliasToConverter(uint16_t alias, uint16_t *tagNum, uint16_t *converterNum) {
    uint16_t idx, idx2, idx3;

    for (idx = UCNV_NUM_RESERVED_TAGS; idx < tagCount; idx++) {
        for (idx2 = 0; idx2 < converterCount; idx2++) {
            for (idx3 = 0; idx3 < tags[idx].aliasList[idx2].aliasCount; idx3++) {
                uint16_t aliasNum = tags[idx].aliasList[idx2].aliases[idx3];
                if (aliasNum == alias) {
                    *tagNum = idx;
                    *converterNum = idx2;
                    return;
                }
            }
        }
    }
    /* Do the leftovers last, just in case */
    /* There is no need to do the ALL tag */
    idx = 0;
    for (idx2 = 0; idx2 < converterCount; idx2++) {
        for (idx3 = 0; idx3 < tags[idx].aliasList[idx2].aliasCount; idx3++) {
            uint16_t aliasNum = tags[idx].aliasList[idx2].aliases[idx3];
            if (aliasNum == alias) {
                *tagNum = idx;
                *converterNum = idx2;
                return;
            }
        }
    }
    *tagNum = UINT16_MAX;
    *converterNum = UINT16_MAX;
    fprintf(stderr, "%s: warning: alias %s not found\n",
        path,
        GET_ALIAS_STR(alias));
    return;
}

/* The knownAliases should be sorted before calling this function */
static uint32_t
resolveAliases(uint16_t *uniqueAliasArr, uint16_t *uniqueAliasToConverterArr, uint16_t aliasOffset) {
    uint32_t uniqueAliasIdx = 0;
    uint32_t idx;
    uint16_t currTagNum, oldTagNum;
    uint16_t currConvNum, oldConvNum;
    const char *lastName;

    if (knownAliasesCount != 0) {
      resolveAliasToConverter(knownAliases[0], &oldTagNum, &currConvNum);
      uniqueAliasToConverterArr[uniqueAliasIdx] = currConvNum;
      oldConvNum = currConvNum;
      uniqueAliasArr[uniqueAliasIdx] = knownAliases[0] + aliasOffset;
      uniqueAliasIdx++;
      lastName = GET_ALIAS_STR(knownAliases[0]);

      for (idx = 1; idx < knownAliasesCount; idx++) {
          resolveAliasToConverter(knownAliases[idx], &currTagNum, &currConvNum);
          if (ucnv_compareNames(lastName, GET_ALIAS_STR(knownAliases[idx])) == 0) {
              /* duplicate found */
              if ((currTagNum < oldTagNum && currTagNum >= UCNV_NUM_RESERVED_TAGS)
                  || oldTagNum == 0) {
                  oldTagNum = currTagNum;
                  uniqueAliasToConverterArr[uniqueAliasIdx - 1] = currConvNum;
                  uniqueAliasArr[uniqueAliasIdx - 1] = knownAliases[idx] + aliasOffset;
                  if (verbose) {
                      printf("using %s instead of %s -> %s",
                          GET_ALIAS_STR(knownAliases[idx]),
                          lastName,
                          GET_ALIAS_STR(converters[currConvNum].converter));
                      if (oldConvNum != currConvNum) {
                          printf(" (alias conflict)");
                      }
                      puts("");
                  }
              }
              else {
                  /* else ignore it */
                  if (verbose) {
                      printf("folding %s into %s -> %s",
                          GET_ALIAS_STR(knownAliases[idx]),
                          lastName,
                          GET_ALIAS_STR(converters[oldConvNum].converter));
                      if (oldConvNum != currConvNum) {
                          printf(" (alias conflict)");
                      }
                      puts("");
                  }
              }
              if (oldConvNum != currConvNum) {
                  uniqueAliasToConverterArr[uniqueAliasIdx - 1] |= UCNV_AMBIGUOUS_ALIAS_MAP_BIT;
              }
          }
          else {
              uniqueAliasToConverterArr[uniqueAliasIdx] = currConvNum;
              oldConvNum = currConvNum;
              uniqueAliasArr[uniqueAliasIdx] = knownAliases[idx] + aliasOffset;
              uniqueAliasIdx++;
              lastName = GET_ALIAS_STR(knownAliases[idx]);
              oldTagNum = currTagNum;
              /*printf("%s -> %s\n", GET_ALIAS_STR(knownAliases[idx]), GET_ALIAS_STR(converters[currConvNum].converter));*/
          }
          if (uprv_strchr(GET_ALIAS_STR(converters[currConvNum].converter), UCNV_OPTION_SEP_CHAR) != NULL) {
              uniqueAliasToConverterArr[uniqueAliasIdx-1] |= UCNV_CONTAINS_OPTION_BIT;
          }
      }
    }
    return uniqueAliasIdx;
}

static void
createOneAliasList(uint16_t *aliasArrLists, uint32_t tag, uint32_t converter, uint16_t offset) {
    uint32_t aliasNum;
    AliasList *aliasList = &tags[tag].aliasList[converter];

    if (aliasList->aliasCount == 0) {
        aliasArrLists[tag*converterCount + converter] = 0;
    }
    else {
        aliasLists[aliasListsSize++] = aliasList->aliasCount;

        /* write into the array area a 1's based index. */
        aliasArrLists[tag*converterCount + converter] = aliasListsSize;

/*        printf("tag %s converter %s\n",
            GET_TAG_STR(tags[tag].tag),
            GET_ALIAS_STR(converters[converter].converter));*/
        for (aliasNum = 0; aliasNum < aliasList->aliasCount; aliasNum++) {
            uint16_t value;
/*            printf("   %s\n",
                GET_ALIAS_STR(aliasList->aliases[aliasNum]));*/
            if (aliasList->aliases[aliasNum]) {
                value = aliasList->aliases[aliasNum] + offset;
            } else {
                value = 0;
                if (tag != 0 && !quiet) { /* Only show the warning when it's not the leftover tag. */
                    fprintf(stderr, "%s: warning: tag %s does not have a default alias for %s\n",
                            path,
                            GET_TAG_STR(tags[tag].tag),
                            GET_ALIAS_STR(converters[converter].converter));
                }
            }
            aliasLists[aliasListsSize++] = value;
            if (aliasListsSize >= MAX_LIST_SIZE) {
                fprintf(stderr, "%s: error: Too many alias lists\n", path);
                exit(U_BUFFER_OVERFLOW_ERROR);
            }

        }
    }
}

static void
createNormalizedAliasStrings(char *normalizedStrings, const char *origStringBlock, int32_t stringBlockLength) {
    int32_t currStrLen;
    uprv_memcpy(normalizedStrings, origStringBlock, stringBlockLength);
    while ((currStrLen = (int32_t)uprv_strlen(origStringBlock)) < stringBlockLength) {
        int32_t currStrSize = currStrLen + 1;
        if (currStrLen > 0) {
            int32_t normStrLen;
            ucnv_io_stripForCompare(normalizedStrings, origStringBlock);
            normStrLen = (int32_t)uprv_strlen(normalizedStrings);
            if (normStrLen > 0) {
                uprv_memset(normalizedStrings + normStrLen, 0, currStrSize - normStrLen);
            }
        }
        stringBlockLength -= currStrSize;
        normalizedStrings += currStrSize;
        origStringBlock += currStrSize;
    }
}

static void
writeAliasTable(UNewDataMemory *out) {
    uint32_t i, j;
    uint32_t uniqueAliasesSize;
    uint16_t aliasOffset = (uint16_t)(tagBlock.top/sizeof(uint16_t));
    uint16_t *aliasArrLists = (uint16_t *)uprv_malloc(tagCount * converterCount * sizeof(uint16_t));
    uint16_t *uniqueAliases = (uint16_t *)uprv_malloc(knownAliasesCount * sizeof(uint16_t));
    uint16_t *uniqueAliasesToConverter = (uint16_t *)uprv_malloc(knownAliasesCount * sizeof(uint16_t));

    qsort(knownAliases, knownAliasesCount, sizeof(knownAliases[0]), compareAliases);
    uniqueAliasesSize = resolveAliases(uniqueAliases, uniqueAliasesToConverter, aliasOffset);

    /* Array index starts at 1. aliasLists[0] is the size of the lists section. */
    aliasListsSize = 0;

    /* write the offsets of all the aliases lists in a 2D array, and create the lists. */
    for (i = 0; i < tagCount; ++i) {
        for (j = 0; j < converterCount; ++j) {
            createOneAliasList(aliasArrLists, i, j, aliasOffset);
        }
    }

    /* Write the size of the TOC */
    if (tableOptions.stringNormalizationType == UCNV_IO_UNNORMALIZED) {
        udata_write32(out, 8);
    }
    else {
        udata_write32(out, 9);
    }

    /* Write the sizes of each section */
    /* All sizes are the number of uint16_t units, not bytes */
    udata_write32(out, converterCount);
    udata_write32(out, tagCount);
    udata_write32(out, uniqueAliasesSize);  /* list of aliases */
    udata_write32(out, uniqueAliasesSize);  /* The preresolved form of mapping an untagged the alias to a converter */
    udata_write32(out, tagCount * converterCount);
    udata_write32(out, aliasListsSize + 1);
    udata_write32(out, sizeof(tableOptions) / sizeof(uint16_t));
    udata_write32(out, (tagBlock.top + stringBlock.top) / sizeof(uint16_t));
    if (tableOptions.stringNormalizationType != UCNV_IO_UNNORMALIZED) {
        udata_write32(out, (tagBlock.top + stringBlock.top) / sizeof(uint16_t));
    }

    /* write the table of converters */
    /* Think of this as the column headers */
    for(i=0; i<converterCount; ++i) {
        udata_write16(out, (uint16_t)(converters[i].converter + aliasOffset));
    }

    /* write the table of tags */
    /* Think of this as the row headers */
    for(i=UCNV_NUM_RESERVED_TAGS; i<tagCount; ++i) {
        udata_write16(out, tags[i].tag);
    }
    /* The empty tag is considered the leftover list, and put that at the end of the priority list. */
    udata_write16(out, tags[EMPTY_TAG_NUM].tag);
    udata_write16(out, tags[ALL_TAG_NUM].tag);

    /* Write the unique list of aliases */
    udata_writeBlock(out, uniqueAliases, uniqueAliasesSize * sizeof(uint16_t));

    /* Write the unique list of aliases */
    udata_writeBlock(out, uniqueAliasesToConverter, uniqueAliasesSize * sizeof(uint16_t));

    /* Write the array to the lists */
    udata_writeBlock(out, (const void *)(aliasArrLists + (2*converterCount)), (((tagCount - 2) * converterCount) * sizeof(uint16_t)));
    /* Now write the leftover part of the array for the EMPTY and ALL lists */
    udata_writeBlock(out, (const void *)aliasArrLists, (2 * converterCount * sizeof(uint16_t)));

    /* Offset the next array to make the index start at 1. */
    udata_write16(out, 0xDEAD);

    /* Write the lists */
    udata_writeBlock(out, (const void *)aliasLists, aliasListsSize * sizeof(uint16_t));

    /* Write any options for the alias table. */
    udata_writeBlock(out, (const void *)&tableOptions, sizeof(tableOptions));

    /* write the tags strings */
    udata_writeString(out, tagBlock.store, tagBlock.top);

    /* write the aliases strings */
    udata_writeString(out, stringBlock.store, stringBlock.top);

    /* write the normalized aliases strings */
    if (tableOptions.stringNormalizationType != UCNV_IO_UNNORMALIZED) {
        char *normalizedStrings = (char *)uprv_malloc(tagBlock.top + stringBlock.top);
        createNormalizedAliasStrings(normalizedStrings, tagBlock.store, tagBlock.top);
        createNormalizedAliasStrings(normalizedStrings + tagBlock.top, stringBlock.store, stringBlock.top);

        /* Write out the complete normalized array. */
        udata_writeString(out, normalizedStrings, tagBlock.top + stringBlock.top);
        uprv_free(normalizedStrings);
    }

    uprv_free(uniqueAliasesToConverter);
    uprv_free(uniqueAliases);
    uprv_free(aliasArrLists);
}

static char *
allocString(StringBlock *block, const char *s, int32_t length) {
    uint32_t top;
    char *p;

    if(length<0) {
        length=(int32_t)uprv_strlen(s);
    }

    /*
     * add 1 for the terminating NUL
     * and round up (+1 &~1)
     * to keep the addresses on a 16-bit boundary
     */
    top=block->top + (uint32_t)((length + 1 + 1) & ~1);

    if(top >= block->max) {
        fprintf(stderr, "%s:%d: error: out of memory\n", path, lineNum);
        exit(U_MEMORY_ALLOCATION_ERROR);
    }

    /* get the pointer and copy the string */
    p = block->store + block->top;
    uprv_memcpy(p, s, length);
    p[length] = 0; /* NUL-terminate it */
    if((length & 1) == 0) {
        p[length + 1] = 0; /* set the padding byte */
    }

    /* check for invariant characters now that we have a NUL-terminated string for easy output */
    if(!uprv_isInvariantString(p, length)) {
        fprintf(stderr, "%s:%d: error: the name %s contains not just invariant characters\n", path, lineNum, p);
        exit(U_INVALID_TABLE_FORMAT);
    }

    block->top = top;
    return p;
}

static int
compareAliases(const void *alias1, const void *alias2) {
    /* Names like IBM850 and ibm-850 need to be sorted together */
    int result = ucnv_compareNames(GET_ALIAS_STR(*(uint16_t*)alias1), GET_ALIAS_STR(*(uint16_t*)alias2));
    if (!result) {
        /* Sort the shortest first */
        return (int)uprv_strlen(GET_ALIAS_STR(*(uint16_t*)alias1)) - (int)uprv_strlen(GET_ALIAS_STR(*(uint16_t*)alias2));
    }
    return result;
}

/*
 * Hey, Emacs, please set the following:
 *
 * Local Variables:
 * indent-tabs-mode: nil
 * End:
 *
 */

