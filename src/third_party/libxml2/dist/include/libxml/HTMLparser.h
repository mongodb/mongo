/**
 * @file
 * 
 * @brief HTML parser, doesn't support HTML5
 * 
 * This module orginally implemented an HTML parser based on the
 * (underspecified) HTML 4.0 spec. As of 2.14, the tokenizer
 * conforms to HTML5. Tree construction still follows a custom,
 * unspecified algorithm with many differences to HTML5.
 *
 * The parser defaults to ISO-8859-1, the default encoding of
 * HTTP/1.0.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __HTML_PARSER_H__
#define __HTML_PARSER_H__
#include <libxml/xmlversion.h>
#include <libxml/parser.h>

#ifdef LIBXML_HTML_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backward compatibility
 */
#define UTF8ToHtml htmlUTF8ToHtml
#define htmlDefaultSubelement(elt) elt->defaultsubelt
#define htmlElementAllowedHereDesc(parent,elt) \
	htmlElementAllowedHere((parent), (elt)->name)
#define htmlRequiredAttrs(elt) (elt)->attrs_req

/*
 * Most of the back-end structures from XML and HTML are shared.
 */
/** Same as xmlParserCtxt */
typedef xmlParserCtxt htmlParserCtxt;
typedef xmlParserCtxtPtr htmlParserCtxtPtr;
typedef xmlParserNodeInfo htmlParserNodeInfo;
/** Same as xmlSAXHandler */
typedef xmlSAXHandler htmlSAXHandler;
typedef xmlSAXHandlerPtr htmlSAXHandlerPtr;
/** Same as xmlParserInput */
typedef xmlParserInput htmlParserInput;
typedef xmlParserInputPtr htmlParserInputPtr;
typedef xmlDocPtr htmlDocPtr;
typedef xmlNodePtr htmlNodePtr;

/** @cond ignore */

/*
 * Internal description of an HTML element, representing HTML 4.01
 * and XHTML 1.0 (which share the same structure).
 */
typedef struct _htmlElemDesc htmlElemDesc;
typedef htmlElemDesc *htmlElemDescPtr;
struct _htmlElemDesc {
    const char *name;	/* The tag name */
    char startTag;      /* unused */
    char endTag;        /* Whether the end tag can be implied */
    char saveEndTag;    /* unused */
    char empty;         /* Is this an empty element ? */
    char depr;          /* unused */
    char dtd;           /* unused */
    char isinline;      /* is this a block 0 or inline 1 element */
    const char *desc;   /* the description */

    const char** subelts XML_DEPRECATED_MEMBER;
    const char* defaultsubelt XML_DEPRECATED_MEMBER;
    const char** attrs_opt XML_DEPRECATED_MEMBER;
    const char** attrs_depr XML_DEPRECATED_MEMBER;
    const char** attrs_req XML_DEPRECATED_MEMBER;

    int dataMode;
};

/*
 * Internal description of an HTML entity.
 */
typedef struct _htmlEntityDesc htmlEntityDesc;
typedef htmlEntityDesc *htmlEntityDescPtr;
struct _htmlEntityDesc {
    unsigned int value;	/* the UNICODE value for the character */
    const char *name;	/* The entity name */
    const char *desc;   /* the description */
};

#ifdef LIBXML_SAX1_ENABLED
/**
 * @deprecated Use #xmlSAX2InitHtmlDefaultSAXHandler
 */
XML_DEPRECATED
XMLPUBVAR const xmlSAXHandlerV1 htmlDefaultSAXHandler;
#endif /* LIBXML_SAX1_ENABLED */

/** @endcond */

/*
 * There is only few public functions.
 */
XML_DEPRECATED
XMLPUBFUN void
			htmlInitAutoClose	(void);
XML_DEPRECATED
XMLPUBFUN const htmlElemDesc *
			htmlTagLookup	(const xmlChar *tag);
XML_DEPRECATED
XMLPUBFUN const htmlEntityDesc *
			htmlEntityLookup(const xmlChar *name);
XML_DEPRECATED
XMLPUBFUN const htmlEntityDesc *
			htmlEntityValueLookup(unsigned int value);

XML_DEPRECATED
XMLPUBFUN int
			htmlIsAutoClosed(xmlDoc *doc,
					 xmlNode *elem);
XML_DEPRECATED
XMLPUBFUN int
			htmlAutoCloseTag(xmlDoc *doc,
					 const xmlChar *name,
					 xmlNode *elem);
XML_DEPRECATED
XMLPUBFUN const htmlEntityDesc *
			htmlParseEntityRef(htmlParserCtxt *ctxt,
					 const xmlChar **str);
XML_DEPRECATED
XMLPUBFUN int
			htmlParseCharRef(htmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
			htmlParseElement(htmlParserCtxt *ctxt);

XMLPUBFUN htmlParserCtxt *
			htmlNewParserCtxt(void);
XMLPUBFUN htmlParserCtxt *
			htmlNewSAXParserCtxt(const htmlSAXHandler *sax,
					     void *userData);

XMLPUBFUN htmlParserCtxt *
			htmlCreateMemoryParserCtxt(const char *buffer,
						   int size);

XMLPUBFUN int
			htmlParseDocument(htmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
			htmlSAXParseDoc	(const xmlChar *cur,
					 const char *encoding,
					 htmlSAXHandler *sax,
					 void *userData);
XMLPUBFUN xmlDoc *
			htmlParseDoc	(const xmlChar *cur,
					 const char *encoding);
XMLPUBFUN htmlParserCtxt *
			htmlCreateFileParserCtxt(const char *filename,
	                                         const char *encoding);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
			htmlSAXParseFile(const char *filename,
					 const char *encoding,
					 htmlSAXHandler *sax,
					 void *userData);
XMLPUBFUN xmlDoc *
			htmlParseFile	(const char *filename,
					 const char *encoding);
XML_DEPRECATED
XMLPUBFUN int
			htmlUTF8ToHtml	(unsigned char *out,
					 int *outlen,
					 const unsigned char *in,
					 int *inlen);
XML_DEPRECATED
XMLPUBFUN int
			htmlEncodeEntities(unsigned char *out,
					 int *outlen,
					 const unsigned char *in,
					 int *inlen, int quoteChar);
XML_DEPRECATED
XMLPUBFUN int
			htmlIsScriptAttribute(const xmlChar *name);
XML_DEPRECATED
XMLPUBFUN int
			htmlHandleOmittedElem(int val);

#ifdef LIBXML_PUSH_ENABLED
/*
 * Interfaces for the Push mode.
 */
XMLPUBFUN htmlParserCtxt *
			htmlCreatePushParserCtxt(htmlSAXHandler *sax,
						 void *user_data,
						 const char *chunk,
						 int size,
						 const char *filename,
						 xmlCharEncoding enc);
XMLPUBFUN int
			htmlParseChunk		(htmlParserCtxt *ctxt,
						 const char *chunk,
						 int size,
						 int terminate);
#endif /* LIBXML_PUSH_ENABLED */

XMLPUBFUN void
			htmlFreeParserCtxt	(htmlParserCtxt *ctxt);

/*
 * New set of simpler/more flexible APIs
 */

/**
 * This is the set of HTML parser options that can be passed to
 * #htmlReadDoc, #htmlCtxtSetOptions and other functions.
 */
typedef enum {
    /**
     * No effect as of 2.14.0.
     */
    HTML_PARSE_RECOVER = 1<<0,
    /**
     * Do not default to a doctype if none was found.
     */
    HTML_PARSE_NODEFDTD = 1<<2,
    /**
     * Disable error and warning reports to the error handlers.
     * Errors are still accessible with xmlCtxtGetLastError().
     */
    HTML_PARSE_NOERROR = 1<<5,
    /**
     * Disable warning reports.
     */
    HTML_PARSE_NOWARNING = 1<<6,
    /**
     * No effect.
     */
    HTML_PARSE_PEDANTIC = 1<<7,
    /**
     * Remove some text nodes containing only whitespace from the
     * result document. Which nodes are removed depends on a conservative
     * heuristic. The reindenting feature of the serialization code relies
     * on this option to be set when parsing. Use of this option is
     * DISCOURAGED.
     */
    HTML_PARSE_NOBLANKS = 1<<8,
    /**
     * No effect.
     */
    HTML_PARSE_NONET = 1<<11,
    /**
     * Do not add implied html, head or body elements.
     */
    HTML_PARSE_NOIMPLIED = 1<<13,
    /**
     * Store small strings directly in the node struct to save
     * memory.
    */
    HTML_PARSE_COMPACT = 1<<16,
    /**
     * Relax some internal limits. See XML_PARSE_HUGE in xmlParserOption.
     *
     * @since 2.14.0
     *
     * Use XML_PARSE_HUGE with older versions.
     */
    HTML_PARSE_HUGE = 1<<19,
    /**
     * Ignore the encoding in the HTML declaration. This option is
     * mostly unneeded these days. The only effect is to enforce
     * ISO-8859-1 decoding of ASCII-like data.
     */
    HTML_PARSE_IGNORE_ENC =1<<21,
    /**
     * Enable reporting of line numbers larger than 65535.
     *
     * @since 2.14.0
     *
     * Use XML_PARSE_BIG_LINES with older versions.
     */
    HTML_PARSE_BIG_LINES = 1<<22,
    /**
     * Make the tokenizer emit a SAX callback for each token. This results
     * in unbalanced invocations of startElement and endElement.
     *
     * For now, this is only usable to tokenize HTML5 with custom SAX
     * callbacks. A tree builder isn't implemented yet.
     *
     * @since 2.14.0
    */
    HTML_PARSE_HTML5 = 1<<26
} htmlParserOption;

XMLPUBFUN void
		htmlCtxtReset		(htmlParserCtxt *ctxt);
XMLPUBFUN int
		htmlCtxtSetOptions	(htmlParserCtxt *ctxt,
					 int options);
XMLPUBFUN int
		htmlCtxtUseOptions	(htmlParserCtxt *ctxt,
					 int options);
XMLPUBFUN xmlDoc *
		htmlReadDoc		(const xmlChar *cur,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlReadFile		(const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlReadMemory		(const char *buffer,
					 int size,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlReadFd		(int fd,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlReadIO		(xmlInputReadCallback ioread,
					 xmlInputCloseCallback ioclose,
					 void *ioctx,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlCtxtParseDocument	(htmlParserCtxt *ctxt,
					 xmlParserInput *input);
XMLPUBFUN xmlDoc *
		htmlCtxtReadDoc		(xmlParserCtxt *ctxt,
					 const xmlChar *cur,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlCtxtReadFile		(xmlParserCtxt *ctxt,
					 const char *filename,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlCtxtReadMemory		(xmlParserCtxt *ctxt,
					 const char *buffer,
					 int size,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlCtxtReadFd		(xmlParserCtxt *ctxt,
					 int fd,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		htmlCtxtReadIO		(xmlParserCtxt *ctxt,
					 xmlInputReadCallback ioread,
					 xmlInputCloseCallback ioclose,
					 void *ioctx,
					 const char *URL,
					 const char *encoding,
					 int options);

/**
 * deprecated content model
 */
typedef enum {
  HTML_NA = 0 ,		/* something we don't check at all */
  HTML_INVALID = 0x1 ,
  HTML_DEPRECATED = 0x2 ,
  HTML_VALID = 0x4 ,
  HTML_REQUIRED = 0xc /* VALID bit set so ( & HTML_VALID ) is TRUE */
} htmlStatus ;

/* Using htmlElemDesc rather than name here, to emphasise the fact
   that otherwise there's a lookup overhead
*/
XML_DEPRECATED
XMLPUBFUN htmlStatus htmlAttrAllowed(const htmlElemDesc*, const xmlChar*, int) ;
XML_DEPRECATED
XMLPUBFUN int htmlElementAllowedHere(const htmlElemDesc*, const xmlChar*) ;
XML_DEPRECATED
XMLPUBFUN htmlStatus htmlElementStatusHere(const htmlElemDesc*, const htmlElemDesc*) ;
XML_DEPRECATED
XMLPUBFUN htmlStatus htmlNodeStatus(xmlNode *, int) ;

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_HTML_ENABLED */
#endif /* __HTML_PARSER_H__ */
