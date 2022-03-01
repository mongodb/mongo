// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 2002-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*
* File gendict.cpp
*/

#include "unicode/utypes.h"
#include "unicode/uchar.h"
#include "unicode/ucnv.h"
#include "unicode/uniset.h"
#include "unicode/unistr.h"
#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/putil.h"
#include "unicode/ucharstriebuilder.h"
#include "unicode/bytestriebuilder.h"
#include "unicode/ucharstrie.h"
#include "unicode/bytestrie.h"
#include "unicode/ucnv.h"
#include "unicode/ustring.h"
#include "unicode/utf16.h"

#include "charstr.h"
#include "dictionarydata.h"
#include "uoptions.h"
#include "unewdata.h"
#include "cmemory.h"
#include "uassert.h"
#include "ucbuf.h"
#include "toolutil.h"
#include "cstring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putilimp.h"
UDate startTime;

static int elapsedTime() {
  return (int)uprv_floor((uprv_getRawUTCtime()-startTime)/1000.0);
}

U_NAMESPACE_USE

static char *progName;
static UOption options[]={
    UOPTION_HELP_H,             /* 0 */
    UOPTION_HELP_QUESTION_MARK, /* 1 */
    UOPTION_VERBOSE,            /* 2 */
    UOPTION_ICUDATADIR,         /* 4 */
    UOPTION_COPYRIGHT,          /* 5 */
    { "uchars", NULL, NULL, NULL, '\1', UOPT_NO_ARG, 0}, /* 6 */
    { "bytes", NULL, NULL, NULL, '\1', UOPT_NO_ARG, 0}, /* 7 */
    { "transform", NULL, NULL, NULL, '\1', UOPT_REQUIRES_ARG, 0}, /* 8 */
    UOPTION_QUIET,              /* 9 */
};

enum arguments {
    ARG_HELP = 0,
    ARG_QMARK,
    ARG_VERBOSE,
    ARG_ICUDATADIR,
    ARG_COPYRIGHT,
    ARG_UCHARS,
    ARG_BYTES,
    ARG_TRANSFORM,
    ARG_QUIET
};

// prints out the standard usage method describing command line arguments, 
// then bails out with the desired exit code
static void usageAndDie(UErrorCode retCode) {
    fprintf((U_SUCCESS(retCode) ? stdout : stderr), "Usage: %s -trietype [-options] input-dictionary-file output-file\n", progName);
    fprintf((U_SUCCESS(retCode) ? stdout : stderr),
           "\tRead in a word list and write out a string trie dictionary\n"
           "options:\n"
           "\t-h or -? or --help  this usage text\n"
           "\t-V or --version     show a version message\n"
           "\t-c or --copyright   include a copyright notice\n"
           "\t-v or --verbose     turn on verbose output\n"
           "\t-q or --quiet       do not display warnings and progress\n"
           "\t-i or --icudatadir  directory for locating any needed intermediate data files,\n" // TODO: figure out if we need this option
           "\t                    followed by path, defaults to %s\n"
           "\t--uchars            output a UCharsTrie (mutually exclusive with -b!)\n"
           "\t--bytes             output a BytesTrie (mutually exclusive with -u!)\n"
           "\t--transform         the kind of transform to use (eg --transform offset-40A3,\n"
           "\t                    which specifies an offset transform with constant 0x40A3)\n",
            u_getDataDirectory());
    exit(retCode);
}


/* UDataInfo cf. udata.h */
static UDataInfo dataInfo = {
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    U_SIZEOF_UCHAR,
    0,

    { 0x44, 0x69, 0x63, 0x74 },     /* "Dict" */
    { 1, 0, 0, 0 },                 /* format version */
    { 0, 0, 0, 0 }                  /* data version */
};

#if !UCONFIG_NO_BREAK_ITERATION

// A wrapper for both BytesTrieBuilder and UCharsTrieBuilder.
// may want to put this somewhere in ICU, as it could be useful outside
// of this tool?
class DataDict {
private:
    BytesTrieBuilder *bt;
    UCharsTrieBuilder *ut;
    UChar32 transformConstant;
    int32_t transformType;
public:
    // constructs a new data dictionary. if there is an error, 
    // it will be returned in status
    // isBytesTrie != 0 will produce a BytesTrieBuilder,
    // isBytesTrie == 0 will produce a UCharsTrieBuilder
    DataDict(UBool isBytesTrie, UErrorCode &status) : bt(NULL), ut(NULL), 
        transformConstant(0), transformType(DictionaryData::TRANSFORM_NONE) {
        if (isBytesTrie) {
            bt = new BytesTrieBuilder(status);
        } else {
            ut = new UCharsTrieBuilder(status);
        }
    }

    ~DataDict() {
        delete bt;
        delete ut;
    }

private:
    char transform(UChar32 c, UErrorCode &status) {
        if (transformType == DictionaryData::TRANSFORM_TYPE_OFFSET) {
            if (c == 0x200D) { return (char)0xFF; }
            else if (c == 0x200C) { return (char)0xFE; }
            int32_t delta = c - transformConstant;
            if (delta < 0 || 0xFD < delta) {
                fprintf(stderr, "Codepoint U+%04lx out of range for --transform offset-%04lx!\n",
                        (long)c, (long)transformConstant);
                exit(U_ILLEGAL_ARGUMENT_ERROR); // TODO: should return and print the line number
            }
            return (char)delta;
        } else { // no such transform type 
            status = U_INTERNAL_PROGRAM_ERROR;
            return (char)c; // it should be noted this transform type will not generally work
        }
    }

    void transform(const UnicodeString &word, CharString &buf, UErrorCode &errorCode) {
        UChar32 c = 0;
        int32_t len = word.length();
        for (int32_t i = 0; i < len; i += U16_LENGTH(c)) {
            c = word.char32At(i);
            buf.append(transform(c, errorCode), errorCode);
        }
    }

public:
    // sets the desired transformation data.
    // should be populated from a command line argument
    // so far the only acceptable format is offset-<hex constant>
    // eventually others (mask-<hex constant>?) may be enabled
    // more complex functions may be more difficult
    void setTransform(const char *t) {
        if (strncmp(t, "offset-", 7) == 0) {
            char *end;
            unsigned long base = uprv_strtoul(t + 7, &end, 16);
            if (end == (t + 7) || *end != 0 || base > 0x10FF80) {
                fprintf(stderr, "Syntax for offset value in --transform offset-%s invalid!\n", t + 7);
                usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
            }
            transformType = DictionaryData::TRANSFORM_TYPE_OFFSET;
            transformConstant = (UChar32)base;
        }
        else {
            fprintf(stderr, "Invalid transform specified: %s\n", t);
            usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
        }
    }

    // add a word to the trie
    void addWord(const UnicodeString &word, int32_t value, UErrorCode &status) {
        if (bt) {
            CharString buf;
            transform(word, buf, status);
            bt->add(buf.toStringPiece(), value, status);
        }
        if (ut) { ut->add(word, value, status); }
    }

    // if we are a bytestrie, give back the StringPiece representing the serialized version of us
    StringPiece serializeBytes(UErrorCode &status) {
        return bt->buildStringPiece(USTRINGTRIE_BUILD_SMALL, status);
    }

    // if we are a ucharstrie, produce the UnicodeString representing the serialized version of us
    void serializeUChars(UnicodeString &s, UErrorCode &status) {
        ut->buildUnicodeString(USTRINGTRIE_BUILD_SMALL, s, status);
    }

    int32_t getTransform() {
        return (int32_t)(transformType | transformConstant); 
    }
};
#endif

static const UChar LINEFEED_CHARACTER = 0x000A;
static const UChar CARRIAGE_RETURN_CHARACTER = 0x000D;

static UBool readLine(UCHARBUF *f, UnicodeString &fileLine, IcuToolErrorCode &errorCode) {
    int32_t lineLength;
    const UChar *line = ucbuf_readline(f, &lineLength, errorCode);
    if(line == NULL || errorCode.isFailure()) { return FALSE; }
    // Strip trailing CR/LF, comments, and spaces.
    const UChar *comment = u_memchr(line, 0x23, lineLength);  // '#'
    if(comment != NULL) {
        lineLength = (int32_t)(comment - line);
    } else {
        while(lineLength > 0 && (line[lineLength - 1] == CARRIAGE_RETURN_CHARACTER || line[lineLength - 1] == LINEFEED_CHARACTER)) { --lineLength; }
    }
    while(lineLength > 0 && u_isspace(line[lineLength - 1])) { --lineLength; }
    fileLine.setTo(FALSE, line, lineLength);
    return TRUE;
}

//----------------------------------------------------------------------------
//
//  main      for gendict
//
//----------------------------------------------------------------------------
int  main(int argc, char **argv) {
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

    if(options[ARG_HELP].doesOccur || options[ARG_QMARK].doesOccur) {
        //  -? or -h for help.
        usageAndDie(U_ZERO_ERROR);
    }

    UBool verbose = options[ARG_VERBOSE].doesOccur;
    UBool quiet = options[ARG_QUIET].doesOccur;

    if (argc < 3) {
        fprintf(stderr, "input and output file must both be specified.\n");
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }
    const char *outFileName  = argv[2];
    const char *wordFileName = argv[1];

    startTime = uprv_getRawUTCtime(); // initialize start timer

	if (options[ARG_ICUDATADIR].doesOccur) {
        u_setDataDirectory(options[ARG_ICUDATADIR].value);
    }

    const char *copyright = NULL;
    if (options[ARG_COPYRIGHT].doesOccur) {
        copyright = U_COPYRIGHT_STRING;
    }

    if (options[ARG_UCHARS].doesOccur == options[ARG_BYTES].doesOccur) {
        fprintf(stderr, "you must specify exactly one type of trie to output!\n");
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }
    UBool isBytesTrie = options[ARG_BYTES].doesOccur;
    if (isBytesTrie != options[ARG_TRANSFORM].doesOccur) {
        fprintf(stderr, "you must provide a transformation for a bytes trie, and must not provide one for a uchars trie!\n");
        usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }

    IcuToolErrorCode status("gendict/main()");

#if UCONFIG_NO_BREAK_ITERATION || UCONFIG_NO_FILE_IO
    const char* outDir=NULL;

    UNewDataMemory *pData;
    char msg[1024];
    UErrorCode tempstatus = U_ZERO_ERROR;

    /* write message with just the name */ // potential for a buffer overflow here...
    sprintf(msg, "gendict writes dummy %s because of UCONFIG_NO_BREAK_ITERATION and/or UCONFIG_NO_FILE_IO, see uconfig.h", outFileName);
    fprintf(stderr, "%s\n", msg);

    /* write the dummy data file */
    pData = udata_create(outDir, NULL, outFileName, &dataInfo, NULL, &tempstatus);
    udata_writeBlock(pData, msg, strlen(msg));
    udata_finish(pData, &tempstatus);
    return (int)tempstatus;

#else
    //  Read in the dictionary source file
    if (verbose) { printf("Opening file %s...\n", wordFileName); }
    const char *codepage = "UTF-8";
    LocalUCHARBUFPointer f(ucbuf_open(wordFileName, &codepage, TRUE, FALSE, status));
    if (status.isFailure()) {
        fprintf(stderr, "error opening input file: ICU Error \"%s\"\n", status.errorName());
        exit(status.reset());
    }
    if (verbose) { printf("Initializing dictionary builder of type %s...\n", (isBytesTrie ? "BytesTrie" : "UCharsTrie")); }
    DataDict dict(isBytesTrie, status);
    if (status.isFailure()) {
        fprintf(stderr, "new DataDict: ICU Error \"%s\"\n", status.errorName());
        exit(status.reset());
    }
    if (options[ARG_TRANSFORM].doesOccur) {
        dict.setTransform(options[ARG_TRANSFORM].value);
    }

    UnicodeString fileLine;
    if (verbose) { puts("Adding words to dictionary..."); }
    UBool hasValues = FALSE;
    UBool hasValuelessContents = FALSE;
    int lineCount = 0;
    int wordCount = 0;
    int minlen = 255;
    int maxlen = 0;
    UBool isOk = TRUE;
    while (readLine(f.getAlias(), fileLine, status)) {
        lineCount++;
        if (fileLine.isEmpty()) continue;
 
        // Parse word [spaces value].
        int32_t keyLen;
        for (keyLen = 0; keyLen < fileLine.length() && !u_isspace(fileLine[keyLen]); ++keyLen) {}
        if (keyLen == 0) {
            fprintf(stderr, "Error: no word on line %i!\n", lineCount);
            isOk = FALSE;
            continue;
        }
        int32_t valueStart;
        for (valueStart = keyLen;
            valueStart < fileLine.length() && u_isspace(fileLine[valueStart]);
            ++valueStart) {}

        if (keyLen < valueStart) {
            int32_t valueLength = fileLine.length() - valueStart;
            if (valueLength > 15) {
                fprintf(stderr, "Error: value too long on line %i!\n", lineCount);
                isOk = FALSE;
                continue;
            }
            char s[16];
            fileLine.extract(valueStart, valueLength, s, 16, US_INV);
            char *end;
            unsigned long value = uprv_strtoul(s, &end, 0);
            if (end == s || *end != 0 || (int32_t)uprv_strlen(s) != valueLength || value > 0xffffffff) {
                fprintf(stderr, "Error: value syntax error or value too large on line %i!\n", lineCount);
                isOk = FALSE;
                continue;
            }
            dict.addWord(fileLine.tempSubString(0, keyLen), (int32_t)value, status);
            hasValues = TRUE;
            wordCount++;
            if (keyLen < minlen) minlen = keyLen;
            if (keyLen > maxlen) maxlen = keyLen;
        } else {
            dict.addWord(fileLine.tempSubString(0, keyLen), 0, status);
            hasValuelessContents = TRUE;
            wordCount++;
            if (keyLen < minlen) minlen = keyLen;
            if (keyLen > maxlen) maxlen = keyLen;
        }

        if (status.isFailure()) {
            fprintf(stderr, "ICU Error \"%s\": Failed to add word to trie at input line %d in input file\n",
                status.errorName(), lineCount);
            exit(status.reset());
        }
    }
    if (verbose) { printf("Processed %d lines, added %d words, minlen %d, maxlen %d\n", lineCount, wordCount, minlen, maxlen); }

    if (!isOk && status.isSuccess()) {
        status.set(U_ILLEGAL_ARGUMENT_ERROR);
    }
    if (hasValues && hasValuelessContents) {
        fprintf(stderr, "warning: file contained both valued and unvalued strings!\n");
    }

    if (verbose) { printf("Serializing data...isBytesTrie? %d\n", isBytesTrie); }
    int32_t outDataSize;
    const void *outData;
    UnicodeString usp;
    if (isBytesTrie) {
        StringPiece sp = dict.serializeBytes(status);
        outDataSize = sp.size();
        outData = sp.data();
    } else {
        dict.serializeUChars(usp, status);
        outDataSize = usp.length() * U_SIZEOF_UCHAR;
        outData = usp.getBuffer();
    }
    if (status.isFailure()) {
        fprintf(stderr, "gendict: got failure of type %s while serializing, if U_ILLEGAL_ARGUMENT_ERROR possibly due to duplicate dictionary entries\n", status.errorName());
        exit(status.reset());
    }
    if (verbose) { puts("Opening output file..."); }
    UNewDataMemory *pData = udata_create(NULL, NULL, outFileName, &dataInfo, copyright, status);
    if (status.isFailure()) {
        fprintf(stderr, "gendict: could not open output file \"%s\", \"%s\"\n", outFileName, status.errorName());
        exit(status.reset());
    }

    if (verbose) { puts("Writing to output file..."); }
    int32_t indexes[DictionaryData::IX_COUNT] = {
        DictionaryData::IX_COUNT * sizeof(int32_t), 0, 0, 0, 0, 0, 0, 0
    };
    int32_t size = outDataSize + indexes[DictionaryData::IX_STRING_TRIE_OFFSET];
    indexes[DictionaryData::IX_RESERVED1_OFFSET] = size;
    indexes[DictionaryData::IX_RESERVED2_OFFSET] = size;
    indexes[DictionaryData::IX_TOTAL_SIZE] = size;

    indexes[DictionaryData::IX_TRIE_TYPE] = isBytesTrie ? DictionaryData::TRIE_TYPE_BYTES : DictionaryData::TRIE_TYPE_UCHARS;
    if (hasValues) {
        indexes[DictionaryData::IX_TRIE_TYPE] |= DictionaryData::TRIE_HAS_VALUES;
    }

    indexes[DictionaryData::IX_TRANSFORM] = dict.getTransform();
    udata_writeBlock(pData, indexes, sizeof(indexes));
    udata_writeBlock(pData, outData, outDataSize);
    size_t bytesWritten = udata_finish(pData, status);
    if (status.isFailure()) {
        fprintf(stderr, "gendict: error \"%s\" writing the output file\n", status.errorName());
        exit(status.reset());
    }

    if (bytesWritten != (size_t)size) {
        fprintf(stderr, "Error writing to output file \"%s\"\n", outFileName);
        exit(U_INTERNAL_PROGRAM_ERROR);
    }

    if (!quiet) { printf("%s: done writing\t%s (%ds).\n", progName, outFileName, elapsedTime()); }

#ifdef TEST_GENDICT
    if (isBytesTrie) {
        BytesTrie::Iterator it(outData, outDataSize, status);
        while (it.hasNext()) {
            it.next(status);
            const StringPiece s = it.getString();
            int32_t val = it.getValue();
            printf("%s -> %i\n", s.data(), val);
        }
    } else {
        UCharsTrie::Iterator it((const UChar *)outData, outDataSize, status);
        while (it.hasNext()) {
            it.next(status);
            const UnicodeString s = it.getString();
            int32_t val = it.getValue();
            char tmp[1024];
            s.extract(0, s.length(), tmp, 1024);
            printf("%s -> %i\n", tmp, val);
        }
    }
#endif

    return 0;
#endif /* #if !UCONFIG_NO_BREAK_ITERATION */
}
