// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
 ********************************************************************************
 *
 *   Copyright (C) 1998-2015, International Business Machines
 *   Corporation and others.  All Rights Reserved.
 *
 ********************************************************************************
 *
 *
 *  makeconv.cpp:
 *  tool creating a binary (compressed) representation of the conversion mapping
 *  table (IBM NLTC ucmap format).
 *
 *  05/04/2000    helena     Added fallback mapping into the picture...
 *  06/29/2000  helena      Major rewrite of the callback APIs.
 */

#include <stdio.h>
#include "unicode/putil.h"
#include "unicode/ucnv_err.h"
#include "charstr.h"
#include "ucnv_bld.h"
#include "ucnv_imp.h"
#include "ucnv_cnv.h"
#include "cstring.h"
#include "cmemory.h"
#include "uinvchar.h"
#include "filestrm.h"
#include "toolutil.h"
#include "uoptions.h"
#include "unicode/udata.h"
#include "unewdata.h"
#include "uparse.h"
#include "ucm.h"
#include "makeconv.h"
#include "genmbcs.h"

#define DEBUG 0

typedef struct ConvData {
    UCMFile *ucm;
    NewConverter *cnvData, *extData;
    UConverterSharedData sharedData;
    UConverterStaticData staticData;
} ConvData;

static void
initConvData(ConvData *data) {
    uprv_memset(data, 0, sizeof(ConvData));
    data->sharedData.structSize=sizeof(UConverterSharedData);
    data->staticData.structSize=sizeof(UConverterStaticData);
    data->sharedData.staticData=&data->staticData;
}

static void
cleanupConvData(ConvData *data) {
    if(data!=NULL) {
        if(data->cnvData!=NULL) {
            data->cnvData->close(data->cnvData);
            data->cnvData=NULL;
        }
        if(data->extData!=NULL) {
            data->extData->close(data->extData);
            data->extData=NULL;
        }
        ucm_close(data->ucm);
        data->ucm=NULL;
    }
}

/*
 * from ucnvstat.c - static prototypes of data-based converters
 */
U_CAPI const UConverterStaticData * ucnv_converterStaticData[UCNV_NUMBER_OF_SUPPORTED_CONVERTER_TYPES];

/*
 * Global - verbosity
 */
UBool VERBOSE = FALSE;
UBool QUIET = FALSE;
UBool SMALL = FALSE;
UBool IGNORE_SISO_CHECK = FALSE;

static void
createConverter(ConvData *data, const char* converterName, UErrorCode *pErrorCode);

/*
 * Set up the UNewData and write the converter..
 */
static void
writeConverterData(ConvData *data, const char *cnvName, const char *cnvDir, UErrorCode *status);

UBool haveCopyright=TRUE;

static UDataInfo dataInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    sizeof(UChar),
    0,

    {0x63, 0x6e, 0x76, 0x74},     /* dataFormat="cnvt" */
    {6, 2, 0, 0},                 /* formatVersion */
    {0, 0, 0, 0}                  /* dataVersion (calculated at runtime) */
};

static void
writeConverterData(ConvData *data, const char *cnvName, const char *cnvDir, UErrorCode *status)
{
    UNewDataMemory *mem = NULL;
    uint32_t sz2;
    uint32_t size = 0;
    int32_t tableType;

    if(U_FAILURE(*status))
      {
        return;
      }

    tableType=TABLE_NONE;
    if(data->cnvData!=NULL) {
        tableType|=TABLE_BASE;
    }
    if(data->extData!=NULL) {
        tableType|=TABLE_EXT;
    }

    mem = udata_create(cnvDir, "cnv", cnvName, &dataInfo, haveCopyright ? U_COPYRIGHT_STRING : NULL, status);

    if(U_FAILURE(*status))
      {
        fprintf(stderr, "Couldn't create the udata %s.%s: %s\n",
                cnvName,
                "cnv",
                u_errorName(*status));
        return;
      }

    if(VERBOSE)
      {
        printf("- Opened udata %s.%s\n", cnvName, "cnv");
      }


    /* all read only, clean, platform independent data.  Mmmm. :)  */
    udata_writeBlock(mem, &data->staticData, sizeof(UConverterStaticData));
    size += sizeof(UConverterStaticData); /* Is 4-aligned  - by size */
    /* Now, write the table */
    if(tableType&TABLE_BASE) {
        size += data->cnvData->write(data->cnvData, &data->staticData, mem, tableType);
    }
    if(tableType&TABLE_EXT) {
        size += data->extData->write(data->extData, &data->staticData, mem, tableType);
    }

    sz2 = udata_finish(mem, status);
    if(size != sz2)
    {
        fprintf(stderr, "error: wrote %u bytes to the .cnv file but counted %u bytes\n", (int)sz2, (int)size);
        *status=U_INTERNAL_PROGRAM_ERROR;
    }
    if(VERBOSE)
    {
      printf("- Wrote %u bytes to the udata.\n", (int)sz2);
    }
}

enum {
    OPT_HELP_H,
    OPT_HELP_QUESTION_MARK,
    OPT_COPYRIGHT,
    OPT_VERSION,
    OPT_DESTDIR,
    OPT_VERBOSE,
    OPT_SMALL,
    OPT_IGNORE_SISO_CHECK,
    OPT_QUIET,
    OPT_SOURCEDIR,

    OPT_COUNT
};

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_COPYRIGHT,
    UOPTION_VERSION,
    UOPTION_DESTDIR,
    UOPTION_VERBOSE,
    { "small", NULL, NULL, NULL, '\1', UOPT_NO_ARG, 0 },
    { "ignore-siso-check", NULL, NULL, NULL, '\1', UOPT_NO_ARG, 0 },
    UOPTION_QUIET,
    UOPTION_SOURCEDIR,
};

int main(int argc, char* argv[])
{
    ConvData data;
    char cnvName[UCNV_MAX_FULL_FILE_NAME_LENGTH];

    U_MAIN_INIT_ARGS(argc, argv);

    /* Set up the ICU version number */
    UVersionInfo icuVersion;
    u_getVersion(icuVersion);
    uprv_memcpy(&dataInfo.dataVersion, &icuVersion, sizeof(UVersionInfo));

    /* preset then read command line options */
    options[OPT_DESTDIR].value=u_getDataDirectory();
    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    } else if(argc<2) {
        argc=-1;
    }
    if(argc<0 || options[OPT_HELP_H].doesOccur || options[OPT_HELP_QUESTION_MARK].doesOccur) {
        FILE *stdfile=argc<0 ? stderr : stdout;
        fprintf(stdfile,
            "usage: %s [-options] files...\n"
            "\tread .ucm codepage mapping files and write .cnv files\n"
            "options:\n"
            "\t-h or -? or --help  this usage text\n"
            "\t-V or --version     show a version message\n"
            "\t-c or --copyright   include a copyright notice\n"
            "\t-d or --destdir     destination directory, followed by the path\n"
            "\t-v or --verbose     Turn on verbose output\n"
            "\t-q or --quiet       do not display warnings and progress\n"
            "\t-s or --sourcedir   source directory, followed by the path\n",
            argv[0]);
        fprintf(stdfile,
            "\t      --small       Generate smaller .cnv files. They will be\n"
            "\t                    significantly smaller but may not be compatible with\n"
            "\t                    older versions of ICU and will require heap memory\n"
            "\t                    allocation when loaded.\n"
            "\t      --ignore-siso-check         Use SI/SO other than 0xf/0xe.\n");
        return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }

    if(options[OPT_VERSION].doesOccur) {
        printf("makeconv version %u.%u, ICU tool to read .ucm codepage mapping files and write .cnv files\n",
               dataInfo.formatVersion[0], dataInfo.formatVersion[1]);
        printf("%s\n", U_COPYRIGHT_STRING);
        exit(0);
    }

    /* get the options values */
    haveCopyright = options[OPT_COPYRIGHT].doesOccur;
    const char *destdir = options[OPT_DESTDIR].value;
    VERBOSE = options[OPT_VERBOSE].doesOccur;
    QUIET = options[OPT_QUIET].doesOccur;
    SMALL = options[OPT_SMALL].doesOccur;

    if (options[OPT_IGNORE_SISO_CHECK].doesOccur) {
        IGNORE_SISO_CHECK = TRUE;
    }

    icu::CharString outFileName;
    UErrorCode err = U_ZERO_ERROR;
    if (destdir != NULL && *destdir != 0) {
        outFileName.append(destdir, err).ensureEndsWithFileSeparator(err);
        if (U_FAILURE(err)) {
            return err;
        }
    }
    int32_t outBasenameStart = outFileName.length();

#if DEBUG
    {
      int i;
      printf("makeconv: processing %d files...\n", argc - 1);
      for(i=1; i<argc; ++i) {
        printf("%s ", argv[i]);
      }
      printf("\n");
      fflush(stdout);
    }
#endif

    UBool printFilename = (UBool) (argc > 2 || VERBOSE);
    icu::CharString pathBuf;
    for (++argv; --argc; ++argv)
    {
        UErrorCode localError = U_ZERO_ERROR;
        const char *arg = getLongPathname(*argv);

        const char* sourcedir = options[OPT_SOURCEDIR].value;
        if (sourcedir != NULL && *sourcedir != 0 && uprv_strcmp(sourcedir, ".") != 0) {
            pathBuf.clear();
            pathBuf.appendPathPart(sourcedir, localError);
            pathBuf.appendPathPart(arg, localError);
            arg = pathBuf.data();
        }

        /*produces the right destination path for display*/
        outFileName.truncate(outBasenameStart);
        if (outBasenameStart != 0)
        {
            /* find the last file sepator */
            const char *basename = findBasename(arg);
            outFileName.append(basename, localError);
        }
        else
        {
            outFileName.append(arg, localError);
        }
        if (U_FAILURE(localError)) {
            return localError;
        }

        /*removes the extension if any is found*/
        int32_t lastDotIndex = outFileName.lastIndexOf('.');
        if (lastDotIndex >= outBasenameStart) {
            outFileName.truncate(lastDotIndex);
        }

        /* the basename without extension is the converter name */
        if ((outFileName.length() - outBasenameStart) >= UPRV_LENGTHOF(cnvName)) {
            fprintf(stderr, "converter name %s too long\n", outFileName.data() + outBasenameStart);
            return U_BUFFER_OVERFLOW_ERROR;
        }
        uprv_strcpy(cnvName, outFileName.data() + outBasenameStart);

        /*Adds the target extension*/
        outFileName.append(CONVERTER_FILE_EXTENSION, localError);
        if (U_FAILURE(localError)) {
            return localError;
        }

#if DEBUG
        printf("makeconv: processing %s  ...\n", arg);
        fflush(stdout);
#endif
        initConvData(&data);
        createConverter(&data, arg, &localError);

        if (U_FAILURE(localError))
        {
            /* if an error is found, print out an error msg and keep going */
            fprintf(stderr, "Error creating converter for \"%s\" file for \"%s\" (%s)\n",
                    outFileName.data(), arg, u_errorName(localError));
            if(U_SUCCESS(err)) {
                err = localError;
            }
        }
        else
        {
            /* Insure the static data name matches the  file name */
            /* Changed to ignore directory and only compare base name
             LDH 1/2/08*/
            char *p;
            p = strrchr(cnvName, U_FILE_SEP_CHAR); /* Find last file separator */

            if(p == NULL)            /* OK, try alternate */
            {
                p = strrchr(cnvName, U_FILE_ALT_SEP_CHAR);
                if(p == NULL)
                {
                    p=cnvName; /* If no separators, no problem */
                }
            }
            else
            {
                p++;   /* If found separator, don't include it in compare */
            }
            if(uprv_stricmp(p,data.staticData.name) && !QUIET)
            {
                fprintf(stderr, "Warning: %s%s claims to be '%s'\n",
                    cnvName,  CONVERTER_FILE_EXTENSION,
                    data.staticData.name);
            }

            uprv_strcpy((char*)data.staticData.name, cnvName);

            if(!uprv_isInvariantString((char*)data.staticData.name, -1)) {
                fprintf(stderr,
                    "Error: A converter name must contain only invariant characters.\n"
                    "%s is not a valid converter name.\n",
                    data.staticData.name);
                if(U_SUCCESS(err)) {
                    err = U_INVALID_TABLE_FORMAT;
                }
            }

            localError = U_ZERO_ERROR;
            writeConverterData(&data, cnvName, destdir, &localError);

            if(U_FAILURE(localError))
            {
                /* if an error is found, print out an error msg and keep going*/
                fprintf(stderr, "Error writing \"%s\" file for \"%s\" (%s)\n", outFileName.data(), arg,
                    u_errorName(localError));
                if(U_SUCCESS(err)) {
                    err = localError;
                }
            }
            else if (printFilename)
            {
                puts(outFileName.data() + outBasenameStart);
            }
        }
        fflush(stdout);
        fflush(stderr);

        cleanupConvData(&data);
    }

    return err;
}

static void
getPlatformAndCCSIDFromName(const char *name, int8_t *pPlatform, int32_t *pCCSID) {
    if( (name[0]=='i' || name[0]=='I') &&
        (name[1]=='b' || name[1]=='B') &&
        (name[2]=='m' || name[2]=='M')
    ) {
        name+=3;
        if(*name=='-') {
            ++name;
        }
        *pPlatform=UCNV_IBM;
        *pCCSID=(int32_t)uprv_strtoul(name, NULL, 10);
    } else {
        *pPlatform=UCNV_UNKNOWN;
        *pCCSID=0;
    }
}

static void
readHeader(ConvData *data,
           FileStream* convFile,
           UErrorCode *pErrorCode) {
    char line[1024];
    char *s, *key, *value;
    const UConverterStaticData *prototype;
    UConverterStaticData *staticData;

    if(U_FAILURE(*pErrorCode)) {
        return;
    }

    staticData=&data->staticData;
    staticData->platform=UCNV_IBM;
    staticData->subCharLen=0;

    while(T_FileStream_readLine(convFile, line, sizeof(line))) {
        /* basic parsing and handling of state-related items */
        if(ucm_parseHeaderLine(data->ucm, line, &key, &value)) {
            continue;
        }

        /* stop at the beginning of the mapping section */
        if(uprv_strcmp(line, "CHARMAP")==0) {
            break;
        }

        /* collect the information from the header field, ignore unknown keys */
        if(uprv_strcmp(key, "code_set_name")==0) {
            if(*value!=0) {
                uprv_strcpy((char *)staticData->name, value);
                getPlatformAndCCSIDFromName(value, &staticData->platform, &staticData->codepage);
            }
        } else if(uprv_strcmp(key, "subchar")==0) {
            uint8_t bytes[UCNV_EXT_MAX_BYTES];
            int8_t length;

            s=value;
            length=ucm_parseBytes(bytes, line, (const char **)&s);
            if(1<=length && length<=4 && *s==0) {
                staticData->subCharLen=length;
                uprv_memcpy(staticData->subChar, bytes, length);
            } else {
                fprintf(stderr, "error: illegal <subchar> %s\n", value);
                *pErrorCode=U_INVALID_TABLE_FORMAT;
                return;
            }
        } else if(uprv_strcmp(key, "subchar1")==0) {
            uint8_t bytes[UCNV_EXT_MAX_BYTES];

            s=value;
            if(1==ucm_parseBytes(bytes, line, (const char **)&s) && *s==0) {
                staticData->subChar1=bytes[0];
            } else {
                fprintf(stderr, "error: illegal <subchar1> %s\n", value);
                *pErrorCode=U_INVALID_TABLE_FORMAT;
                return;
            }
        }
    }

    /* copy values from the UCMFile to the static data */
    staticData->maxBytesPerChar=(int8_t)data->ucm->states.maxCharLength;
    staticData->minBytesPerChar=(int8_t)data->ucm->states.minCharLength;
    staticData->conversionType=data->ucm->states.conversionType;

    if(staticData->conversionType==UCNV_UNSUPPORTED_CONVERTER) {
        fprintf(stderr, "ucm error: missing conversion type (<uconv_class>)\n");
        *pErrorCode=U_INVALID_TABLE_FORMAT;
        return;
    }

    /*
     * Now that we know the type, copy any 'default' values from the table.
     * We need not check the type any further because the parser only
     * recognizes what we have prototypes for.
     *
     * For delta (extension-only) tables, copy values from the base file
     * instead, see createConverter().
     */
    if(data->ucm->baseName[0]==0) {
        prototype=ucnv_converterStaticData[staticData->conversionType];
        if(prototype!=NULL) {
            if(staticData->name[0]==0) {
                uprv_strcpy((char *)staticData->name, prototype->name);
            }

            if(staticData->codepage==0) {
                staticData->codepage=prototype->codepage;
            }

            if(staticData->platform==0) {
                staticData->platform=prototype->platform;
            }

            if(staticData->minBytesPerChar==0) {
                staticData->minBytesPerChar=prototype->minBytesPerChar;
            }

            if(staticData->maxBytesPerChar==0) {
                staticData->maxBytesPerChar=prototype->maxBytesPerChar;
            }

            if(staticData->subCharLen==0) {
                staticData->subCharLen=prototype->subCharLen;
                if(prototype->subCharLen>0) {
                    uprv_memcpy(staticData->subChar, prototype->subChar, prototype->subCharLen);
                }
            }
        }
    }

    if(data->ucm->states.outputType<0) {
        data->ucm->states.outputType=(int8_t)data->ucm->states.maxCharLength-1;
    }

    if( staticData->subChar1!=0 &&
            (staticData->minBytesPerChar>1 ||
                (staticData->conversionType!=UCNV_MBCS &&
                 staticData->conversionType!=UCNV_EBCDIC_STATEFUL))
    ) {
        fprintf(stderr, "error: <subchar1> defined for a type other than MBCS or EBCDIC_STATEFUL\n");
        *pErrorCode=U_INVALID_TABLE_FORMAT;
    }
}

/* return TRUE if a base table was read, FALSE for an extension table */
static UBool
readFile(ConvData *data, const char* converterName,
         UErrorCode *pErrorCode) {
    char line[1024];
    char *end;
    FileStream *convFile;

    UCMStates *baseStates;
    UBool dataIsBase;

    if(U_FAILURE(*pErrorCode)) {
        return FALSE;
    }

    data->ucm=ucm_open();

    convFile=T_FileStream_open(converterName, "r");
    if(convFile==NULL) {
        *pErrorCode=U_FILE_ACCESS_ERROR;
        return FALSE;
    }

    readHeader(data, convFile, pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        return FALSE;
    }

    if(data->ucm->baseName[0]==0) {
        dataIsBase=TRUE;
        baseStates=&data->ucm->states;
        ucm_processStates(baseStates, IGNORE_SISO_CHECK);
    } else {
        dataIsBase=FALSE;
        baseStates=NULL;
    }

    /* read the base table */
    ucm_readTable(data->ucm, convFile, dataIsBase, baseStates, pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        return FALSE;
    }

    /* read an extension table if there is one */
    while(T_FileStream_readLine(convFile, line, sizeof(line))) {
        end=uprv_strchr(line, 0);
        while(line<end &&
              (*(end-1)=='\n' || *(end-1)=='\r' || *(end-1)==' ' || *(end-1)=='\t')) {
            --end;
        }
        *end=0;

        if(line[0]=='#' || u_skipWhitespace(line)==end) {
            continue; /* ignore empty and comment lines */
        }

        if(0==uprv_strcmp(line, "CHARMAP")) {
            /* read the extension table */
            ucm_readTable(data->ucm, convFile, FALSE, baseStates, pErrorCode);
        } else {
            fprintf(stderr, "unexpected text after the base mapping table\n");
        }
        break;
    }

    T_FileStream_close(convFile);

    if(data->ucm->base->flagsType==UCM_FLAGS_MIXED || data->ucm->ext->flagsType==UCM_FLAGS_MIXED) {
        fprintf(stderr, "error: some entries have the mapping precision (with '|'), some do not\n");
        *pErrorCode=U_INVALID_TABLE_FORMAT;
    }

    return dataIsBase;
}

static void
createConverter(ConvData *data, const char *converterName, UErrorCode *pErrorCode) {
    ConvData baseData;
    UBool dataIsBase;

    UConverterStaticData *staticData;
    UCMStates *states, *baseStates;

    if(U_FAILURE(*pErrorCode)) {
        return;
    }

    initConvData(data);

    dataIsBase=readFile(data, converterName, pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        return;
    }

    staticData=&data->staticData;
    states=&data->ucm->states;

    if(dataIsBase) {
        /*
         * Build a normal .cnv file with a base table
         * and an optional extension table.
         */
        data->cnvData=MBCSOpen(data->ucm);
        if(data->cnvData==NULL) {
            *pErrorCode=U_MEMORY_ALLOCATION_ERROR;

        } else if(!data->cnvData->isValid(data->cnvData,
                            staticData->subChar, staticData->subCharLen)
        ) {
            fprintf(stderr, "       the substitution character byte sequence is illegal in this codepage structure!\n");
            *pErrorCode=U_INVALID_TABLE_FORMAT;

        } else if(staticData->subChar1!=0 &&
                    !data->cnvData->isValid(data->cnvData, &staticData->subChar1, 1)
        ) {
            fprintf(stderr, "       the subchar1 byte is illegal in this codepage structure!\n");
            *pErrorCode=U_INVALID_TABLE_FORMAT;

        } else if(
            data->ucm->ext->mappingsLength>0 &&
            !ucm_checkBaseExt(states, data->ucm->base, data->ucm->ext, data->ucm->ext, FALSE)
        ) {
            *pErrorCode=U_INVALID_TABLE_FORMAT;
        } else if(data->ucm->base->flagsType&UCM_FLAGS_EXPLICIT) {
            /* sort the table so that it can be turned into UTF-8-friendly data */
            ucm_sortTable(data->ucm->base);
        }

        if(U_SUCCESS(*pErrorCode)) {
            if(
                /* add the base table after ucm_checkBaseExt()! */
                !data->cnvData->addTable(data->cnvData, data->ucm->base, &data->staticData)
            ) {
                *pErrorCode=U_INVALID_TABLE_FORMAT;
            } else {
                /*
                 * addTable() may have requested moving more mappings to the extension table
                 * if they fit into the base toUnicode table but not into the
                 * base fromUnicode table.
                 * (Especially for UTF-8-friendly fromUnicode tables.)
                 * Such mappings will have the MBCS_FROM_U_EXT_FLAG set, which causes them
                 * to be excluded from the extension toUnicode data.
                 * See MBCSOkForBaseFromUnicode() for which mappings do not fit into
                 * the base fromUnicode table.
                 */
                ucm_moveMappings(data->ucm->base, data->ucm->ext);
                ucm_sortTable(data->ucm->ext);
                if(data->ucm->ext->mappingsLength>0) {
                    /* prepare the extension table, if there is one */
                    data->extData=CnvExtOpen(data->ucm);
                    if(data->extData==NULL) {
                        *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
                    } else if(
                        !data->extData->addTable(data->extData, data->ucm->ext, &data->staticData)
                    ) {
                        *pErrorCode=U_INVALID_TABLE_FORMAT;
                    }
                }
            }
        }
    } else {
        /* Build an extension-only .cnv file. */
        char baseFilename[500];
        char *basename;

        initConvData(&baseData);

        /* assemble a path/filename for data->ucm->baseName */
        uprv_strcpy(baseFilename, converterName);
        basename=(char *)findBasename(baseFilename);
        uprv_strcpy(basename, data->ucm->baseName);
        uprv_strcat(basename, ".ucm");

        /* read the base table */
        dataIsBase=readFile(&baseData, baseFilename, pErrorCode);
        if(U_FAILURE(*pErrorCode)) {
            return;
        } else if(!dataIsBase) {
            fprintf(stderr, "error: the <icu:base> file \"%s\" is not a base table file\n", baseFilename);
            *pErrorCode=U_INVALID_TABLE_FORMAT;
        } else {
            /* prepare the extension table */
            data->extData=CnvExtOpen(data->ucm);
            if(data->extData==NULL) {
                *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
            } else {
                /* fill in gaps in extension file header fields */
                UCMapping *m, *mLimit;
                uint8_t fallbackFlags;

                baseStates=&baseData.ucm->states;
                if(states->conversionType==UCNV_DBCS) {
                    staticData->minBytesPerChar=(int8_t)(states->minCharLength=2);
                } else if(states->minCharLength==0) {
                    staticData->minBytesPerChar=(int8_t)(states->minCharLength=baseStates->minCharLength);
                }
                if(states->maxCharLength<states->minCharLength) {
                    staticData->maxBytesPerChar=(int8_t)(states->maxCharLength=baseStates->maxCharLength);
                }

                if(staticData->subCharLen==0) {
                    uprv_memcpy(staticData->subChar, baseData.staticData.subChar, 4);
                    staticData->subCharLen=baseData.staticData.subCharLen;
                }
                /*
                 * do not copy subChar1 -
                 * only use what is explicitly specified
                 * because it cannot be unset in the extension file header
                 */

                /* get the fallback flags */
                fallbackFlags=0;
                for(m=baseData.ucm->base->mappings, mLimit=m+baseData.ucm->base->mappingsLength;
                    m<mLimit && fallbackFlags!=3;
                    ++m
                ) {
                    if(m->f==1) {
                        fallbackFlags|=1;
                    } else if(m->f==3) {
                        fallbackFlags|=2;
                    }
                }

                if(fallbackFlags&1) {
                    staticData->hasFromUnicodeFallback=TRUE;
                }
                if(fallbackFlags&2) {
                    staticData->hasToUnicodeFallback=TRUE;
                }

                if(1!=ucm_countChars(baseStates, staticData->subChar, staticData->subCharLen)) {
                    fprintf(stderr, "       the substitution character byte sequence is illegal in this codepage structure!\n");
                    *pErrorCode=U_INVALID_TABLE_FORMAT;

                } else if(staticData->subChar1!=0 && 1!=ucm_countChars(baseStates, &staticData->subChar1, 1)) {
                    fprintf(stderr, "       the subchar1 byte is illegal in this codepage structure!\n");
                    *pErrorCode=U_INVALID_TABLE_FORMAT;

                } else if(
                    !ucm_checkValidity(data->ucm->ext, baseStates) ||
                    !ucm_checkBaseExt(baseStates, baseData.ucm->base, data->ucm->ext, data->ucm->ext, FALSE)
                ) {
                    *pErrorCode=U_INVALID_TABLE_FORMAT;
                } else {
                    if(states->maxCharLength>1) {
                        /*
                         * When building a normal .cnv file with a base table
                         * for an MBCS (not SBCS) table with explicit precision flags,
                         * the MBCSAddTable() function marks some mappings for moving
                         * to the extension table.
                         * They fit into the base toUnicode table but not into the
                         * base fromUnicode table.
                         * (Note: We do have explicit precision flags because they are
                         * required for extension table generation, and
                         * ucm_checkBaseExt() verified it.)
                         *
                         * We do not call MBCSAddTable() here (we probably could)
                         * so we need to do the analysis before building the extension table.
                         * We assume that MBCSAddTable() will build a UTF-8-friendly table.
                         * Redundant mappings in the extension table are ok except they cost some size.
                         *
                         * Do this after ucm_checkBaseExt().
                         */
                        const MBCSData *mbcsData=MBCSGetDummy();
                        int32_t needsMove=0;
                        for(m=baseData.ucm->base->mappings, mLimit=m+baseData.ucm->base->mappingsLength;
                            m<mLimit;
                            ++m
                        ) {
                            if(!MBCSOkForBaseFromUnicode(mbcsData, m->b.bytes, m->bLen, m->u, m->f)) {
                                m->f|=MBCS_FROM_U_EXT_FLAG;
                                m->moveFlag=UCM_MOVE_TO_EXT;
                                ++needsMove;
                            }
                        }

                        if(needsMove!=0) {
                            ucm_moveMappings(baseData.ucm->base, data->ucm->ext);
                            ucm_sortTable(data->ucm->ext);
                        }
                    }
                    if(!data->extData->addTable(data->extData, data->ucm->ext, &data->staticData)) {
                        *pErrorCode=U_INVALID_TABLE_FORMAT;
                    }
                }
            }
        }

        cleanupConvData(&baseData);
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
