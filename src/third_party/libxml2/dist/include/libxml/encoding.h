/**
 * @file
 * 
 * @brief Character encoding conversion functions
 * 
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_CHAR_ENCODING_H__
#define __XML_CHAR_ENCODING_H__

#include <libxml/xmlversion.h>
#include <libxml/xmlerror.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backward compatibility
 */
/** @cond ignore */
#define UTF8Toisolat1 xmlUTF8ToIsolat1
#define isolat1ToUTF8 xmlIsolat1ToUTF8
/** @endcond */

/**
 * Encoding conversion errors
 */
typedef enum {
    /** Success */
    XML_ENC_ERR_SUCCESS     =  0,
    /** Internal or unclassified error */
    XML_ENC_ERR_INTERNAL    = -1,
    /** Invalid or untranslatable input sequence */
    XML_ENC_ERR_INPUT       = -2,
    /** Not enough space in output buffer */
    XML_ENC_ERR_SPACE       = -3,
    /** Out-of-memory error */
    XML_ENC_ERR_MEMORY      = -4
} xmlCharEncError;

/**
 * Predefined values for some standard encodings.
 */
typedef enum {
    /** No char encoding detected */
    XML_CHAR_ENCODING_ERROR=   -1,
    /** No char encoding detected */
    XML_CHAR_ENCODING_NONE=	0,
    /** UTF-8 */
    XML_CHAR_ENCODING_UTF8=	1,
    /** UTF-16 little endian */
    XML_CHAR_ENCODING_UTF16LE=	2,
    /** UTF-16 big endian */
    XML_CHAR_ENCODING_UTF16BE=	3,
    /** UCS-4 little endian */
    XML_CHAR_ENCODING_UCS4LE=	4,
    /** UCS-4 big endian */
    XML_CHAR_ENCODING_UCS4BE=	5,
    /** EBCDIC uh! */
    XML_CHAR_ENCODING_EBCDIC=	6,
    /** UCS-4 unusual ordering */
    XML_CHAR_ENCODING_UCS4_2143=7,
    /** UCS-4 unusual ordering */
    XML_CHAR_ENCODING_UCS4_3412=8,
    /** UCS-2 */
    XML_CHAR_ENCODING_UCS2=	9,
    /** ISO-8859-1 ISO Latin 1 */
    XML_CHAR_ENCODING_8859_1=	10,
    /** ISO-8859-2 ISO Latin 2 */
    XML_CHAR_ENCODING_8859_2=	11,
    /** ISO-8859-3 */
    XML_CHAR_ENCODING_8859_3=	12,
    /** ISO-8859-4 */
    XML_CHAR_ENCODING_8859_4=	13,
    /** ISO-8859-5 */
    XML_CHAR_ENCODING_8859_5=	14,
    /** ISO-8859-6 */
    XML_CHAR_ENCODING_8859_6=	15,
    /** ISO-8859-7 */
    XML_CHAR_ENCODING_8859_7=	16,
    /** ISO-8859-8 */
    XML_CHAR_ENCODING_8859_8=	17,
    /** ISO-8859-9 */
    XML_CHAR_ENCODING_8859_9=	18,
    /** ISO-2022-JP */
    XML_CHAR_ENCODING_2022_JP=  19,
    /** Shift_JIS */
    XML_CHAR_ENCODING_SHIFT_JIS=20,
    /** EUC-JP */
    XML_CHAR_ENCODING_EUC_JP=   21,
    /** pure ASCII */
    XML_CHAR_ENCODING_ASCII=    22,
    /** UTF-16 native, available since 2.14 */
    XML_CHAR_ENCODING_UTF16=	23,
    /** HTML (output only), available since 2.14 */
    XML_CHAR_ENCODING_HTML=	24,
    /** ISO-8859-10, available since 2.14 */
    XML_CHAR_ENCODING_8859_10=	25,
    /** ISO-8859-11, available since 2.14 */
    XML_CHAR_ENCODING_8859_11=	26,
    /** ISO-8859-13, available since 2.14 */
    XML_CHAR_ENCODING_8859_13=	27,
    /** ISO-8859-14, available since 2.14 */
    XML_CHAR_ENCODING_8859_14=	28,
    /** ISO-8859-15, available since 2.14 */
    XML_CHAR_ENCODING_8859_15=	29,
    /** ISO-8859-16, available since 2.14 */
    XML_CHAR_ENCODING_8859_16=	30,
    /** windows-1252, available since 2.15 */
    XML_CHAR_ENCODING_WINDOWS_1252 = 31
} xmlCharEncoding;

/**
 * Encoding conversion flags
 */
typedef enum {
    /** Create converter for input (conversion to UTF-8) */
    XML_ENC_INPUT = (1 << 0),
    /** Create converter for output (conversion from UTF-8) */
    XML_ENC_OUTPUT = (1 << 1),
    /** Use HTML5 mappings */
    XML_ENC_HTML = (1 << 2)
} xmlCharEncFlags;

/**
 * Convert characters to UTF-8.
 *
 * On success, the value of `inlen` after return is the number of
 * bytes consumed and `outlen` is the number of bytes produced.
 *
 * @param out  a pointer to an array of bytes to store the UTF-8 result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of chars in the original encoding
 * @param inlen  the length of `in`
 * @returns the number of bytes written or an xmlCharEncError code.
 */
typedef int (*xmlCharEncodingInputFunc)(unsigned char *out, int *outlen,
                                        const unsigned char *in, int *inlen);


/**
 * Convert characters from UTF-8.
 *
 * On success, the value of `inlen` after return is the number of
 * bytes consumed and `outlen` is the number of bytes produced.
 *
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of UTF-8 chars
 * @param inlen  the length of `in`
 * @returns the number of bytes written or an xmlCharEncError code.
 */
typedef int (*xmlCharEncodingOutputFunc)(unsigned char *out, int *outlen,
                                         const unsigned char *in, int *inlen);


/**
 * Convert between character encodings.
 *
 * The value of `inlen` after return is the number of bytes consumed
 * and `outlen` is the number of bytes produced.
 *
 * If the converter can consume partial multi-byte sequences, the
 * `flush` flag can be used to detect truncated sequences at EOF.
 * Otherwise, the flag can be ignored.
 *
 * @param vctxt  conversion context
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of input bytes
 * @param inlen  the length of `in`
 * @param flush  end of input
 * @returns an xmlCharEncError code.
 */
typedef xmlCharEncError
(*xmlCharEncConvFunc)(void *vctxt, unsigned char *out, int *outlen,
                      const unsigned char *in, int *inlen, int flush);

/**
 * Free a conversion context.
 *
 * @param vctxt  conversion context
 */
typedef void
(*xmlCharEncConvCtxtDtor)(void *vctxt);

/** Character encoding converter */
typedef struct _xmlCharEncodingHandler xmlCharEncodingHandler;
typedef xmlCharEncodingHandler *xmlCharEncodingHandlerPtr;
/**
 * A character encoding conversion handler for non UTF-8 encodings.
 *
 * This structure will be made private.
 */
struct _xmlCharEncodingHandler {
    char *name XML_DEPRECATED_MEMBER;
    union {
        xmlCharEncConvFunc func;
        xmlCharEncodingInputFunc legacyFunc;
    } input XML_DEPRECATED_MEMBER;
    union {
        xmlCharEncConvFunc func;
        xmlCharEncodingOutputFunc legacyFunc;
    } output XML_DEPRECATED_MEMBER;
    void *inputCtxt XML_DEPRECATED_MEMBER;
    void *outputCtxt XML_DEPRECATED_MEMBER;
    xmlCharEncConvCtxtDtor ctxtDtor XML_DEPRECATED_MEMBER;
    int flags XML_DEPRECATED_MEMBER;
};

/**
 * If this function returns XML_ERR_OK, it must fill the `out`
 * pointer with an encoding handler. The handler can be obtained
 * from #xmlCharEncNewCustomHandler.
 *
 * `flags` can contain XML_ENC_INPUT, XML_ENC_OUTPUT or both.
 *
 * @param vctxt  user data
 * @param name  encoding name
 * @param flags  bit mask of flags
 * @param out  pointer to resulting handler
 * @returns an xmlParserErrors code.
 */
typedef xmlParserErrors
(*xmlCharEncConvImpl)(void *vctxt, const char *name, xmlCharEncFlags flags,
                      xmlCharEncodingHandler **out);

/*
 * Interfaces for encoding handlers.
 */
XML_DEPRECATED
XMLPUBFUN void
	xmlInitCharEncodingHandlers	(void);
XML_DEPRECATED
XMLPUBFUN void
	xmlCleanupCharEncodingHandlers	(void);
XML_DEPRECATED
XMLPUBFUN void
	xmlRegisterCharEncodingHandler	(xmlCharEncodingHandler *handler);
XMLPUBFUN xmlParserErrors
	xmlLookupCharEncodingHandler	(xmlCharEncoding enc,
					 xmlCharEncodingHandler **out);
XMLPUBFUN xmlParserErrors
	xmlOpenCharEncodingHandler	(const char *name,
					 int output,
					 xmlCharEncodingHandler **out);
XMLPUBFUN xmlParserErrors
	xmlCreateCharEncodingHandler	(const char *name,
					 xmlCharEncFlags flags,
					 xmlCharEncConvImpl impl,
					 void *implCtxt,
					 xmlCharEncodingHandler **out);
XMLPUBFUN xmlCharEncodingHandler *
	xmlGetCharEncodingHandler	(xmlCharEncoding enc);
XMLPUBFUN xmlCharEncodingHandler *
	xmlFindCharEncodingHandler	(const char *name);
XML_DEPRECATED
XMLPUBFUN xmlCharEncodingHandler *
	xmlNewCharEncodingHandler	(const char *name,
					 xmlCharEncodingInputFunc input,
					 xmlCharEncodingOutputFunc output);
XMLPUBFUN xmlParserErrors
	xmlCharEncNewCustomHandler	(const char *name,
					 xmlCharEncConvFunc input,
					 xmlCharEncConvFunc output,
					 xmlCharEncConvCtxtDtor ctxtDtor,
					 void *inputCtxt,
					 void *outputCtxt,
					 xmlCharEncodingHandler **out);

/*
 * Interfaces for encoding names and aliases.
 */
XML_DEPRECATED
XMLPUBFUN int
	xmlAddEncodingAlias		(const char *name,
					 const char *alias);
XML_DEPRECATED
XMLPUBFUN int
	xmlDelEncodingAlias		(const char *alias);
XML_DEPRECATED
XMLPUBFUN const char *
	xmlGetEncodingAlias		(const char *alias);
XML_DEPRECATED
XMLPUBFUN void
	xmlCleanupEncodingAliases	(void);
XMLPUBFUN xmlCharEncoding
	xmlParseCharEncoding		(const char *name);
XMLPUBFUN const char *
	xmlGetCharEncodingName		(xmlCharEncoding enc);

/*
 * Interfaces directly used by the parsers.
 */
XMLPUBFUN xmlCharEncoding
	xmlDetectCharEncoding		(const unsigned char *in,
					 int len);

struct _xmlBuffer;
XMLPUBFUN int
	xmlCharEncOutFunc		(xmlCharEncodingHandler *handler,
					 struct _xmlBuffer *out,
					 struct _xmlBuffer *in);

XMLPUBFUN int
	xmlCharEncInFunc		(xmlCharEncodingHandler *handler,
					 struct _xmlBuffer *out,
					 struct _xmlBuffer *in);
XML_DEPRECATED
XMLPUBFUN int
	xmlCharEncFirstLine		(xmlCharEncodingHandler *handler,
					 struct _xmlBuffer *out,
					 struct _xmlBuffer *in);
XMLPUBFUN int
	xmlCharEncCloseFunc		(xmlCharEncodingHandler *handler);

/*
 * Export a few useful functions
 */
#ifdef LIBXML_OUTPUT_ENABLED
XMLPUBFUN int
	xmlUTF8ToIsolat1		(unsigned char *out,
					 int *outlen,
					 const unsigned char *in,
					 int *inlen);
#endif /* LIBXML_OUTPUT_ENABLED */
XMLPUBFUN int
	xmlIsolat1ToUTF8		(unsigned char *out,
					 int *outlen,
					 const unsigned char *in,
					 int *inlen);
#ifdef __cplusplus
}
#endif

#endif /* __XML_CHAR_ENCODING_H__ */
