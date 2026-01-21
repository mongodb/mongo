#ifndef XML_SAVE_H_PRIVATE__
#define XML_SAVE_H_PRIVATE__

#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#include <libxml/xmlversion.h>

#ifdef LIBXML_OUTPUT_ENABLED

XML_HIDDEN int
xmlSaveNotationDecl(xmlSaveCtxt *ctxt, xmlNotation *cur);
XML_HIDDEN int
xmlSaveNotationTable(xmlSaveCtxt *ctxt, xmlNotationTable *cur);

XML_HIDDEN void
xmlBufAttrSerializeTxtContent(xmlOutputBuffer *buf, xmlDoc *doc,
                              const xmlChar *string);
XML_HIDDEN void
xmlNsListDumpOutput(xmlOutputBuffer *buf, xmlNs *cur);

#endif /* LIBXML_OUTPUT_ENABLED */

#endif /* XML_SAVE_H_PRIVATE__ */

