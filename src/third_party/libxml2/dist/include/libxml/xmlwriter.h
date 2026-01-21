/**
 * @file
 * 
 * @brief text writing API for XML
 * 
 * text writing API for XML
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Alfred Mickautsch
 */

#ifndef __XML_XMLWRITER_H__
#define __XML_XMLWRITER_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_WRITER_ENABLED

#include <stdarg.h>
#include <libxml/xmlIO.h>
#include <libxml/list.h>
#include <libxml/xmlstring.h>

#ifdef __cplusplus
extern "C" {
#endif

    /** Writer object */
    typedef struct _xmlTextWriter xmlTextWriter;
    typedef xmlTextWriter *xmlTextWriterPtr;

/*
 * Constructors & Destructor
 */
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriter(xmlOutputBuffer *out);
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriterFilename(const char *uri, int compression);
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriterMemory(xmlBuffer *buf, int compression);
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriterPushParser(xmlParserCtxt *ctxt, int compression);
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriterDoc(xmlDoc ** doc, int compression);
    XMLPUBFUN xmlTextWriter *
        xmlNewTextWriterTree(xmlDoc *doc, xmlNode *node,
                             int compression);
    XMLPUBFUN void xmlFreeTextWriter(xmlTextWriter *writer);

/*
 * Functions
 */


/*
 * Document
 */
    XMLPUBFUN int
        xmlTextWriterStartDocument(xmlTextWriter *writer,
                                   const char *version,
                                   const char *encoding,
                                   const char *standalone);
    XMLPUBFUN int xmlTextWriterEndDocument(xmlTextWriter *
                                                   writer);

/*
 * Comments
 */
    XMLPUBFUN int xmlTextWriterStartComment(xmlTextWriter *
                                                    writer);
    XMLPUBFUN int xmlTextWriterEndComment(xmlTextWriter *writer);
    XMLPUBFUN int
        xmlTextWriterWriteFormatComment(xmlTextWriter *writer,
                                        const char *format, ...)
					LIBXML_ATTR_FORMAT(2,3);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatComment(xmlTextWriter *writer,
                                         const char *format,
                                         va_list argptr)
					 LIBXML_ATTR_FORMAT(2,0);
    XMLPUBFUN int xmlTextWriterWriteComment(xmlTextWriter *
                                                    writer,
                                                    const xmlChar *
                                                    content);

/*
 * Elements
 */
    XMLPUBFUN int
        xmlTextWriterStartElement(xmlTextWriter *writer,
                                  const xmlChar * name);
    XMLPUBFUN int xmlTextWriterStartElementNS(xmlTextWriter *
                                                      writer,
                                                      const xmlChar *
                                                      prefix,
                                                      const xmlChar * name,
                                                      const xmlChar *
                                                      namespaceURI);
    XMLPUBFUN int xmlTextWriterEndElement(xmlTextWriter *writer);
    XMLPUBFUN int xmlTextWriterFullEndElement(xmlTextWriter *
                                                      writer);

/*
 * Elements conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatElement(xmlTextWriter *writer,
                                        const xmlChar * name,
                                        const char *format, ...)
					LIBXML_ATTR_FORMAT(3,4);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatElement(xmlTextWriter *writer,
                                         const xmlChar * name,
                                         const char *format,
                                         va_list argptr)
					 LIBXML_ATTR_FORMAT(3,0);
    XMLPUBFUN int xmlTextWriterWriteElement(xmlTextWriter *
                                                    writer,
                                                    const xmlChar * name,
                                                    const xmlChar *
                                                    content);
    XMLPUBFUN int
        xmlTextWriterWriteFormatElementNS(xmlTextWriter *writer,
                                          const xmlChar * prefix,
                                          const xmlChar * name,
                                          const xmlChar * namespaceURI,
                                          const char *format, ...)
					  LIBXML_ATTR_FORMAT(5,6);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatElementNS(xmlTextWriter *writer,
                                           const xmlChar * prefix,
                                           const xmlChar * name,
                                           const xmlChar * namespaceURI,
                                           const char *format,
                                           va_list argptr)
					   LIBXML_ATTR_FORMAT(5,0);
    XMLPUBFUN int xmlTextWriterWriteElementNS(xmlTextWriter *
                                                      writer,
                                                      const xmlChar *
                                                      prefix,
                                                      const xmlChar * name,
                                                      const xmlChar *
                                                      namespaceURI,
                                                      const xmlChar *
                                                      content);

/*
 * Text
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatRaw(xmlTextWriter *writer,
                                    const char *format, ...)
				    LIBXML_ATTR_FORMAT(2,3);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatRaw(xmlTextWriter *writer,
                                     const char *format, va_list argptr)
				     LIBXML_ATTR_FORMAT(2,0);
    XMLPUBFUN int
        xmlTextWriterWriteRawLen(xmlTextWriter *writer,
                                 const xmlChar * content, int len);
    XMLPUBFUN int
        xmlTextWriterWriteRaw(xmlTextWriter *writer,
                              const xmlChar * content);
    XMLPUBFUN int xmlTextWriterWriteFormatString(xmlTextWriter *
                                                         writer,
                                                         const char
                                                         *format, ...)
							 LIBXML_ATTR_FORMAT(2,3);
    XMLPUBFUN int xmlTextWriterWriteVFormatString(xmlTextWriter *
                                                          writer,
                                                          const char
                                                          *format,
                                                          va_list argptr)
							  LIBXML_ATTR_FORMAT(2,0);
    XMLPUBFUN int xmlTextWriterWriteString(xmlTextWriter *writer,
                                                   const xmlChar *
                                                   content);
    XMLPUBFUN int xmlTextWriterWriteBase64(xmlTextWriter *writer,
                                                   const char *data,
                                                   int start, int len);
    XMLPUBFUN int xmlTextWriterWriteBinHex(xmlTextWriter *writer,
                                                   const char *data,
                                                   int start, int len);

/*
 * Attributes
 */
    XMLPUBFUN int
        xmlTextWriterStartAttribute(xmlTextWriter *writer,
                                    const xmlChar * name);
    XMLPUBFUN int xmlTextWriterStartAttributeNS(xmlTextWriter *
                                                        writer,
                                                        const xmlChar *
                                                        prefix,
                                                        const xmlChar *
                                                        name,
                                                        const xmlChar *
                                                        namespaceURI);
    XMLPUBFUN int xmlTextWriterEndAttribute(xmlTextWriter *
                                                    writer);

/*
 * Attributes conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatAttribute(xmlTextWriter *writer,
                                          const xmlChar * name,
                                          const char *format, ...)
					  LIBXML_ATTR_FORMAT(3,4);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatAttribute(xmlTextWriter *writer,
                                           const xmlChar * name,
                                           const char *format,
                                           va_list argptr)
					   LIBXML_ATTR_FORMAT(3,0);
    XMLPUBFUN int xmlTextWriterWriteAttribute(xmlTextWriter *
                                                      writer,
                                                      const xmlChar * name,
                                                      const xmlChar *
                                                      content);
    XMLPUBFUN int
        xmlTextWriterWriteFormatAttributeNS(xmlTextWriter *writer,
                                            const xmlChar * prefix,
                                            const xmlChar * name,
                                            const xmlChar * namespaceURI,
                                            const char *format, ...)
					    LIBXML_ATTR_FORMAT(5,6);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatAttributeNS(xmlTextWriter *writer,
                                             const xmlChar * prefix,
                                             const xmlChar * name,
                                             const xmlChar * namespaceURI,
                                             const char *format,
                                             va_list argptr)
					     LIBXML_ATTR_FORMAT(5,0);
    XMLPUBFUN int xmlTextWriterWriteAttributeNS(xmlTextWriter *
                                                        writer,
                                                        const xmlChar *
                                                        prefix,
                                                        const xmlChar *
                                                        name,
                                                        const xmlChar *
                                                        namespaceURI,
                                                        const xmlChar *
                                                        content);

/*
 * PI's
 */
    XMLPUBFUN int
        xmlTextWriterStartPI(xmlTextWriter *writer,
                             const xmlChar * target);
    XMLPUBFUN int xmlTextWriterEndPI(xmlTextWriter *writer);

/*
 * PI conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatPI(xmlTextWriter *writer,
                                   const xmlChar * target,
                                   const char *format, ...)
				   LIBXML_ATTR_FORMAT(3,4);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatPI(xmlTextWriter *writer,
                                    const xmlChar * target,
                                    const char *format, va_list argptr)
				    LIBXML_ATTR_FORMAT(3,0);
    XMLPUBFUN int
        xmlTextWriterWritePI(xmlTextWriter *writer,
                             const xmlChar * target,
                             const xmlChar * content);

/**
 * This macro maps to #xmlTextWriterWritePI
 */
#define xmlTextWriterWriteProcessingInstruction xmlTextWriterWritePI

/*
 * CDATA
 */
    XMLPUBFUN int xmlTextWriterStartCDATA(xmlTextWriter *writer);
    XMLPUBFUN int xmlTextWriterEndCDATA(xmlTextWriter *writer);

/*
 * CDATA conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatCDATA(xmlTextWriter *writer,
                                      const char *format, ...)
				      LIBXML_ATTR_FORMAT(2,3);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatCDATA(xmlTextWriter *writer,
                                       const char *format, va_list argptr)
				       LIBXML_ATTR_FORMAT(2,0);
    XMLPUBFUN int
        xmlTextWriterWriteCDATA(xmlTextWriter *writer,
                                const xmlChar * content);

/*
 * DTD
 */
    XMLPUBFUN int
        xmlTextWriterStartDTD(xmlTextWriter *writer,
                              const xmlChar * name,
                              const xmlChar * pubid,
                              const xmlChar * sysid);
    XMLPUBFUN int xmlTextWriterEndDTD(xmlTextWriter *writer);

/*
 * DTD conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatDTD(xmlTextWriter *writer,
                                    const xmlChar * name,
                                    const xmlChar * pubid,
                                    const xmlChar * sysid,
                                    const char *format, ...)
				    LIBXML_ATTR_FORMAT(5,6);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatDTD(xmlTextWriter *writer,
                                     const xmlChar * name,
                                     const xmlChar * pubid,
                                     const xmlChar * sysid,
                                     const char *format, va_list argptr)
				     LIBXML_ATTR_FORMAT(5,0);
    XMLPUBFUN int
        xmlTextWriterWriteDTD(xmlTextWriter *writer,
                              const xmlChar * name,
                              const xmlChar * pubid,
                              const xmlChar * sysid,
                              const xmlChar * subset);

/**
 * this macro maps to #xmlTextWriterWriteDTD
 */
#define xmlTextWriterWriteDocType xmlTextWriterWriteDTD

/*
 * DTD element definition
 */
    XMLPUBFUN int
        xmlTextWriterStartDTDElement(xmlTextWriter *writer,
                                     const xmlChar * name);
    XMLPUBFUN int xmlTextWriterEndDTDElement(xmlTextWriter *
                                                     writer);

/*
 * DTD element definition conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatDTDElement(xmlTextWriter *writer,
                                           const xmlChar * name,
                                           const char *format, ...)
					   LIBXML_ATTR_FORMAT(3,4);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatDTDElement(xmlTextWriter *writer,
                                            const xmlChar * name,
                                            const char *format,
                                            va_list argptr)
					    LIBXML_ATTR_FORMAT(3,0);
    XMLPUBFUN int xmlTextWriterWriteDTDElement(xmlTextWriter *
                                                       writer,
                                                       const xmlChar *
                                                       name,
                                                       const xmlChar *
                                                       content);

/*
 * DTD attribute list definition
 */
    XMLPUBFUN int
        xmlTextWriterStartDTDAttlist(xmlTextWriter *writer,
                                     const xmlChar * name);
    XMLPUBFUN int xmlTextWriterEndDTDAttlist(xmlTextWriter *
                                                     writer);

/*
 * DTD attribute list definition conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatDTDAttlist(xmlTextWriter *writer,
                                           const xmlChar * name,
                                           const char *format, ...)
					   LIBXML_ATTR_FORMAT(3,4);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatDTDAttlist(xmlTextWriter *writer,
                                            const xmlChar * name,
                                            const char *format,
                                            va_list argptr)
					    LIBXML_ATTR_FORMAT(3,0);
    XMLPUBFUN int xmlTextWriterWriteDTDAttlist(xmlTextWriter *
                                                       writer,
                                                       const xmlChar *
                                                       name,
                                                       const xmlChar *
                                                       content);

/*
 * DTD entity definition
 */
    XMLPUBFUN int
        xmlTextWriterStartDTDEntity(xmlTextWriter *writer,
                                    int pe, const xmlChar * name);
    XMLPUBFUN int xmlTextWriterEndDTDEntity(xmlTextWriter *
                                                    writer);

/*
 * DTD entity definition conveniency functions
 */
    XMLPUBFUN int
        xmlTextWriterWriteFormatDTDInternalEntity(xmlTextWriter *writer,
                                                  int pe,
                                                  const xmlChar * name,
                                                  const char *format, ...)
						  LIBXML_ATTR_FORMAT(4,5);
    XMLPUBFUN int
        xmlTextWriterWriteVFormatDTDInternalEntity(xmlTextWriter *writer,
                                                   int pe,
                                                   const xmlChar * name,
                                                   const char *format,
                                                   va_list argptr)
						   LIBXML_ATTR_FORMAT(4,0);
    XMLPUBFUN int
        xmlTextWriterWriteDTDInternalEntity(xmlTextWriter *writer,
                                            int pe,
                                            const xmlChar * name,
                                            const xmlChar * content);
    XMLPUBFUN int
        xmlTextWriterWriteDTDExternalEntity(xmlTextWriter *writer,
                                            int pe,
                                            const xmlChar * name,
                                            const xmlChar * pubid,
                                            const xmlChar * sysid,
                                            const xmlChar * ndataid);
    XMLPUBFUN int
        xmlTextWriterWriteDTDExternalEntityContents(xmlTextWriter *
                                                    writer,
                                                    const xmlChar * pubid,
                                                    const xmlChar * sysid,
                                                    const xmlChar *
                                                    ndataid);
    XMLPUBFUN int xmlTextWriterWriteDTDEntity(xmlTextWriter *
                                                      writer, int pe,
                                                      const xmlChar * name,
                                                      const xmlChar *
                                                      pubid,
                                                      const xmlChar *
                                                      sysid,
                                                      const xmlChar *
                                                      ndataid,
                                                      const xmlChar *
                                                      content);

/*
 * DTD notation definition
 */
    XMLPUBFUN int
        xmlTextWriterWriteDTDNotation(xmlTextWriter *writer,
                                      const xmlChar * name,
                                      const xmlChar * pubid,
                                      const xmlChar * sysid);

/*
 * Indentation
 */
    XMLPUBFUN int
        xmlTextWriterSetIndent(xmlTextWriter *writer, int indent);
    XMLPUBFUN int
        xmlTextWriterSetIndentString(xmlTextWriter *writer,
                                     const xmlChar * str);

    XMLPUBFUN int
        xmlTextWriterSetQuoteChar(xmlTextWriter *writer, xmlChar quotechar);


/*
 * misc
 */
    XMLPUBFUN int xmlTextWriterFlush(xmlTextWriter *writer);
    XMLPUBFUN int xmlTextWriterClose(xmlTextWriter *writer);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_WRITER_ENABLED */

#endif                          /* __XML_XMLWRITER_H__ */
