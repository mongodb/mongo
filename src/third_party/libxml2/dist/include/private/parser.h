#ifndef XML_PARSER_H_PRIVATE__
#define XML_PARSER_H_PRIVATE__

#include <limits.h>

#include <libxml/parser.h>
#include <libxml/xmlversion.h>

#define XML_INVALID_CHAR 0x200000

#define XML_MAX_URI_LENGTH 2000

/**
 * Set after xmlValidateDtdFinal was called.
 */
#define XML_VCTXT_DTD_VALIDATED (1u << 0)
/**
 * Set if the validation context is part of a parser context.
 */
#define XML_VCTXT_USE_PCTXT (1u << 1)
/**
 * Set if the validation is enabled.
 */
#define XML_VCTXT_VALIDATE (1u << 2)
/**
 * Set when parsing entities.
 */
#define XML_VCTXT_IN_ENTITY (1u << 3)

/*
 * TODO: Rename to avoid confusion with xmlParserInputFlags
 */
#define XML_INPUT_HAS_ENCODING      (1u << 0)
#define XML_INPUT_AUTO_ENCODING     (7u << 1)
#define XML_INPUT_AUTO_UTF8         (1u << 1)
#define XML_INPUT_AUTO_UTF16LE      (2u << 1)
#define XML_INPUT_AUTO_UTF16BE      (3u << 1)
#define XML_INPUT_AUTO_OTHER        (4u << 1)
#define XML_INPUT_USES_ENC_DECL     (1u << 4)
#define XML_INPUT_ENCODING_ERROR    (1u << 5)
#define XML_INPUT_PROGRESSIVE       (1u << 6)
#define XML_INPUT_MARKUP_DECL       (1u << 7)

#define PARSER_STOPPED(ctxt) ((ctxt)->disableSAX > 1)

#define PARSER_PROGRESSIVE(ctxt) \
    ((ctxt)->input->flags & XML_INPUT_PROGRESSIVE)

#define PARSER_IN_PE(ctxt) \
    (((ctxt)->input->entity != NULL) && \
     (((ctxt)->input->entity->etype == XML_INTERNAL_PARAMETER_ENTITY) || \
      ((ctxt)->input->entity->etype == XML_EXTERNAL_PARAMETER_ENTITY)))

#define PARSER_EXTERNAL(ctxt) \
    (((ctxt)->inSubset == 2) || \
     (((ctxt)->input->entity != NULL) && \
      ((ctxt)->input->entity->etype == XML_EXTERNAL_PARAMETER_ENTITY)))

/**
 * The parser tries to always have that amount of input ready.
 * One of the point is providing context when reporting errors.
 */
#define INPUT_CHUNK	250

struct _xmlAttrHashBucket {
    int index;
};

#define XML_SCAN_NC         1
#define XML_SCAN_NMTOKEN    2
#define XML_SCAN_OLD10      4

XML_HIDDEN const xmlChar *
xmlScanName(const xmlChar *buf, size_t maxSize, int flags);

XML_HIDDEN void
xmlCtxtVErr(xmlParserCtxt *ctxt, xmlNode *node, xmlErrorDomain domain,
            xmlParserErrors code, xmlErrorLevel level,
            const xmlChar *str1, const xmlChar *str2, const xmlChar *str3,
            int int1, const char *msg, va_list ap);
XML_HIDDEN void
xmlCtxtErr(xmlParserCtxt *ctxt, xmlNode *node, xmlErrorDomain domain,
           xmlParserErrors code, xmlErrorLevel level,
           const xmlChar *str1, const xmlChar *str2, const xmlChar *str3,
           int int1, const char *msg, ...);
XML_HIDDEN void
xmlFatalErr(xmlParserCtxt *ctxt, xmlParserErrors error, const char *info);
XML_HIDDEN void LIBXML_ATTR_FORMAT(3,0)
xmlWarningMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
              const char *msg, const xmlChar *str1, const xmlChar *str2);
XML_HIDDEN void
xmlCtxtErrIO(xmlParserCtxt *ctxt, int code, const char *uri);
XML_HIDDEN int
xmlCtxtIsCatastrophicError(xmlParserCtxt *ctxt);

XML_HIDDEN int
xmlParserGrow(xmlParserCtxt *ctxt);
XML_HIDDEN void
xmlParserShrink(xmlParserCtxt *ctxt);

XML_HIDDEN void
xmlDetectEncoding(xmlParserCtxt *ctxt);
XML_HIDDEN void
xmlSetDeclaredEncoding(xmlParserCtxt *ctxt, xmlChar *encoding);
XML_HIDDEN const xmlChar *
xmlGetActualEncoding(xmlParserCtxt *ctxt);

XML_HIDDEN int
nodePush(xmlParserCtxt *ctxt, xmlNode *value);
XML_HIDDEN xmlNode *
nodePop(xmlParserCtxt *ctxt);

XML_HIDDEN xmlParserNsData *
xmlParserNsCreate(void);
XML_HIDDEN void
xmlParserNsFree(xmlParserNsData *nsdb);
/*
 * These functions allow SAX handlers to attach extra data to namespaces
 * efficiently and should be made public.
 */
XML_HIDDEN int
xmlParserNsUpdateSax(xmlParserCtxt *ctxt, const xmlChar *prefix,
                     void *saxData);
XML_HIDDEN void *
xmlParserNsLookupSax(xmlParserCtxt *ctxt, const xmlChar *prefix);

XML_HIDDEN xmlParserInput *
xmlLoadResource(xmlParserCtxt *ctxt, const char *url, const char *publicId,
                xmlResourceType type);
XML_HIDDEN xmlParserInput *
xmlCtxtNewInputFromUrl(xmlParserCtxt *ctxt, const char *url,
                       const char *publicId, const char *encoding,
                       xmlParserInputFlags flags);
XML_HIDDEN xmlParserInput *
xmlCtxtNewInputFromMemory(xmlParserCtxt *ctxt, const char *url,
                          const void *mem, size_t size,
                          const char *encoding,
                          xmlParserInputFlags flags);
XML_HIDDEN xmlParserInput *
xmlCtxtNewInputFromString(xmlParserCtxt *ctxt, const char *url,
                          const char *str, const char *encoding,
                          xmlParserInputFlags flags);
XML_HIDDEN xmlParserInput *
xmlCtxtNewInputFromFd(xmlParserCtxt *ctxt, const char *filename, int fd,
                      const char *encoding, xmlParserInputFlags flags);
XML_HIDDEN xmlParserInput *
xmlCtxtNewInputFromIO(xmlParserCtxt *ctxt, const char *url,
                      xmlInputReadCallback ioRead,
                      xmlInputCloseCallback ioClose,
                      void *ioCtxt,
                      const char *encoding, xmlParserInputFlags flags);
XML_HIDDEN xmlParserInput *
xmlNewPushInput(const char *url, const char *chunk, int size);

XML_HIDDEN xmlChar *
xmlExpandEntitiesInAttValue(xmlParserCtxt *ctxt, const xmlChar *str,
                            int normalize);

XML_HIDDEN void
xmlParserCheckEOF(xmlParserCtxt *ctxt, xmlParserErrors code);

XML_HIDDEN void
xmlParserInputGetWindow(xmlParserInput *input, const xmlChar **startOut,
                        int *sizeInOut, int *offsetOut);

static XML_INLINE void
xmlSaturatedAdd(unsigned long *dst, unsigned long val) {
    if (val > ULONG_MAX - *dst)
        *dst = ULONG_MAX;
    else
        *dst += val;
}

static XML_INLINE void
xmlSaturatedAddSizeT(unsigned long *dst, size_t val) {
    if (val > ULONG_MAX - *dst)
        *dst = ULONG_MAX;
    else
        *dst += val;
}

#endif /* XML_PARSER_H_PRIVATE__ */
