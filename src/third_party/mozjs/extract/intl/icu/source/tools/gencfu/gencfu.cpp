// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 2009-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*
* File gencfu.c
*/

//--------------------------------------------------------------------
//
//   Tool for generating Unicode Confusable data files (.cfu files).
//   .cfu files contain the compiled of the confusable data
//   derived from the Unicode Consortium data described in
//   Unicode UAX 39.
//
//   Usage:  gencfu [options] -r confusables-file.txt -o output-file.cfu
//
//       options:   -v         verbose
//                  -? or -h   help
//
//   The input rule filew is are plain text files containing confusable character
//    definitions in the input format defined by Unicode UAX39 for the files
//    confusables.txt.  This source (.txt) format
//    is also accepted direaccepted by ICU spoof detedtors.  The
//    files must be encoded in utf-8 format, with or without a BOM.
//
//   The script used to compile confusablesWholeScript.txt into the CFU file
//    until the Unicode consortium deprecated it.
//
//--------------------------------------------------------------------

#include "unicode/utypes.h"
#include "unicode/unistr.h"
#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/putil.h"

#include "uoptions.h"
#include "unewdata.h"
#include "ucmndata.h"
#include "uspoof_impl.h"
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
    { "wsrules", NULL, NULL, NULL, 'w', UOPT_REQUIRES_ARG, 0},  /* 4 */  // deprecated
    { "out",   NULL, NULL, NULL, 'o', UOPT_REQUIRES_ARG, 0 },   /* 5 */
    UOPTION_ICUDATADIR,         /* 6 */
    UOPTION_DESTDIR,            /* 7 */
    UOPTION_COPYRIGHT,          /* 8 */
    UOPTION_QUIET,              /* 9 */
};

void usageAndDie(int retCode) {
        printf("Usage: %s [-v] [-options] -r confusablesRules.txt -o output-file\n", progName);
        printf("\tRead in Unicode confusable character definitions and write out the binary data\n"
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


#if UCONFIG_NO_REGULAR_EXPRESSIONS || UCONFIG_NO_NORMALIZATION || UCONFIG_NO_FILE_IO

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

    { 0x43, 0x66, 0x75, 0x20 },     //     dataFormat="Cfu "
    { 0xff, 0, 0, 0 },              //     formatVersion.  Filled in later with values
                                    //      from the  builder.  The  values declared
                                    //      here should never appear in any real data.
        { 5, 1, 0, 0 }              //   dataVersion (Unicode version)
    }};

#endif

// Forward declaration for function for reading source files.
static const char *readFile(const char *fileName, int32_t *len);

//----------------------------------------------------------------------------
//
//  main      for gencfu
//
//----------------------------------------------------------------------------
int  main(int argc, char **argv) {
    UErrorCode  status = U_ZERO_ERROR;
    const char *confFileName;
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

    if (!(options[3].doesOccur && options[5].doesOccur)) {
        fprintf(stderr, "confusables file and output file must all be specified.\n");
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }
    confFileName   = options[3].value;
    outFileName    = options[5].value;

    if (options[6].doesOccur) {
        u_setDataDirectory(options[6].value);
    }

    status = U_ZERO_ERROR;

    /* Combine the directory with the file name */
    if(options[7].doesOccur) {
        outDir = options[7].value;
    }
    if (options[8].doesOccur) {
        copyright = U_COPYRIGHT_STRING;
    }

    UBool quiet = false;
    if (options[9].doesOccur) {
      quiet = true;
    }

#if UCONFIG_NO_REGULAR_EXPRESSIONS || UCONFIG_NO_NORMALIZATION || UCONFIG_NO_FILE_IO
    // spoof detection data file parsing is dependent on regular expressions.
    // TODO: have the tool return an error status.  Requires fixing the ICU data build
    //       so that it doesn't abort entirely on that error.

    UNewDataMemory *pData;
    char msg[1024];

    /* write message with just the name */
    sprintf(msg, "gencfu writes dummy %s because of UCONFIG_NO_REGULAR_EXPRESSIONS and/or UCONFIG_NO_NORMALIZATION and/or UCONFIG_NO_FILE_IO, see uconfig.h", outFileName);
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

    //  Read in the confusables source file

    int32_t      confusablesLen = 0;
    const char  *confusables = readFile(confFileName, &confusablesLen);
    if (confusables == NULL) {
        printf("gencfu: error reading file  \"%s\"\n", confFileName);
        exit(-1);
    }

    //
    //  Create the Spoof Detector from the source confusables files.
    //     This will compile the data.
    //
    UParseError parseError;
    parseError.line = 0;
    parseError.offset = 0;
    int32_t errType;
    USpoofChecker *sc = uspoof_openFromSource(confusables, confusablesLen,
                                              NULL, 0,
                                              &errType, &parseError, &status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "gencfu: uspoof_openFromSource error \"%s\"  at file %s, line %d, column %d\n",
                u_errorName(status), confFileName, (int)parseError.line, (int)parseError.offset);
        exit(status);
    }


    //
    //  Get the compiled rule data from the USpoofChecker.
    //
    uint32_t        outDataSize;
    uint8_t        *outData;
    outDataSize = uspoof_serialize(sc, NULL, 0, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR) {
        fprintf(stderr, "gencfu: uspoof_serialize() returned %s\n", u_errorName(status));
        exit(status);
    }
    status = U_ZERO_ERROR;
    outData = new uint8_t[outDataSize];
    uspoof_serialize(sc, outData, outDataSize, &status);

    // Copy the data format version numbers from the spoof data header into the UDataMemory header.
    
    uprv_memcpy(dh.info.formatVersion, 
                reinterpret_cast<SpoofDataHeader *>(outData)->fFormatVersion,
                sizeof(dh.info.formatVersion));

    //
    //  Create the output file
    //
    size_t bytesWritten;
    UNewDataMemory *pData;
    pData = udata_create(outDir, NULL, outFileName, &(dh.info), copyright, &status);
    if(U_FAILURE(status)) {
        fprintf(stderr, "gencfu: Could not open output file \"%s\", \"%s\"\n", 
                         outFileName, u_errorName(status));
        exit(status);
    }


    //  Write the data itself.
    udata_writeBlock(pData, outData, outDataSize);
    // finish up 
    bytesWritten = udata_finish(pData, &status);
    if(U_FAILURE(status)) {
        fprintf(stderr, "gencfu: Error %d writing the output file\n", status);
        exit(status);
    }
    
    if (bytesWritten != outDataSize) {
        fprintf(stderr, "gencfu: Error writing to output file \"%s\"\n", outFileName);
        exit(-1);
    }

    uspoof_close(sc);
    delete [] outData;
    delete [] confusables;
    u_cleanup();
    if (!quiet) {
        printf("gencfu: tool completed successfully.\n");
    }
    return 0;
#endif   // UCONFIG_NO_REGULAR_EXPRESSIONS
}


 //
 //  Read in a confusables source file
 //
 static const char *readFile(const char *fileName, int32_t *len) {
    char       *result;
    long        fileSize;
    FILE        *file;

    file = fopen(fileName, "rb");
    if( file == 0 ) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    result = new char[fileSize+10];
    if (result==NULL) {
        fclose(file);
        return NULL;
    }

    long t = static_cast<long>(fread(result, 1, fileSize, file));
    if (t != fileSize)  {
        delete [] result;
        fclose(file);
        return NULL;
    }
    result[fileSize]=0;
    *len = static_cast<int32_t>(fileSize);
    fclose(file);
    return result;
 }
