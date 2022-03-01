// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 2002-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*
* File genbrk.c
*/

//--------------------------------------------------------------------
//
//   Tool for generating RuleBasedBreakIterator data files (.brk files).
//   .brk files contain the precompiled rules for standard types
//   of iterators - word, line, sentence, etc.
//
//   Usage:  genbrk [options] -r rule-file.txt  -o output-file.brk
//
//       options:   -v         verbose
//                  -? or -h   help
//
//   The input rule file is a plain text file containing break rules
//    in the input format accepted by RuleBasedBreakIterators.  The
//    file can be encoded as utf-8, or utf-16 (either endian), or
//    in the default code page (platform dependent.).  utf encoded
//    files must include a BOM.
//
//--------------------------------------------------------------------

#include "unicode/utypes.h"
#include "unicode/ucnv.h"
#include "unicode/unistr.h"
#include "unicode/rbbi.h"
#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/putil.h"

#include "uoptions.h"
#include "unewdata.h"
#include "ucmndata.h"
#include "rbbidata.h"
#include "cmemory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

U_NAMESPACE_USE

static char *progName;
static UOption options[]={
    UOPTION_HELP_H,             /* 0 */
    UOPTION_HELP_QUESTION_MARK, /* 1 */
    UOPTION_VERBOSE,            /* 2 */
    { "rules", NULL, NULL, NULL, 'r', UOPT_REQUIRES_ARG, 0 },   /* 3 */
    { "out",   NULL, NULL, NULL, 'o', UOPT_REQUIRES_ARG, 0 },   /* 4 */
    UOPTION_ICUDATADIR,         /* 5 */
    UOPTION_DESTDIR,            /* 6 */
    UOPTION_COPYRIGHT,          /* 7 */
    UOPTION_QUIET,              /* 8 */
};

void usageAndDie(int retCode) {
        printf("Usage: %s [-v] [-options] -r rule-file -o output-file\n", progName);
        printf("\tRead in break iteration rules text and write out the binary data\n"
            "options:\n"
            "\t-h or -? or --help  this usage text\n"
            "\t-V or --version     show a version message\n"
            "\t-c or --copyright   include a copyright notice\n"
            "\t-v or --verbose     turn on verbose output\n"
            "\t-q or --quiet       do not display warnings and progress\n"
            "\t-i or --icudatadir  directory for locating any needed intermediate data files,\n"
            "\t                    followed by path, defaults to %s\n"
            "\t-d or --destdir     destination directory, followed by the path\n",
            u_getDataDirectory());
        exit (retCode);
}


#if UCONFIG_NO_BREAK_ITERATION || UCONFIG_NO_FILE_IO

/* dummy UDataInfo cf. udata.h */
static UDataInfo dummyDataInfo = {
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

//
//  Set up the ICU data header, defined in ucmndata.h
//
DataHeader dh ={
    {sizeof(DataHeader),           // Struct MappedData
        0xda,
        0x27},

    {                               // struct UDataInfo
        sizeof(UDataInfo),          //     size
        0,                          //     reserved
        U_IS_BIG_ENDIAN,
        U_CHARSET_FAMILY,
        U_SIZEOF_UCHAR,
        0,                          //     reserved

    { 0x42, 0x72, 0x6b, 0x20 },     //     dataFormat="Brk "
    { 0xff, 0, 0, 0 },              //     formatVersion.  Filled in later with values
                                    //      from the RBBI rule builder.  The  values declared
                                    //      here should never appear in any real RBBI data.
        { 4, 1, 0, 0 }              //   dataVersion (Unicode version)
    }};

#endif

//----------------------------------------------------------------------------
//
//  main      for genbrk
//
//----------------------------------------------------------------------------
int  main(int argc, char **argv) {
    UErrorCode  status = U_ZERO_ERROR;
    const char *ruleFileName;
    const char *outFileName;
    const char *outDir = NULL;
    const char *copyright = NULL;

    //
    // Pick up and check the command line arguments,
    //    using the standard ICU tool utils option handling.
    //
    U_MAIN_INIT_ARGS(argc, argv);
    progName = argv[0];
    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);
    if(argc<0) {
        // Unrecognized option
        fprintf(stderr, "error in command line argument \"%s\"\n", argv[-argc]);
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }

    if(options[0].doesOccur || options[1].doesOccur) {
        //  -? or -h for help.
        usageAndDie(0);
    }

    if (!(options[3].doesOccur && options[4].doesOccur)) {
        fprintf(stderr, "rule file and output file must both be specified.\n");
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }
    ruleFileName = options[3].value;
    outFileName  = options[4].value;

    if (options[5].doesOccur) {
        u_setDataDirectory(options[5].value);
    }

    status = U_ZERO_ERROR;

    /* Combine the directory with the file name */
    if(options[6].doesOccur) {
        outDir = options[6].value;
    }
    if (options[7].doesOccur) {
        copyright = U_COPYRIGHT_STRING;
    }

#if UCONFIG_NO_BREAK_ITERATION || UCONFIG_NO_FILE_IO

    UNewDataMemory *pData;
    char msg[1024];

    /* write message with just the name */
    sprintf(msg, "genbrk writes dummy %s because of UCONFIG_NO_BREAK_ITERATION and/or UCONFIG_NO_FILE_IO, see uconfig.h", outFileName);
    fprintf(stderr, "%s\n", msg);

    /* write the dummy data file */
    pData = udata_create(outDir, NULL, outFileName, &dummyDataInfo, NULL, &status);
    udata_writeBlock(pData, msg, strlen(msg));
    udata_finish(pData, &status);
    return (int)status;

#else
    /* Initialize ICU */
    u_init(&status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "%s: can not initialize ICU.  status = %s\n",
            argv[0], u_errorName(status));
        exit(1);
    }
    status = U_ZERO_ERROR;

    //
    //  Read in the rule source file
    //
    long        result;
    long        ruleFileSize;
    FILE        *file;
    char        *ruleBufferC;

    file = fopen(ruleFileName, "rb");
    if( file == 0 ) {
        fprintf(stderr, "Could not open file \"%s\"\n", ruleFileName);
        exit(-1);
    }
    fseek(file, 0, SEEK_END);
    ruleFileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    ruleBufferC = new char[ruleFileSize+10];

    result = (long)fread(ruleBufferC, 1, ruleFileSize, file);
    if (result != ruleFileSize)  {
        fprintf(stderr, "Error reading file \"%s\"\n", ruleFileName);
        exit (-1);
    }
    ruleBufferC[ruleFileSize]=0;
    fclose(file);

    //
    // Look for a Unicode Signature (BOM) on the rule file
    //
    int32_t        signatureLength;
    const char *   ruleSourceC = ruleBufferC;
    const char*    encoding = ucnv_detectUnicodeSignature(
                           ruleSourceC, ruleFileSize, &signatureLength, &status);
    if (U_FAILURE(status)) {
        exit(status);
    }
    if(encoding!=NULL ){
        ruleSourceC  += signatureLength;
        ruleFileSize -= signatureLength;
    }

    //
    // Open a converter to take the rule file to UTF-16
    //
    UConverter* conv;
    conv = ucnv_open(encoding, &status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "ucnv_open: ICU Error \"%s\"\n", u_errorName(status));
        exit(status);
    }

    //
    // Convert the rules to UChar.
    //  Preflight first to determine required buffer size.
    //
    uint32_t destCap = ucnv_toUChars(conv,
                       NULL,           //  dest,
                       0,              //  destCapacity,
                       ruleSourceC,
                       ruleFileSize,
                       &status);
    if (status != U_BUFFER_OVERFLOW_ERROR) {
        fprintf(stderr, "ucnv_toUChars: ICU Error \"%s\"\n", u_errorName(status));
        exit(status);
    }

    status = U_ZERO_ERROR;
    UChar *ruleSourceU = new UChar[destCap+1];
    ucnv_toUChars(conv,
                  ruleSourceU,     //  dest,
                  destCap+1,
                  ruleSourceC,
                  ruleFileSize,
                  &status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "ucnv_toUChars: ICU Error \"%s\"\n", u_errorName(status));
        exit(status);
    }
    ucnv_close(conv);


    //
    //  Put the source rules into a UnicodeString
    //
    UnicodeString ruleSourceS(false, ruleSourceU, destCap);

    //
    //  Create the break iterator from the rules
    //     This will compile the rules.
    //
    UParseError parseError;
    parseError.line = 0;
    parseError.offset = 0;
    RuleBasedBreakIterator *bi = new RuleBasedBreakIterator(ruleSourceS, parseError, status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "createRuleBasedBreakIterator: ICU Error \"%s\"  at line %d, column %d\n",
                u_errorName(status), (int)parseError.line, (int)parseError.offset);
        exit(status);
    }


    //
    //  Get the compiled rule data from the break iterator.
    //
    uint32_t        outDataSize;
    const uint8_t  *outData;
    outData = bi->getBinaryRules(outDataSize);

    // Copy the data format version numbers from the RBBI data header into the UDataMemory header.
    uprv_memcpy(dh.info.formatVersion, ((RBBIDataHeader *)outData)->fFormatVersion, sizeof(dh.info.formatVersion));

    //
    //  Create the output file
    //
    size_t bytesWritten;
    UNewDataMemory *pData;
    pData = udata_create(outDir, NULL, outFileName, &(dh.info), copyright, &status);
    if(U_FAILURE(status)) {
        fprintf(stderr, "genbrk: Could not open output file \"%s\", \"%s\"\n", 
                         outFileName, u_errorName(status));
        exit(status);
    }


    //  Write the data itself.
    udata_writeBlock(pData, outData, outDataSize);
    // finish up 
    bytesWritten = udata_finish(pData, &status);
    if(U_FAILURE(status)) {
        fprintf(stderr, "genbrk: error %d writing the output file\n", status);
        exit(status);
    }
    
    if (bytesWritten != outDataSize) {
        fprintf(stderr, "Error writing to output file \"%s\"\n", outFileName);
        exit(-1);
    }

    delete bi;
    delete[] ruleSourceU;
    delete[] ruleBufferC;
    u_cleanup();


    if(!options[8].doesOccur) {
        printf("genbrk: tool completed successfully.\n");
    }
    return 0;

#endif /* #if !UCONFIG_NO_BREAK_ITERATION */
}

