#ifndef XML_BUF_H_PRIVATE__
#define XML_BUF_H_PRIVATE__

#include <libxml/parser.h>
#include <libxml/tree.h>

XML_HIDDEN xmlBuf *
xmlBufCreate(size_t size);
XML_HIDDEN xmlBuf *
xmlBufCreateMem(const xmlChar *mem, size_t size, int isStatic);
XML_HIDDEN void
xmlBufFree(xmlBuf *buf);

XML_HIDDEN void
xmlBufEmpty(xmlBuf *buf);

XML_HIDDEN int
xmlBufGrow(xmlBuf *buf, size_t len);

XML_HIDDEN int
xmlBufAdd(xmlBuf *buf, const xmlChar *str, size_t len);
XML_HIDDEN int
xmlBufCat(xmlBuf *buf, const xmlChar *str);

XML_HIDDEN size_t
xmlBufAvail(xmlBuf *buf);
XML_HIDDEN int
xmlBufIsEmpty(xmlBuf *buf);
XML_HIDDEN int
xmlBufAddLen(xmlBuf *buf, size_t len);

XML_HIDDEN xmlChar *
xmlBufDetach(xmlBuf *buf);

XML_HIDDEN xmlBuf *
xmlBufFromBuffer(xmlBuffer *buffer);
XML_HIDDEN int
xmlBufBackToBuffer(xmlBuf *buf, xmlBuffer *ret);

XML_HIDDEN int
xmlBufResetInput(xmlBuf *buf, xmlParserInput *input);
XML_HIDDEN int
xmlBufUpdateInput(xmlBuf *buf, xmlParserInput *input, size_t pos);

#endif /* XML_BUF_H_PRIVATE__ */
