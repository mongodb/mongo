/**
 * @file
 * 
 * @brief Internals routines and limits exported by the parser.
 * 
 * Except for some I/O-related functions, most of these macros and
 * functions are deprecated.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_PARSER_INTERNALS_H__
#define __XML_PARSER_INTERNALS_H__

#include <libxml/xmlversion.h>
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <libxml/chvalid.h>
#include <libxml/SAX2.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Push an input on the stack.
 *
 * @deprecated Use #xmlCtxtPushInput
 */
#define inputPush xmlCtxtPushInput
/**
 * Pop an input from the stack.
 *
 * @deprecated Use #xmlCtxtPushInput
 */
#define inputPop xmlCtxtPopInput
/**
 * Maximum element nesting depth (without XML_PARSE_HUGE).
 */
#define xmlParserMaxDepth 256

/**
 * Maximum size allowed for a single text node when building a tree.
 * This is not a limitation of the parser but a safety boundary feature,
 * use XML_PARSE_HUGE option to override it.
 * Introduced in 2.9.0
 */
#define XML_MAX_TEXT_LENGTH 10000000

/**
 * Maximum size allowed when XML_PARSE_HUGE is set.
 */
#define XML_MAX_HUGE_LENGTH 1000000000

/**
 * Maximum size allowed for a markup identifier.
 * This is not a limitation of the parser but a safety boundary feature,
 * use XML_PARSE_HUGE option to override it.
 * Note that with the use of parsing dictionaries overriding the limit
 * may result in more runtime memory usage in face of "unfriendly' content
 * Introduced in 2.9.0
 */
#define XML_MAX_NAME_LENGTH 50000

/**
 * Maximum size allowed by the parser for a dictionary by default
 * This is not a limitation of the parser but a safety boundary feature,
 * use XML_PARSE_HUGE option to override it.
 * Introduced in 2.9.0
 */
#define XML_MAX_DICTIONARY_LIMIT 100000000

/**
 * Maximum size allowed by the parser for ahead lookup
 * This is an upper boundary enforced by the parser to avoid bad
 * behaviour on "unfriendly' content
 * Introduced in 2.9.0
 */
#define XML_MAX_LOOKUP_LIMIT 10000000

/**
 * Identifiers can be longer, but this will be more costly
 * at runtime.
 */
#define XML_MAX_NAMELEN 100

/************************************************************************
 *									*
 * UNICODE version of the macros.					*
 *									*
 ************************************************************************/
/**
 * Macro to check the following production in the XML spec:
 *
 *     [2] Char ::= #x9 | #xA | #xD | [#x20...]
 *
 * any byte character in the accepted range
 *
 * @param c  an byte value (int)
 */
#define IS_BYTE_CHAR(c)	 xmlIsChar_ch(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD]
 *                      | [#x10000-#x10FFFF]
 *
 * any Unicode character, excluding the surrogate blocks, FFFE, and FFFF.
 *
 * @param c  an UNICODE value (int)
 */
#define IS_CHAR(c)   xmlIsCharQ(c)

/**
 * Behaves like IS_CHAR on single-byte value
 *
 * @param c  an xmlChar (usually an unsigned char)
 */
#define IS_CHAR_CH(c)  xmlIsChar_ch(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [3] S ::= (#x20 | #x9 | #xD | #xA)+
 * @param c  an UNICODE value (int)
 */
#define IS_BLANK(c)  xmlIsBlankQ(c)

/**
 * Behaviour same as IS_BLANK
 *
 * @param c  an xmlChar value (normally unsigned char)
 */
#define IS_BLANK_CH(c)  xmlIsBlank_ch(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [85] BaseChar ::= ... long list see REC ...
 * @param c  an UNICODE value (int)
 */
#define IS_BASECHAR(c) xmlIsBaseCharQ(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [88] Digit ::= ... long list see REC ...
 * @param c  an UNICODE value (int)
 */
#define IS_DIGIT(c) xmlIsDigitQ(c)

/**
 * Behaves like IS_DIGIT but with a single byte argument
 *
 * @param c  an xmlChar value (usually an unsigned char)
 */
#define IS_DIGIT_CH(c)  xmlIsDigit_ch(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [87] CombiningChar ::= ... long list see REC ...
 * @param c  an UNICODE value (int)
 */
#define IS_COMBINING(c) xmlIsCombiningQ(c)

/**
 * Always false (all combining chars > 0xff)
 *
 * @param c  an xmlChar (usually an unsigned char)
 */
#define IS_COMBINING_CH(c) 0

/**
 * Macro to check the following production in the XML spec:
 *
 *     [89] Extender ::= #x00B7 | #x02D0 | #x02D1 | #x0387 | #x0640 |
 *                       #x0E46 | #x0EC6 | #x3005 | [#x3031-#x3035] |
 *                       [#x309D-#x309E] | [#x30FC-#x30FE]
 * @param c  an UNICODE value (int)
 */
#define IS_EXTENDER(c) xmlIsExtenderQ(c)

/**
 * Behaves like IS_EXTENDER but with a single-byte argument
 *
 * @param c  an xmlChar value (usually an unsigned char)
 */
#define IS_EXTENDER_CH(c)  xmlIsExtender_ch(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [86] Ideographic ::= [#x4E00-#x9FA5] | #x3007 | [#x3021-#x3029]
 * @param c  an UNICODE value (int)
 */
#define IS_IDEOGRAPHIC(c) xmlIsIdeographicQ(c)

/**
 * Macro to check the following production in the XML spec:
 *
 *     [84] Letter ::= BaseChar | Ideographic
 * @param c  an UNICODE value (int)
 */
#define IS_LETTER(c) (IS_BASECHAR(c) || IS_IDEOGRAPHIC(c))

/**
 * Macro behaves like IS_LETTER, but only check base chars
 *
 * @param c  an xmlChar value (normally unsigned char)
 */
#define IS_LETTER_CH(c) xmlIsBaseChar_ch(c)

/**
 * Macro to check [a-zA-Z]
 *
 * @param c  an xmlChar value
 */
#define IS_ASCII_LETTER(c)	((0x61 <= ((c) | 0x20)) && \
                                 (((c) | 0x20) <= 0x7a))

/**
 * Macro to check [0-9]
 *
 * @param c  an xmlChar value
 */
#define IS_ASCII_DIGIT(c)	((0x30 <= (c)) && ((c) <= 0x39))

/**
 * Macro to check the following production in the XML spec:
 *
 *     [13] PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] |
 *                        [-'()+,./:=?;!*#@$_%]
 * @param c  an UNICODE value (int)
 */
#define IS_PUBIDCHAR(c)	xmlIsPubidCharQ(c)

/**
 * Same as IS_PUBIDCHAR but for single-byte value
 *
 * @param c  an xmlChar value (normally unsigned char)
 */
#define IS_PUBIDCHAR_CH(c) xmlIsPubidChar_ch(c)

/*
 * Global variables used for predefined strings.
 */
/** @cond ignore */
XMLPUBVAR const xmlChar xmlStringText[];
XMLPUBVAR const xmlChar xmlStringTextNoenc[];
XML_DEPRECATED
XMLPUBVAR const xmlChar xmlStringComment[];
/** @endcond */

XML_DEPRECATED
XMLPUBFUN int                   xmlIsLetter     (int c);

/*
 * Parser context.
 */
XMLPUBFUN xmlParserCtxt *
			xmlCreateFileParserCtxt	(const char *filename);
XMLPUBFUN xmlParserCtxt *
			xmlCreateURLParserCtxt	(const char *filename,
						 int options);
XMLPUBFUN xmlParserCtxt *
			xmlCreateMemoryParserCtxt(const char *buffer,
						 int size);
XML_DEPRECATED
XMLPUBFUN xmlParserCtxt *
			xmlCreateEntityParserCtxt(const xmlChar *URL,
						 const xmlChar *ID,
						 const xmlChar *base);
XMLPUBFUN void
			xmlCtxtErrMemory	(xmlParserCtxt *ctxt);
XMLPUBFUN int
			xmlSwitchEncoding	(xmlParserCtxt *ctxt,
						 xmlCharEncoding enc);
XMLPUBFUN int
			xmlSwitchEncodingName	(xmlParserCtxt *ctxt,
						 const char *encoding);
XMLPUBFUN int
			xmlSwitchToEncoding	(xmlParserCtxt *ctxt,
					 xmlCharEncodingHandler *handler);
XML_DEPRECATED
XMLPUBFUN int
			xmlSwitchInputEncoding	(xmlParserCtxt *ctxt,
						 xmlParserInput *input,
					 xmlCharEncodingHandler *handler);

/*
 * Input Streams.
 */
XMLPUBFUN xmlParserInput *
			xmlNewStringInputStream	(xmlParserCtxt *ctxt,
						 const xmlChar *buffer);
XML_DEPRECATED
XMLPUBFUN xmlParserInput *
			xmlNewEntityInputStream	(xmlParserCtxt *ctxt,
						 xmlEntity *entity);
XMLPUBFUN int
			xmlCtxtPushInput	(xmlParserCtxt *ctxt,
						 xmlParserInput *input);
XMLPUBFUN xmlParserInput *
			xmlCtxtPopInput		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
			xmlPushInput		(xmlParserCtxt *ctxt,
						 xmlParserInput *input);
XML_DEPRECATED
XMLPUBFUN xmlChar
			xmlPopInput		(xmlParserCtxt *ctxt);
XMLPUBFUN void
			xmlFreeInputStream	(xmlParserInput *input);
XMLPUBFUN xmlParserInput *
			xmlNewInputFromFile	(xmlParserCtxt *ctxt,
						 const char *filename);
XMLPUBFUN xmlParserInput *
			xmlNewInputStream	(xmlParserCtxt *ctxt);

/*
 * Namespaces.
 */
XMLPUBFUN xmlChar *
			xmlSplitQName		(xmlParserCtxt *ctxt,
						 const xmlChar *name,
						 xmlChar **prefix);

/*
 * Generic production rules.
 */
XML_DEPRECATED
XMLPUBFUN const xmlChar *
			xmlParseName		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseNmtoken		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseEntityValue	(xmlParserCtxt *ctxt,
						 xmlChar **orig);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseAttValue	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseSystemLiteral	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParsePubidLiteral	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseCharData	(xmlParserCtxt *ctxt,
						 int cdata);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseExternalID	(xmlParserCtxt *ctxt,
						 xmlChar **publicId,
						 int strict);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseComment		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN const xmlChar *
			xmlParsePITarget	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParsePI		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseNotationDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseEntityDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseDefaultDecl	(xmlParserCtxt *ctxt,
						 xmlChar **value);
XML_DEPRECATED
XMLPUBFUN xmlEnumeration *
			xmlParseNotationType	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlEnumeration *
			xmlParseEnumerationType	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseEnumeratedType	(xmlParserCtxt *ctxt,
						 xmlEnumeration **tree);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseAttributeType	(xmlParserCtxt *ctxt,
						 xmlEnumeration **tree);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseAttributeListDecl(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
			xmlParseElementMixedContentDecl
						(xmlParserCtxt *ctxt,
						 int inputchk);
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
			xmlParseElementChildrenContentDecl
						(xmlParserCtxt *ctxt,
						 int inputchk);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseElementContentDecl(xmlParserCtxt *ctxt,
						 const xmlChar *name,
						 xmlElementContent **result);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseElementDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseMarkupDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseCharRef		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlEntity *
			xmlParseEntityRef	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseReference	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParsePEReference	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseDocTypeDecl	(xmlParserCtxt *ctxt);
#ifdef LIBXML_SAX1_ENABLED
XML_DEPRECATED
XMLPUBFUN const xmlChar *
			xmlParseAttribute	(xmlParserCtxt *ctxt,
						 xmlChar **value);
XML_DEPRECATED
XMLPUBFUN const xmlChar *
			xmlParseStartTag	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseEndTag		(xmlParserCtxt *ctxt);
#endif /* LIBXML_SAX1_ENABLED */
XML_DEPRECATED
XMLPUBFUN void
			xmlParseCDSect		(xmlParserCtxt *ctxt);
XMLPUBFUN void
			xmlParseContent		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseElement		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseVersionNum	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseVersionInfo	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlChar *
			xmlParseEncName		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN const xmlChar *
			xmlParseEncodingDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
			xmlParseSDDecl		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseXMLDecl		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseTextDecl	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseMisc		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			xmlParseExternalSubset	(xmlParserCtxt *ctxt,
						 const xmlChar *publicId,
						 const xmlChar *systemId);

/** @cond ignore */
#define XML_SUBSTITUTE_NONE	0
#define XML_SUBSTITUTE_REF	1
#define XML_SUBSTITUTE_PEREF	2
#define XML_SUBSTITUTE_BOTH	3
/** @endcond */
XML_DEPRECATED
XMLPUBFUN xmlChar *
		xmlStringDecodeEntities		(xmlParserCtxt *ctxt,
						 const xmlChar *str,
						 int what,
						 xmlChar end,
						 xmlChar  end2,
						 xmlChar end3);
XML_DEPRECATED
XMLPUBFUN xmlChar *
		xmlStringLenDecodeEntities	(xmlParserCtxt *ctxt,
						 const xmlChar *str,
						 int len,
						 int what,
						 xmlChar end,
						 xmlChar  end2,
						 xmlChar end3);

/*
 * other commodities shared between parser.c and parserInternals.
 */
XML_DEPRECATED
XMLPUBFUN int			xmlSkipBlankChars	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int			xmlStringCurrentChar	(xmlParserCtxt *ctxt,
						 const xmlChar *cur,
						 int *len);
XML_DEPRECATED
XMLPUBFUN void			xmlParserHandlePEReference(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int			xmlCheckLanguageID	(const xmlChar *lang);

/*
 * Really core function shared with HTML parser.
 */
XML_DEPRECATED
XMLPUBFUN int			xmlCurrentChar		(xmlParserCtxt *ctxt,
						 int *len);
XML_DEPRECATED
XMLPUBFUN int		xmlCopyCharMultiByte	(xmlChar *out,
						 int val);
XML_DEPRECATED
XMLPUBFUN int			xmlCopyChar		(int len,
						 xmlChar *out,
						 int val);
XML_DEPRECATED
XMLPUBFUN void			xmlNextChar		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void			xmlParserInputShrink	(xmlParserInput *in);

#ifdef __cplusplus
}
#endif
#endif /* __XML_PARSER_INTERNALS_H__ */
