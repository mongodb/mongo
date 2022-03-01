// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  gensprep.c
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003-02-06
*   created by: Ram Viswanadha
*
*   This program reads the Profile.txt files,
*   parses them, and extracts the data for StringPrep profile.
*   It then preprocesses it and writes a binary file for efficient use
*   in various StringPrep conversion processes.
*/

#define USPREP_TYPE_NAMES_ARRAY 1

#include <stdio.h>
#include <stdlib.h>

#include "cmemory.h"
#include "cstring.h"
#include "toolutil.h"
#include "unewdata.h"
#include "uoptions.h"
#include "uparse.h"
#include "sprpimpl.h"

#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/utypes.h"
#include "unicode/putil.h"


U_CDECL_BEGIN
#include "gensprep.h"
U_CDECL_END

UBool beVerbose=FALSE, haveCopyright=TRUE;

#define NORM_CORRECTIONS_FILE_NAME "NormalizationCorrections.txt"

#define NORMALIZE_DIRECTIVE "normalize"
#define NORMALIZE_DIRECTIVE_LEN 9
#define CHECK_BIDI_DIRECTIVE "check-bidi"
#define CHECK_BIDI_DIRECTIVE_LEN 10

/* prototypes --------------------------------------------------------------- */

static void
parseMappings(const char *filename, UBool reportError, UErrorCode *pErrorCode);

static void
parseNormalizationCorrections(const char *filename, UErrorCode *pErrorCode);


/* -------------------------------------------------------------------------- */

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_VERBOSE,
    UOPTION_COPYRIGHT,
    UOPTION_DESTDIR,
    UOPTION_SOURCEDIR,
    UOPTION_ICUDATADIR,
    UOPTION_BUNDLE_NAME,
    { "normalization", NULL, NULL, NULL, 'n', UOPT_REQUIRES_ARG, 0 },
    { "norm-correction", NULL, NULL, NULL, 'm', UOPT_REQUIRES_ARG, 0 },
    { "check-bidi", NULL, NULL, NULL,  'k', UOPT_NO_ARG, 0},
    { "unicode", NULL, NULL, NULL, 'u', UOPT_REQUIRES_ARG, 0 },
};

enum{
    HELP,
    HELP_QUESTION_MARK,
    VERBOSE,
    COPYRIGHT,
    DESTDIR,
    SOURCEDIR,
    ICUDATADIR,
    BUNDLE_NAME,
    NORMALIZE,
    NORM_CORRECTION_DIR,
    CHECK_BIDI,
    UNICODE_VERSION
};

static int printHelp(int argc, char* argv[]){
    /*
     * Broken into chucks because the C89 standard says the minimum
     * required supported string length is 509 bytes.
     */
    fprintf(stderr,
        "Usage: %s [-options] [file_name]\n"
        "\n"
        "Read the files specified and\n"
        "create a binary file [package-name]_[bundle-name]." DATA_TYPE " with the StringPrep profile data\n"
        "\n",
        argv[0]);
    fprintf(stderr,
        "Options:\n"
        "\t-h or -? or --help       print this usage text\n"
        "\t-v or --verbose          verbose output\n"
        "\t-c or --copyright        include a copyright notice\n");
    fprintf(stderr,
        "\t-d or --destdir          destination directory, followed by the path\n"
        "\t-s or --sourcedir        source directory of ICU data, followed by the path\n"
        "\t-b or --bundle-name      generate the output data file with the name specified\n"
        "\t-i or --icudatadir       directory for locating any needed intermediate data files,\n"
        "\t                         followed by path, defaults to %s\n",
        u_getDataDirectory());
    fprintf(stderr,
        "\t-n or --normalize        turn on the option for normalization and include mappings\n"
        "\t                         from NormalizationCorrections.txt from the given path,\n"
        "\t                         e.g: /test/icu/source/data/unidata\n");
    fprintf(stderr,
        "\t-m or --norm-correction  use NormalizationCorrections.txt from the given path\n"
        "\t                         when the input file contains a normalization directive.\n"
        "\t                         unlike -n/--normalize, this option does not force the\n"
        "\t                         normalization.\n");
    fprintf(stderr,
        "\t-k or --check-bidi       turn on the option for checking for BiDi in the profile\n"
        "\t-u or --unicode          version of Unicode to be used with this profile followed by the version\n"
        );
    return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
}


extern int
main(int argc, char* argv[]) {
#if !UCONFIG_NO_IDNA
    char* filename = NULL;
#endif
    const char *srcDir=NULL, *destDir=NULL, *icuUniDataDir=NULL;
    const char *bundleName=NULL, *inputFileName = NULL;
    char *basename=NULL;
    int32_t sprepOptions = 0;

    UErrorCode errorCode=U_ZERO_ERROR;

    U_MAIN_INIT_ARGS(argc, argv);

    /* preset then read command line options */
    options[DESTDIR].value=u_getDataDirectory();
    options[SOURCEDIR].value="";
    options[UNICODE_VERSION].value="0"; /* don't assume the unicode version */
    options[BUNDLE_NAME].value = DATA_NAME;
    options[NORMALIZE].value = "";

    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    }
    if(argc<0 || options[HELP].doesOccur || options[HELP_QUESTION_MARK].doesOccur) {
        return printHelp(argc, argv);
        
    }

    /* get the options values */
    beVerbose=options[VERBOSE].doesOccur;
    haveCopyright=options[COPYRIGHT].doesOccur;
    srcDir=options[SOURCEDIR].value;
    destDir=options[DESTDIR].value;
    bundleName = options[BUNDLE_NAME].value;
    if(options[NORMALIZE].doesOccur) {
        icuUniDataDir = options[NORMALIZE].value;
    } else {
        icuUniDataDir = options[NORM_CORRECTION_DIR].value;
    }

    if(argc<2) {
        /* print the help message */
        return printHelp(argc, argv);
    } else {
        inputFileName = argv[1];
    }
    if(!options[UNICODE_VERSION].doesOccur){
        return printHelp(argc, argv);
    }
    if(options[ICUDATADIR].doesOccur) {
        u_setDataDirectory(options[ICUDATADIR].value);
    }
#if UCONFIG_NO_IDNA

    fprintf(stderr,
        "gensprep writes dummy " U_ICUDATA_NAME "_" DATA_NAME "." DATA_TYPE
        " because UCONFIG_NO_IDNA is set, \n"
        "see icu/source/common/unicode/uconfig.h\n");
    generateData(destDir, bundleName);

#else

    setUnicodeVersion(options[UNICODE_VERSION].value);
    filename = (char* ) uprv_malloc(uprv_strlen(srcDir) + uprv_strlen(inputFileName) + (icuUniDataDir == NULL ? 0 : uprv_strlen(icuUniDataDir)) + 40); /* hopefully this should be enough */
   
    /* prepare the filename beginning with the source dir */
    if(uprv_strchr(srcDir,U_FILE_SEP_CHAR) == NULL && uprv_strchr(srcDir,U_FILE_ALT_SEP_CHAR) == NULL){
        filename[0] = '.';
        filename[1] = U_FILE_SEP_CHAR;
        uprv_strcpy(filename+2,srcDir);
    }else{
        uprv_strcpy(filename, srcDir);
    }
    
    basename=filename+uprv_strlen(filename);
    if(basename>filename && *(basename-1)!=U_FILE_SEP_CHAR) {
        *basename++=U_FILE_SEP_CHAR;
    }
    
    /* initialize */
    init();

    /* process the file */
    uprv_strcpy(basename,inputFileName);
    parseMappings(filename,FALSE, &errorCode);
    if(U_FAILURE(errorCode)) {
        fprintf(stderr, "Could not open file %s for reading. Error: %s \n", filename, u_errorName(errorCode));
        return errorCode;
    }
    
    if(options[NORMALIZE].doesOccur){ /* this option might be set by @normalize;; in the source file */
        /* set up directory for NormalizationCorrections.txt */
        uprv_strcpy(filename,icuUniDataDir);
        basename=filename+uprv_strlen(filename);
        if(basename>filename && *(basename-1)!=U_FILE_SEP_CHAR) {
            *basename++=U_FILE_SEP_CHAR;
        }

        *basename++=U_FILE_SEP_CHAR;
        uprv_strcpy(basename,NORM_CORRECTIONS_FILE_NAME);
    
        parseNormalizationCorrections(filename,&errorCode);
        if(U_FAILURE(errorCode)){
            fprintf(stderr,"Could not open file %s for reading \n", filename);
            return errorCode;
        }
        sprepOptions |= _SPREP_NORMALIZATION_ON;
    }
    
    if(options[CHECK_BIDI].doesOccur){ /* this option might be set by @check-bidi;; in the source file */
        sprepOptions |= _SPREP_CHECK_BIDI_ON;
    }

    setOptions(sprepOptions);

    /* process parsed data */
    if(U_SUCCESS(errorCode)) {
        /* write the data file */
        generateData(destDir, bundleName);

        cleanUpData();
    }

    uprv_free(filename);

    u_cleanup();

#endif

    return errorCode;
}

#if !UCONFIG_NO_IDNA

static void U_CALLCONV
normalizationCorrectionsLineFn(void *context,
                    char *fields[][2], int32_t fieldCount,
                    UErrorCode *pErrorCode) {
    (void)context; // suppress compiler warnings about unused variable
    (void)fieldCount; // suppress compiler warnings about unused variable
    uint32_t mapping[40];
    char *end, *s;
    uint32_t code;
    int32_t length;
    UVersionInfo version;
    UVersionInfo thisVersion;

    /* get the character code, field 0 */
    code=(uint32_t)uprv_strtoul(fields[0][0], &end, 16);
    if(U_FAILURE(*pErrorCode)) {
        fprintf(stderr, "gensprep: error parsing NormalizationCorrections.txt mapping at %s\n", fields[0][0]);
        exit(*pErrorCode);
    }
    /* Original (erroneous) decomposition */
    s = fields[1][0];

    /* parse the mapping string */
    length=u_parseCodePoints(s, mapping, sizeof(mapping)/4, pErrorCode);

    /* ignore corrected decomposition */

    u_versionFromString(version,fields[3][0] );
    u_versionFromString(thisVersion, "3.2.0");



    if(U_FAILURE(*pErrorCode)) {
        fprintf(stderr, "gensprep error parsing NormalizationCorrections.txt of U+%04lx - %s\n",
                (long)code, u_errorName(*pErrorCode));
        exit(*pErrorCode);
    }

    /* store the mapping */
    if( version[0] > thisVersion[0] || 
        ((version[0]==thisVersion[0]) && (version[1] > thisVersion[1]))
        ){
        storeMapping(code,mapping, length, USPREP_MAP, pErrorCode);
    }
    setUnicodeVersionNC(version);
}

static void
parseNormalizationCorrections(const char *filename, UErrorCode *pErrorCode) {
    char *fields[4][2];

    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return;
    }

    u_parseDelimitedFile(filename, ';', fields, 4, normalizationCorrectionsLineFn, NULL, pErrorCode);

    /* fprintf(stdout,"Number of code points that have NormalizationCorrections mapping with length >1 : %i\n",len); */

    if(U_FAILURE(*pErrorCode) && ( *pErrorCode!=U_FILE_ACCESS_ERROR)) {
        fprintf(stderr, "gensprep error: u_parseDelimitedFile(\"%s\") failed - %s\n", filename, u_errorName(*pErrorCode));
        exit(*pErrorCode);
    }
}

static void U_CALLCONV
strprepProfileLineFn(void *context,
              char *fields[][2], int32_t fieldCount,
              UErrorCode *pErrorCode) {
    (void)fieldCount; // suppress compiler warnings about unused variable  
    uint32_t mapping[40];
    char *end, *map;
    uint32_t code;
    int32_t length;
   /*UBool* mapWithNorm = (UBool*) context;*/
    const char* typeName;
    uint32_t rangeStart=0,rangeEnd =0;
    const char* filename = (const char*) context;
    const char *s;

    s = u_skipWhitespace(fields[0][0]);
    if (*s == '@') {
        /* special directive */
        s++;
        length = (int32_t)(fields[0][1] - s);
        if (length >= NORMALIZE_DIRECTIVE_LEN
            && uprv_strncmp(s, NORMALIZE_DIRECTIVE, NORMALIZE_DIRECTIVE_LEN) == 0) {
            options[NORMALIZE].doesOccur = TRUE;
            return;
        }
        else if (length >= CHECK_BIDI_DIRECTIVE_LEN
            && uprv_strncmp(s, CHECK_BIDI_DIRECTIVE, CHECK_BIDI_DIRECTIVE_LEN) == 0) {
            options[CHECK_BIDI].doesOccur = TRUE;
            return;
        }
        else {
            fprintf(stderr, "gensprep error parsing a directive %s.", fields[0][0]);
        }
    }

    typeName = fields[2][0];
    map = fields[1][0];

    if(uprv_strstr(typeName, usprepTypeNames[USPREP_UNASSIGNED])!=NULL){

        u_parseCodePointRange(s, &rangeStart,&rangeEnd, pErrorCode);
        if(U_FAILURE(*pErrorCode)){
            fprintf(stderr, "Could not parse code point range. Error: %s\n",u_errorName(*pErrorCode));
            return;
        }

        /* store the range */
        storeRange(rangeStart,rangeEnd,USPREP_UNASSIGNED, pErrorCode);

    }else if(uprv_strstr(typeName, usprepTypeNames[USPREP_PROHIBITED])!=NULL){

        u_parseCodePointRange(s, &rangeStart,&rangeEnd, pErrorCode);
        if(U_FAILURE(*pErrorCode)){
            fprintf(stderr, "Could not parse code point range. Error: %s\n",u_errorName(*pErrorCode));
            return;
        }

        /* store the range */
        storeRange(rangeStart,rangeEnd,USPREP_PROHIBITED, pErrorCode);

    }else if(uprv_strstr(typeName, usprepTypeNames[USPREP_MAP])!=NULL){

        /* get the character code, field 0 */
        code=(uint32_t)uprv_strtoul(s, &end, 16);
        if(end<=s || end!=fields[0][1]) {
            fprintf(stderr, "gensprep: syntax error in field 0 at %s\n", fields[0][0]);
            *pErrorCode=U_PARSE_ERROR;
            exit(U_PARSE_ERROR);
        }

        /* parse the mapping string */
        length=u_parseCodePoints(map, mapping, sizeof(mapping)/4, pErrorCode);
        
        /* store the mapping */
        storeMapping(code,mapping, length,USPREP_MAP, pErrorCode);

    }else{
        *pErrorCode = U_INVALID_FORMAT_ERROR;
    }
    
    if(U_FAILURE(*pErrorCode)) {
        fprintf(stderr, "gensprep error parsing  %s line %s at %s. Error: %s\n",filename,
               fields[0][0],fields[2][0],u_errorName(*pErrorCode));
        exit(*pErrorCode);
    }

}

static void
parseMappings(const char *filename, UBool reportError, UErrorCode *pErrorCode) {
    char *fields[3][2];

    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return;
    }

    u_parseDelimitedFile(filename, ';', fields, 3, strprepProfileLineFn, (void*)filename, pErrorCode);

    /*fprintf(stdout,"Number of code points that have mappings with length >1 : %i\n",len);*/

    if(U_FAILURE(*pErrorCode) && (reportError || *pErrorCode!=U_FILE_ACCESS_ERROR)) {
        fprintf(stderr, "gensprep error: u_parseDelimitedFile(\"%s\") failed - %s\n", filename, u_errorName(*pErrorCode));
        exit(*pErrorCode);
    }
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
