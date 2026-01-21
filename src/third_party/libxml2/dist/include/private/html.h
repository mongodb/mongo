#ifndef XML_HTML_H_PRIVATE__
#define XML_HTML_H_PRIVATE__

#include <libxml/xmlversion.h>

#ifdef LIBXML_HTML_ENABLED

#define IS_WS_HTML(c) \
    (((c) == 0x20) || \
     (((c) >= 0x09) && ((c) <= 0x0D) && ((c) != 0x0B)))

#define DATA_RCDATA         1
#define DATA_RAWTEXT        2
#define DATA_PLAINTEXT      3
#define DATA_SCRIPT         4
#define DATA_SCRIPT_ESC1    5
#define DATA_SCRIPT_ESC2    6

typedef struct {
    size_t start;
    size_t end;
    size_t size;
} htmlMetaEncodingOffsets;

XML_HIDDEN xmlNode *
htmlCtxtParseContentInternal(xmlParserCtxt *ctxt, xmlParserInput *input);

XML_HIDDEN int
htmlParseContentType(const xmlChar *val, htmlMetaEncodingOffsets *off);

XML_HIDDEN void
htmlNodeDumpInternal(xmlOutputBuffer *buf, xmlNode *cur,
                     const char *encoding, int format);

#endif /* LIBXML_HTML_ENABLED */

#endif /* XML_HTML_H_PRIVATE__ */

