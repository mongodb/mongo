// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2009-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  gennorm2.cpp
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2009nov25
*   created by: Markus W. Scherer
*
*   This program reads text files that define Unicode normalization,
*   parses them, and builds a binary data file.
*/

#include "unicode/utypes.h"
#include "n2builder.h"

#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include "unicode/errorcode.h"
#include "unicode/localpointer.h"
#include "unicode/putil.h"
#include "unicode/uchar.h"
#include "unicode/unistr.h"
#include "charstr.h"
#include "normalizer2impl.h"
#include "toolutil.h"
#include "uoptions.h"
#include "uparse.h"

#if UCONFIG_NO_NORMALIZATION
#include "unewdata.h"
#endif

U_NAMESPACE_BEGIN

UBool beVerbose=FALSE, haveCopyright=TRUE;

#if !UCONFIG_NO_NORMALIZATION
void parseFile(std::ifstream &f, Normalizer2DataBuilder &builder);
#endif

/* -------------------------------------------------------------------------- */

enum {
    HELP_H,
    HELP_QUESTION_MARK,
    VERBOSE,
    COPYRIGHT,
    SOURCEDIR,
    OUTPUT_FILENAME,
    UNICODE_VERSION,
    WRITE_C_SOURCE,
    WRITE_COMBINED_DATA,
    OPT_FAST
};

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_VERBOSE,
    UOPTION_COPYRIGHT,
    UOPTION_SOURCEDIR,
    UOPTION_DEF("output", 'o', UOPT_REQUIRES_ARG),
    UOPTION_DEF("unicode", 'u', UOPT_REQUIRES_ARG),
    UOPTION_DEF("csource", '\1', UOPT_NO_ARG),
    UOPTION_DEF("combined", '\1', UOPT_NO_ARG),
    UOPTION_DEF("fast", '\1', UOPT_NO_ARG)
};

extern "C" int
main(int argc, char* argv[]) {
    U_MAIN_INIT_ARGS(argc, argv);

    /* preset then read command line options */
    options[SOURCEDIR].value="";
    argc=u_parseArgs(argc, argv, sizeof(options)/sizeof(options[HELP_H]), options);

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    }
    if(!options[OUTPUT_FILENAME].doesOccur) {
        argc=-1;
    }
    if( argc<2 ||
        options[HELP_H].doesOccur || options[HELP_QUESTION_MARK].doesOccur
    ) {
        fprintf(stderr,
            "Usage: %s [-options] infiles+ -o outputfilename\n"
            "\n"
            "Reads the infiles with normalization data and\n"
            "creates a binary file, or a C source file (--csource), with the data,\n"
            "or writes a data file with the combined data (--combined).\n"
            "See https://unicode-org.github.io/icu/userguide/transforms/normalization#data-file-syntax\n"
            "\n"
            "Alternate usage: %s [-options] a.txt b.txt minus p.txt q.txt -o outputfilename\n"
            "\n"
            "Computes the difference of (a, b) minus (p, q) and writes the diff data\n"
            "in input-file syntax to the outputfilename.\n"
            "It is then possible to build (p, q, diff) to get the same data as (a, b).\n"
            "(Useful for computing minimal incremental mapping data files.)\n"
            "\n",
            argv[0], argv[0]);
        fprintf(stderr,
            "Options:\n"
            "\t-h or -? or --help  this usage text\n"
            "\t-v or --verbose     verbose output\n"
            "\t-c or --copyright   include a copyright notice\n"
            "\t-u or --unicode     Unicode version, followed by the version like 5.2.0\n");
        fprintf(stderr,
            "\t-s or --sourcedir   source directory, followed by the path\n"
            "\t-o or --output      output filename\n"
            "\t      --csource     writes a C source file with initializers\n"
            "\t      --combined    writes a .txt file (input-file syntax) with the\n"
            "\t                    combined data from all of the input files\n");
        fprintf(stderr,
            "\t      --fast        optimize the data for fast normalization,\n"
            "\t                    which might increase its size  (Writes fully decomposed\n"
            "\t                    regular mappings instead of delta mappings.\n"
            "\t                    You should measure the runtime speed to make sure that\n"
            "\t                    this is a good trade-off.)\n");
        return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }

    beVerbose=options[VERBOSE].doesOccur;
    haveCopyright=options[COPYRIGHT].doesOccur;

    IcuToolErrorCode errorCode("gennorm2/main()");

#if UCONFIG_NO_NORMALIZATION

    fprintf(stderr,
        "gennorm2 writes a dummy binary data file "
        "because UCONFIG_NO_NORMALIZATION is set, \n"
        "see icu/source/common/unicode/uconfig.h\n");
    udata_createDummy(NULL, NULL, options[OUTPUT_FILENAME].value, errorCode);
    // Should not return an error since this is the expected behaviour if UCONFIG_NO_NORMALIZATION is on.
    // return U_UNSUPPORTED_ERROR;
    return 0;

#else

    LocalPointer<Normalizer2DataBuilder> b1(new Normalizer2DataBuilder(errorCode), errorCode);
    LocalPointer<Normalizer2DataBuilder> b2;
    LocalPointer<Normalizer2DataBuilder> diff;
    Normalizer2DataBuilder *builder = b1.getAlias();
    errorCode.assertSuccess();

    if(options[UNICODE_VERSION].doesOccur) {
        builder->setUnicodeVersion(options[UNICODE_VERSION].value);
    }

    if(options[OPT_FAST].doesOccur) {
        builder->setOptimization(Normalizer2DataBuilder::OPTIMIZE_FAST);
    }

    // prepare the filename beginning with the source dir
    CharString filename(options[SOURCEDIR].value, errorCode);
    int32_t pathLength=filename.length();
    if( pathLength>0 &&
        filename[pathLength-1]!=U_FILE_SEP_CHAR &&
        filename[pathLength-1]!=U_FILE_ALT_SEP_CHAR
    ) {
        filename.append(U_FILE_SEP_CHAR, errorCode);
        pathLength=filename.length();
    }

    bool doMinus = false;
    for(int i=1; i<argc; ++i) {
        printf("gennorm2: processing %s\n", argv[i]);
        if(strcmp(argv[i], "minus") == 0) {
            if(doMinus) {
                fprintf(stderr, "gennorm2 error: only one 'minus' can be specified\n");
                exit(U_ILLEGAL_ARGUMENT_ERROR);
            }
            // Data from previous input files has been collected in b1.
            // Collect data from further input files in b2.
            b2.adoptInsteadAndCheckErrorCode(new Normalizer2DataBuilder(errorCode), errorCode);
            diff.adoptInsteadAndCheckErrorCode(new Normalizer2DataBuilder(errorCode), errorCode);
            errorCode.assertSuccess();
            builder = b2.getAlias();
            if(options[UNICODE_VERSION].doesOccur) {
                builder->setUnicodeVersion(options[UNICODE_VERSION].value);
            }
            if(options[OPT_FAST].doesOccur) {
                builder->setOptimization(Normalizer2DataBuilder::OPTIMIZE_FAST);
            }
            doMinus = true;
            continue;
        }
        filename.append(argv[i], errorCode);
        std::ifstream f(filename.data());
        if(f.fail()) {
            fprintf(stderr, "gennorm2 error: unable to open %s\n", filename.data());
            exit(U_FILE_ACCESS_ERROR);
        }
        builder->setOverrideHandling(Normalizer2DataBuilder::OVERRIDE_PREVIOUS);
        parseFile(f, *builder);
        filename.truncate(pathLength);
    }

    if(doMinus) {
        Normalizer2DataBuilder::computeDiff(*b1, *b2, *diff);
        diff->writeDataFile(options[OUTPUT_FILENAME].value, /* writeRemoved= */ true);
    } else if(options[WRITE_COMBINED_DATA].doesOccur) {
        builder->writeDataFile(options[OUTPUT_FILENAME].value, /* writeRemoved= */ false);
    } else if(options[WRITE_C_SOURCE].doesOccur) {
        builder->writeCSourceFile(options[OUTPUT_FILENAME].value);
    } else {
        builder->writeBinaryFile(options[OUTPUT_FILENAME].value);
    }

    return errorCode.get();

#endif
}

#if !UCONFIG_NO_NORMALIZATION

void parseFile(std::ifstream &f, Normalizer2DataBuilder &builder) {
    IcuToolErrorCode errorCode("gennorm2/parseFile()");
    std::string lineString;
    uint32_t startCP, endCP;
    while(std::getline(f, lineString)) {
        if (lineString.empty()) {
            continue;  // skip empty lines.
        }
#if (U_CPLUSPLUS_VERSION >= 11)
        char *line = &lineString.front();
#else
        char *line = &lineString.at(0);
#endif
        char *comment=(char *)strchr(line, '#');
        if(comment!=NULL) {
            *comment=0;
        }
        u_rtrim(line);
        if(line[0]==0) {
            continue;  // skip empty and comment-only lines
        }
        if(line[0]=='*') {
            const char *s=u_skipWhitespace(line+1);
            if(0==strncmp(s, "Unicode", 7)) {
                s=u_skipWhitespace(s+7);
                builder.setUnicodeVersion(s);
            }
            continue;  // reserved syntax
        }
        const char *delimiter;
        int32_t rangeLength=
            u_parseCodePointRangeAnyTerminator(line, &startCP, &endCP, &delimiter, errorCode);
        if(errorCode.isFailure()) {
            fprintf(stderr, "gennorm2 error: parsing code point range from %s\n", line);
            exit(errorCode.reset());
        }
        if (endCP >= 0xd800 && startCP <= 0xdfff) {
                fprintf(stderr, "gennorm2 error: value or mapping for surrogate code points: %s\n",
                        line);
                exit(U_ILLEGAL_ARGUMENT_ERROR);
        }
        delimiter=u_skipWhitespace(delimiter);
        if(*delimiter==':') {
            const char *s=u_skipWhitespace(delimiter+1);
            char *end;
            unsigned long value=strtoul(s, &end, 10);
            if(end<=s || *u_skipWhitespace(end)!=0 || value>=0xff) {
                fprintf(stderr, "gennorm2 error: parsing ccc from %s\n", line);
                exit(U_PARSE_ERROR);
            }
            for(UChar32 c=(UChar32)startCP; c<=(UChar32)endCP; ++c) {
                builder.setCC(c, (uint8_t)value);
            }
            continue;
        }
        if(*delimiter=='-') {
            if(*u_skipWhitespace(delimiter+1)!=0) {
                fprintf(stderr, "gennorm2 error: parsing remove-mapping %s\n", line);
                exit(U_PARSE_ERROR);
            }
            for(UChar32 c=(UChar32)startCP; c<=(UChar32)endCP; ++c) {
                builder.removeMapping(c);
            }
            continue;
        }
        if(*delimiter=='=' || *delimiter=='>') {
            UChar uchars[Normalizer2Impl::MAPPING_LENGTH_MASK];
            int32_t length=u_parseString(delimiter+1, uchars, UPRV_LENGTHOF(uchars), NULL, errorCode);
            if(errorCode.isFailure()) {
                fprintf(stderr, "gennorm2 error: parsing mapping string from %s\n", line);
                exit(errorCode.reset());
            }
            UnicodeString mapping(FALSE, uchars, length);
            if(*delimiter=='=') {
                if(rangeLength!=1) {
                    fprintf(stderr,
                            "gennorm2 error: round-trip mapping for more than 1 code point on %s\n",
                            line);
                    exit(U_PARSE_ERROR);
                }
                builder.setRoundTripMapping((UChar32)startCP, mapping);
            } else {
                for(UChar32 c=(UChar32)startCP; c<=(UChar32)endCP; ++c) {
                    builder.setOneWayMapping(c, mapping);
                }
            }
            continue;
        }
        fprintf(stderr, "gennorm2 error: unrecognized data line %s\n", line);
        exit(U_PARSE_ERROR);
    }
}

#endif // !UCONFIG_NO_NORMALIZATION

U_NAMESPACE_END

/*
 * Hey, Emacs, please set the following:
 *
 * Local Variables:
 * indent-tabs-mode: nil
 * End:
 *
 */
