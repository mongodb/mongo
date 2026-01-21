/*
 * xmlreader.c: implements the xmlTextReader streaming node API
 *
 * NOTE:
 *   XmlTextReader.Normalization Property won't be supported, since
 *     it makes the parser non compliant to the XML recommendation
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

/*
 * TODOs:
 *   - XML Schemas validation
 */
#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_READER_ENABLED
#include <string.h> /* for memset() only ! */
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>

#include <libxml/xmlmemory.h>
#include <libxml/xmlIO.h>
#include <libxml/xmlreader.h>
#include <libxml/parserInternals.h>
#ifdef LIBXML_RELAXNG_ENABLED
#include <libxml/relaxng.h>
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
#include <libxml/xmlschemas.h>
#endif
#include <libxml/uri.h>
#ifdef LIBXML_XINCLUDE_ENABLED
#include <libxml/xinclude.h>
#endif
#ifdef LIBXML_PATTERN_ENABLED
#include <libxml/pattern.h>
#endif

#include "private/buf.h"
#include "private/error.h"
#include "private/io.h"
#include "private/memory.h"
#include "private/parser.h"
#include "private/tree.h"
#ifdef LIBXML_XINCLUDE_ENABLED
#include "private/xinclude.h"
#endif

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
/* Keeping free objects can hide memory errors. */
#define MAX_FREE_NODES 1
#else
#define MAX_FREE_NODES 100
#endif

#ifndef va_copy
  #ifdef __va_copy
    #define va_copy(dest, src) __va_copy(dest, src)
  #else
    #define va_copy(dest, src) memcpy(&(dest), &(src), sizeof(va_list))
  #endif
#endif

#define CHUNK_SIZE 512
/************************************************************************
 *									*
 *	The parser: maps the Text Reader API on top of the existing	*
 *		parsing routines building a tree			*
 *									*
 ************************************************************************/

#define XML_TEXTREADER_INPUT	1
#define XML_TEXTREADER_CTXT	2

typedef enum {
    XML_TEXTREADER_NONE = -1,
    XML_TEXTREADER_START= 0,
    XML_TEXTREADER_ELEMENT= 1,
    XML_TEXTREADER_END= 2,
    XML_TEXTREADER_EMPTY= 3,
    XML_TEXTREADER_BACKTRACK= 4,
    XML_TEXTREADER_DONE= 5,
    XML_TEXTREADER_ERROR= 6
} xmlTextReaderState;

typedef enum {
    XML_TEXTREADER_NOT_VALIDATE = 0,
    XML_TEXTREADER_VALIDATE_DTD = 1,
    XML_TEXTREADER_VALIDATE_RNG = 2,
    XML_TEXTREADER_VALIDATE_XSD = 4
} xmlTextReaderValidate;

struct _xmlTextReader {
    int				mode;	/* the parsing mode */
    xmlDocPtr			doc;    /* when walking an existing doc */
    xmlTextReaderValidate       validate;/* is there any validation */
    int				allocs;	/* what structure were deallocated */
    xmlTextReaderState		state;
    xmlParserCtxtPtr		ctxt;	/* the parser context */
    xmlSAXHandlerPtr		sax;	/* the parser SAX callbacks */
    xmlParserInputBufferPtr	input;	/* the input */
    startElementSAXFunc		startElement;/* initial SAX callbacks */
    endElementSAXFunc		endElement;  /* idem */
    startElementNsSAX2Func	startElementNs;/* idem */
    endElementNsSAX2Func	endElementNs;  /* idem */
    charactersSAXFunc		characters;
    cdataBlockSAXFunc		cdataBlock;
    unsigned int		base;	/* base of the segment in the input */
    unsigned int		cur;	/* current position in the input */
    xmlNodePtr			node;	/* current node */
    xmlNodePtr			curnode;/* current attribute node */
    int				depth;  /* depth of the current node */
    xmlNodePtr			faketext;/* fake xmlNs chld */
    int				preserve;/* preserve the resulting document */
    xmlBufPtr		        buffer; /* used to return const xmlChar * */
    xmlDictPtr			dict;	/* the context dictionary */

    /* entity stack when traversing entities content */
    xmlNodePtr         ent;          /* Current Entity Ref Node */
    int                entNr;        /* Depth of the entities stack */
    int                entMax;       /* Max depth of the entities stack */
    xmlNodePtr        *entTab;       /* array of entities */

    /* error handling */
    xmlTextReaderErrorFunc errorFunc;    /* callback function */
    void                  *errorFuncArg; /* callback function user argument */

#ifdef LIBXML_RELAXNG_ENABLED
    /* Handling of RelaxNG validation */
    xmlRelaxNGPtr          rngSchemas;	/* The Relax NG schemas */
    xmlRelaxNGValidCtxtPtr rngValidCtxt;/* The Relax NG validation context */
    int                    rngPreserveCtxt; /* 1 if the context was provided by the user */
    int                    rngValidErrors;/* The number of errors detected */
    xmlNodePtr             rngFullNode;	/* the node if RNG not progressive */
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
    /* Handling of Schemas validation */
    xmlSchemaPtr          xsdSchemas;	/* The Schemas schemas */
    xmlSchemaValidCtxtPtr xsdValidCtxt;/* The Schemas validation context */
    int                   xsdPreserveCtxt; /* 1 if the context was provided by the user */
    int                   xsdValidErrors;/* The number of errors detected */
    xmlSchemaSAXPlugPtr   xsdPlug;	/* the schemas plug in SAX pipeline */
#endif
#ifdef LIBXML_XINCLUDE_ENABLED
    /* Handling of XInclude processing */
    int                xinclude;	/* is xinclude asked for */
    xmlXIncludeCtxtPtr xincctxt;	/* the xinclude context */
    int                in_xinclude;	/* counts for xinclude */
#endif
#ifdef LIBXML_PATTERN_ENABLED
    int                patternNr;       /* number of preserve patterns */
    int                patternMax;      /* max preserve patterns */
    xmlPatternPtr     *patternTab;      /* array of preserve patterns */
#endif
    int                preserves;	/* level of preserves */
    int                parserFlags;	/* the set of options set */
    /* Structured error handling */
    xmlStructuredErrorFunc sErrorFunc;  /* callback function */

    xmlResourceLoader resourceLoader;
    void *resourceCtxt;
};

#define NODE_IS_EMPTY		0x1
#define NODE_IS_PRESERVED	0x2
#define NODE_IS_SPRESERVED	0x4

static int xmlTextReaderReadTree(xmlTextReaderPtr reader);
static int xmlTextReaderNextTree(xmlTextReaderPtr reader);

/**
 * Free a string if it is not owned by the "dict" dictionary in the
 * current scope
 *
 * @param str  a string
 */
#define DICT_FREE(str)						\
	if ((str) && ((!dict) ||				\
	    (xmlDictOwns(dict, (const xmlChar *)(str)) == 0)))	\
	    xmlFree((char *)(str));

static void xmlTextReaderFreeNode(xmlTextReaderPtr reader, xmlNodePtr cur);
static void xmlTextReaderFreeNodeList(xmlTextReaderPtr reader, xmlNodePtr cur);

static void
xmlTextReaderErr(xmlParserErrors code, const char *msg, ...) {
    va_list ap;
    int res;

    va_start(ap, msg);
    res = xmlVRaiseError(NULL, NULL, NULL, NULL, NULL,
                         XML_FROM_PARSER, code, XML_ERR_FATAL,
                         NULL, 0, NULL, NULL, NULL, 0, 0,
                         msg, ap);
    va_end(ap);
    if (res < 0)
        xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_PARSER, NULL);
}

static void
xmlTextReaderErrMemory(xmlTextReaderPtr reader) {
    if (reader == NULL) {
        xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_PARSER, NULL);
        return;
    }

    if (reader->ctxt != NULL)
        xmlCtxtErrMemory(reader->ctxt);
    else
        xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_PARSER, NULL);

    reader->mode = XML_TEXTREADER_MODE_ERROR;
    reader->state = XML_TEXTREADER_ERROR;
}

static xmlChar *
readerStrdup(xmlTextReaderPtr reader, const xmlChar *string) {
    xmlChar *copy;

    if (string == NULL)
        return(NULL);

    copy = xmlStrdup(string);
    if (copy == NULL)
        xmlTextReaderErrMemory(reader);

    return(copy);
}

static const xmlChar *
constString(xmlTextReaderPtr reader, const xmlChar *string) {
    const xmlChar *dictString;

    if (string == NULL)
        return(NULL);

    dictString = xmlDictLookup(reader->dict, string, -1);
    if (dictString == NULL)
        xmlTextReaderErrMemory(reader);

    return(dictString);
}

static const xmlChar *
constQString(xmlTextReaderPtr reader, const xmlChar *prefix,
             const xmlChar *name) {
    const xmlChar *dictString;

    if (name == NULL)
        return(NULL);

    dictString = xmlDictQLookup(reader->dict, prefix, name);
    if (dictString == NULL)
        xmlTextReaderErrMemory(reader);

    return(dictString);
}

/************************************************************************
 *									*
 *	Our own version of the freeing routines as we recycle nodes	*
 *									*
 ************************************************************************/

/**
 * Free a node.
 *
 * @param reader  the xmlTextReader used
 * @param cur  the node
 */
static void
xmlTextReaderFreeProp(xmlTextReaderPtr reader, xmlAttrPtr cur) {
    xmlDictPtr dict;

    if ((reader != NULL) && (reader->ctxt != NULL))
	dict = reader->ctxt->dict;
    else
        dict = NULL;
    if (cur == NULL) return;

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue((xmlNodePtr) cur);

    if (cur->children != NULL)
        xmlTextReaderFreeNodeList(reader, cur->children);

    if (cur->id != NULL) {
        /*
         * Operating in streaming mode, attr is gonna disappear
         */
        cur->id->attr = NULL;
        if (cur->id->name != NULL)
            DICT_FREE(cur->id->name);
        cur->id->name = cur->name;
        cur->name = NULL;
    } else {
        DICT_FREE(cur->name);
    }

    if ((reader != NULL) && (reader->ctxt != NULL) &&
        (reader->ctxt->freeAttrsNr < MAX_FREE_NODES)) {
        cur->next = reader->ctxt->freeAttrs;
	reader->ctxt->freeAttrs = cur;
	reader->ctxt->freeAttrsNr++;
    } else {
	xmlFree(cur);
    }
}

/**
 * Free a property and all its siblings, all the children are freed too.
 *
 * @param reader  the xmlTextReader used
 * @param cur  the first property in the list
 */
static void
xmlTextReaderFreePropList(xmlTextReaderPtr reader, xmlAttrPtr cur) {
    xmlAttrPtr next;

    while (cur != NULL) {
        next = cur->next;
        xmlTextReaderFreeProp(reader, cur);
	cur = next;
    }
}

/**
 * Free a node and all its siblings, this is a recursive behaviour, all
 * the children are freed too.
 *
 * @param reader  the xmlTextReader used
 * @param cur  the first node in the list
 */
static void
xmlTextReaderFreeNodeList(xmlTextReaderPtr reader, xmlNodePtr cur) {
    xmlNodePtr next;
    xmlNodePtr parent;
    xmlDictPtr dict;
    size_t depth = 0;

    if ((reader != NULL) && (reader->ctxt != NULL))
	dict = reader->ctxt->dict;
    else
        dict = NULL;
    if (cur == NULL) return;
    if (cur->type == XML_NAMESPACE_DECL) {
	xmlFreeNsList((xmlNsPtr) cur);
	return;
    }
    if ((cur->type == XML_DOCUMENT_NODE) ||
	(cur->type == XML_HTML_DOCUMENT_NODE)) {
	xmlFreeDoc((xmlDocPtr) cur);
	return;
    }
    while (1) {
        while ((cur->type != XML_DTD_NODE) &&
               (cur->type != XML_ENTITY_REF_NODE) &&
               (cur->children != NULL) &&
               (cur->children->parent == cur)) {
            cur = cur->children;
            depth += 1;
        }

        next = cur->next;
        parent = cur->parent;

	/* unroll to speed up freeing the document */
	if (cur->type != XML_DTD_NODE) {

	    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
		xmlDeregisterNodeDefaultValue(cur);

	    if (((cur->type == XML_ELEMENT_NODE) ||
		 (cur->type == XML_XINCLUDE_START) ||
		 (cur->type == XML_XINCLUDE_END)) &&
		(cur->properties != NULL))
		xmlTextReaderFreePropList(reader, cur->properties);
	    if ((cur->content != (xmlChar *) &(cur->properties)) &&
	        (cur->type != XML_ELEMENT_NODE) &&
		(cur->type != XML_XINCLUDE_START) &&
		(cur->type != XML_XINCLUDE_END) &&
		(cur->type != XML_ENTITY_REF_NODE)) {
		DICT_FREE(cur->content);
	    }
	    if (((cur->type == XML_ELEMENT_NODE) ||
	         (cur->type == XML_XINCLUDE_START) ||
		 (cur->type == XML_XINCLUDE_END)) &&
		(cur->nsDef != NULL))
		xmlFreeNsList(cur->nsDef);

	    /*
	     * we don't free element names here they are interned now
	     */
	    if ((cur->type != XML_TEXT_NODE) &&
		(cur->type != XML_COMMENT_NODE))
		DICT_FREE(cur->name);
	    if (((cur->type == XML_ELEMENT_NODE) ||
		 (cur->type == XML_TEXT_NODE)) &&
	        (reader != NULL) && (reader->ctxt != NULL) &&
		(reader->ctxt->freeElemsNr < MAX_FREE_NODES)) {
	        cur->next = reader->ctxt->freeElems;
		reader->ctxt->freeElems = cur;
		reader->ctxt->freeElemsNr++;
	    } else {
		xmlFree(cur);
	    }
	}

        if (next != NULL) {
	    cur = next;
        } else {
            if ((depth == 0) || (parent == NULL))
                break;
            depth -= 1;
            cur = parent;
            cur->children = NULL;
        }
    }
}

/**
 * Free a node, this is a recursive behaviour, all the children are freed too.
 * This doesn't unlink the child from the list, use #xmlUnlinkNode first.
 *
 * @param reader  the xmlTextReader used
 * @param cur  the node
 */
static void
xmlTextReaderFreeNode(xmlTextReaderPtr reader, xmlNodePtr cur) {
    xmlDictPtr dict;

    if ((reader != NULL) && (reader->ctxt != NULL))
	dict = reader->ctxt->dict;
    else
        dict = NULL;
    if (cur->type == XML_DTD_NODE) {
	xmlFreeDtd((xmlDtdPtr) cur);
	return;
    }
    if (cur->type == XML_NAMESPACE_DECL) {
	xmlFreeNs((xmlNsPtr) cur);
        return;
    }
    if (cur->type == XML_ATTRIBUTE_NODE) {
	xmlTextReaderFreeProp(reader, (xmlAttrPtr) cur);
	return;
    }

    if ((cur->children != NULL) &&
	(cur->type != XML_ENTITY_REF_NODE)) {
	if (cur->children->parent == cur)
	    xmlTextReaderFreeNodeList(reader, cur->children);
	cur->children = NULL;
    }

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue(cur);

    if (((cur->type == XML_ELEMENT_NODE) ||
	 (cur->type == XML_XINCLUDE_START) ||
	 (cur->type == XML_XINCLUDE_END)) &&
	(cur->properties != NULL))
	xmlTextReaderFreePropList(reader, cur->properties);
    if ((cur->content != (xmlChar *) &(cur->properties)) &&
        (cur->type != XML_ELEMENT_NODE) &&
	(cur->type != XML_XINCLUDE_START) &&
	(cur->type != XML_XINCLUDE_END) &&
	(cur->type != XML_ENTITY_REF_NODE)) {
	DICT_FREE(cur->content);
    }
    if (((cur->type == XML_ELEMENT_NODE) ||
	 (cur->type == XML_XINCLUDE_START) ||
	 (cur->type == XML_XINCLUDE_END)) &&
	(cur->nsDef != NULL))
	xmlFreeNsList(cur->nsDef);

    /*
     * we don't free names here they are interned now
     */
    if ((cur->type != XML_TEXT_NODE) &&
        (cur->type != XML_COMMENT_NODE))
	DICT_FREE(cur->name);

    if (((cur->type == XML_ELEMENT_NODE) ||
	 (cur->type == XML_TEXT_NODE)) &&
	(reader != NULL) && (reader->ctxt != NULL) &&
	(reader->ctxt->freeElemsNr < MAX_FREE_NODES)) {
	cur->next = reader->ctxt->freeElems;
	reader->ctxt->freeElems = cur;
	reader->ctxt->freeElemsNr++;
    } else {
	xmlFree(cur);
    }
}

/**
 * Free up all the structures used by a document, tree included.
 *
 * @param reader  the xmlTextReader used
 * @param cur  pointer to the document
 */
static void
xmlTextReaderFreeDoc(xmlTextReaderPtr reader, xmlDocPtr cur) {
    xmlDtdPtr extSubset, intSubset;

    if (cur == NULL) return;

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue((xmlNodePtr) cur);

    /*
     * Do this before freeing the children list to avoid ID lookups
     */
    if (cur->ids != NULL) xmlFreeIDTable((xmlIDTablePtr) cur->ids);
    cur->ids = NULL;
    if (cur->refs != NULL) xmlFreeRefTable((xmlRefTablePtr) cur->refs);
    cur->refs = NULL;
    extSubset = cur->extSubset;
    intSubset = cur->intSubset;
    if (intSubset == extSubset)
	extSubset = NULL;
    if (extSubset != NULL) {
	xmlUnlinkNode((xmlNodePtr) cur->extSubset);
	cur->extSubset = NULL;
	xmlFreeDtd(extSubset);
    }
    if (intSubset != NULL) {
	xmlUnlinkNode((xmlNodePtr) cur->intSubset);
	cur->intSubset = NULL;
	xmlFreeDtd(intSubset);
    }

    if (cur->children != NULL) xmlTextReaderFreeNodeList(reader, cur->children);

    if (cur->version != NULL) xmlFree(cur->version);
    if (cur->name != NULL) xmlFree((char *) cur->name);
    if (cur->encoding != NULL) xmlFree(cur->encoding);
    if (cur->oldNs != NULL) xmlFreeNsList(cur->oldNs);
    if (cur->URL != NULL) xmlFree(cur->URL);
    if (cur->dict != NULL) xmlDictFree(cur->dict);

    xmlFree(cur);
}

/************************************************************************
 *									*
 *			The reader core parser				*
 *									*
 ************************************************************************/

static void
xmlTextReaderStructuredRelay(void *userData, const xmlError *error)
{
    xmlTextReaderPtr reader = (xmlTextReaderPtr) userData;

    if (reader->sErrorFunc != NULL) {
        reader->sErrorFunc(reader->errorFuncArg, error);
    } else if (reader->errorFunc != NULL) {
        xmlParserSeverities severity;

        if ((error->domain == XML_FROM_VALID) ||
            (error->domain == XML_FROM_DTD)) {
            if (error->level == XML_ERR_WARNING)
                severity = XML_PARSER_SEVERITY_VALIDITY_WARNING;
            else
                severity = XML_PARSER_SEVERITY_VALIDITY_ERROR;
        } else {
            if (error->level == XML_ERR_WARNING)
                severity = XML_PARSER_SEVERITY_WARNING;
            else
                severity = XML_PARSER_SEVERITY_ERROR;
        }

        reader->errorFunc(reader->errorFuncArg, error->message, severity,
                          reader->ctxt);
    }
}

/**
 * Pushes a new entity reference node on top of the entities stack
 *
 * @param reader  the xmlTextReader used
 * @param value  the entity reference node
 * @returns -1 in case of error, the index in the stack otherwise
 */
static int
xmlTextReaderEntPush(xmlTextReaderPtr reader, xmlNodePtr value)
{
    if (reader->entNr >= reader->entMax) {
        xmlNodePtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(reader->entMax, sizeof(tmp[0]),
                                  10, XML_MAX_ITEMS);
        if (newSize < 0) {
            xmlTextReaderErrMemory(reader);
            return (-1);
        }
        tmp = xmlRealloc(reader->entTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlTextReaderErrMemory(reader);
            return (-1);
        }
        reader->entTab = tmp;
        reader->entMax = newSize;
    }
    reader->entTab[reader->entNr] = value;
    reader->ent = value;
    return (reader->entNr++);
}

/**
 * Pops the top element entity from the entities stack
 *
 * @param reader  the xmlTextReader used
 * @returns the entity just removed
 */
static xmlNodePtr
xmlTextReaderEntPop(xmlTextReaderPtr reader)
{
    xmlNodePtr ret;

    if (reader->entNr <= 0)
        return (NULL);
    reader->entNr--;
    if (reader->entNr > 0)
        reader->ent = reader->entTab[reader->entNr - 1];
    else
        reader->ent = NULL;
    ret = reader->entTab[reader->entNr];
    reader->entTab[reader->entNr] = NULL;
    return (ret);
}

/**
 * called when an opening tag has been processed.
 *
 * @param ctx  the user data (XML parser context)
 * @param fullname  The element name, including namespace prefix
 * @param atts  An array of name/value attributes pairs, NULL terminated
 */
static void
xmlTextReaderStartElement(void *ctx, const xmlChar *fullname,
	                  const xmlChar **atts) {
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->startElement != NULL)) {
	reader->startElement(ctx, fullname, atts);
	if ((ctxt->node != NULL) && (ctxt->input != NULL) &&
	    (ctxt->input->cur != NULL) && (ctxt->input->cur[0] == '/') &&
	    (ctxt->input->cur[1] == '>'))
	    ctxt->node->extra = NODE_IS_EMPTY;
    }
    if (reader != NULL)
	reader->state = XML_TEXTREADER_ELEMENT;
}

/**
 * called when an ending tag has been processed.
 *
 * @param ctx  the user data (XML parser context)
 * @param fullname  The element name, including namespace prefix
 */
static void
xmlTextReaderEndElement(void *ctx, const xmlChar *fullname) {
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->endElement != NULL)) {
	reader->endElement(ctx, fullname);
    }
}

/**
 * called when an opening tag has been processed.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 * @param nb_namespaces  number of namespace definitions on that node
 * @param namespaces  pointer to the array of prefix/URI pairs namespace definitions
 * @param nb_attributes  the number of attributes on that node
 * @param nb_defaulted  the number of defaulted attributes.
 * @param attributes  pointer to the array of (localname/prefix/URI/value/end)
 *               attribute values.
 */
static void
xmlTextReaderStartElementNs(void *ctx,
                      const xmlChar *localname,
		      const xmlChar *prefix,
		      const xmlChar *URI,
		      int nb_namespaces,
		      const xmlChar **namespaces,
		      int nb_attributes,
		      int nb_defaulted,
		      const xmlChar **attributes)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->startElementNs != NULL)) {
	reader->startElementNs(ctx, localname, prefix, URI, nb_namespaces,
	                       namespaces, nb_attributes, nb_defaulted,
			       attributes);
	if ((ctxt->node != NULL) && (ctxt->input != NULL) &&
	    (ctxt->input->cur != NULL) && (ctxt->input->cur[0] == '/') &&
	    (ctxt->input->cur[1] == '>'))
	    ctxt->node->extra = NODE_IS_EMPTY;
    }
    if (reader != NULL)
	reader->state = XML_TEXTREADER_ELEMENT;
}

/**
 * called when an ending tag has been processed.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 */
static void
xmlTextReaderEndElementNs(void *ctx,
                          const xmlChar * localname,
                          const xmlChar * prefix,
		          const xmlChar * URI)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->endElementNs != NULL)) {
	reader->endElementNs(ctx, localname, prefix, URI);
    }
}


/**
 * receiving some chars from the parser.
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 */
static void
xmlTextReaderCharacters(void *ctx, const xmlChar *ch, int len)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->characters != NULL)) {
	reader->characters(ctx, ch, len);
    }
}

/**
 * called when a pcdata block has been parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  The pcdata content
 * @param len  the block length
 */
static void
xmlTextReaderCDataBlock(void *ctx, const xmlChar *ch, int len)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlTextReaderPtr reader = ctxt->_private;

    if ((reader != NULL) && (reader->cdataBlock != NULL)) {
	reader->cdataBlock(ctx, ch, len);
    }
}

/**
 * Push data down the progressive parser until a significant callback
 * got raised.
 *
 * @param reader  the xmlTextReader used
 * @returns -1 in case of failure, 0 otherwise
 */
static int
xmlTextReaderPushData(xmlTextReaderPtr reader) {
    xmlBufPtr inbuf;
    int val, s;
    xmlTextReaderState oldstate;

    if ((reader->input == NULL) || (reader->input->buffer == NULL))
	return(-1);

    oldstate = reader->state;
    reader->state = XML_TEXTREADER_NONE;
    inbuf = reader->input->buffer;

    while (reader->state == XML_TEXTREADER_NONE) {
	if (xmlBufUse(inbuf) < reader->cur + CHUNK_SIZE) {
	    /*
	     * Refill the buffer unless we are at the end of the stream
	     */
	    if (reader->mode != XML_TEXTREADER_MODE_EOF) {
		val = xmlParserInputBufferRead(reader->input, 4096);
		if (val == 0) {
		    if (xmlBufUse(inbuf) == reader->cur) {
			reader->mode = XML_TEXTREADER_MODE_EOF;
                        break;
		    }
		} else if (val < 0) {
                    xmlCtxtErrIO(reader->ctxt, reader->input->error, NULL);
                    reader->mode = XML_TEXTREADER_MODE_ERROR;
                    reader->state = XML_TEXTREADER_ERROR;
                    return(-1);
		}

	    } else
		break;
	}
	/*
	 * parse by block of CHUNK_SIZE bytes, various tests show that
	 * it's the best tradeoff at least on a 1.2GH Duron
	 */
	if (xmlBufUse(inbuf) >= reader->cur + CHUNK_SIZE) {
	    val = xmlParseChunk(reader->ctxt,
                 (const char *) xmlBufContent(inbuf) + reader->cur,
                                CHUNK_SIZE, 0);
	    reader->cur += CHUNK_SIZE;
	    if (val != 0)
		reader->ctxt->wellFormed = 0;
	    if (reader->ctxt->wellFormed == 0)
		break;
	} else {
	    s = xmlBufUse(inbuf) - reader->cur;
	    val = xmlParseChunk(reader->ctxt,
		 (const char *) xmlBufContent(inbuf) + reader->cur,
			        s, 0);
	    reader->cur += s;
	    if (val != 0)
		reader->ctxt->wellFormed = 0;
	    break;
	}
    }
    reader->state = oldstate;

    /*
     * Discard the consumed input when needed and possible
     */
    if (reader->mode == XML_TEXTREADER_MODE_INTERACTIVE) {
        if (reader->cur > 80 /* LINE_LEN */) {
            val = xmlBufShrink(inbuf, reader->cur - 80);
            if (val >= 0) {
                reader->cur -= val;
            }
        }
    }

    /*
     * At the end of the stream signal that the work is done to the Push
     * parser.
     */
    else if (reader->mode == XML_TEXTREADER_MODE_EOF) {
	if (reader->state != XML_TEXTREADER_DONE) {
	    s = xmlBufUse(inbuf) - reader->cur;
	    val = xmlParseChunk(reader->ctxt,
		 (const char *) xmlBufContent(inbuf) + reader->cur,
			        s, 1);
	    reader->cur = xmlBufUse(inbuf);
	    reader->state  = XML_TEXTREADER_DONE;
	    if (val != 0) {
	        if (reader->ctxt->wellFormed)
		    reader->ctxt->wellFormed = 0;
		else
		    return(-1);
	    }
	}
    }
    if (reader->ctxt->wellFormed == 0) {
	reader->mode = XML_TEXTREADER_MODE_EOF;
        return(-1);
    }

    return(0);
}

#ifdef LIBXML_REGEXP_ENABLED
/**
 * Push the current node for validation
 *
 * @param reader  the xmlTextReader used
 */
static int
xmlTextReaderValidatePush(xmlTextReaderPtr reader) {
    xmlNodePtr node = reader->node;

#ifdef LIBXML_VALID_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_DTD) &&
        (reader->ctxt != NULL) && (reader->ctxt->validate == 1)) {
	if ((node->ns == NULL) || (node->ns->prefix == NULL)) {
	    reader->ctxt->valid &= xmlValidatePushElement(&reader->ctxt->vctxt,
				    reader->ctxt->myDoc, node, node->name);
	} else {
            xmlChar buf[50];
	    xmlChar *qname;

	    qname = xmlBuildQName(node->name, node->ns->prefix, buf, 50);
            if (qname == NULL) {
                xmlTextReaderErrMemory(reader);
                return(-1);
            }
	    reader->ctxt->valid &= xmlValidatePushElement(&reader->ctxt->vctxt,
				    reader->ctxt->myDoc, node, qname);
            if (qname != buf)
	        xmlFree(qname);
	}
        /*if (reader->ctxt->errNo == XML_ERR_NO_MEMORY) {
            reader->mode = XML_TEXTREADER_MODE_ERROR;
            reader->state = XML_TEXTREADER_ERROR;
            return(-1);
        }*/
    }
#endif /* LIBXML_VALID_ENABLED */
#ifdef LIBXML_RELAXNG_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_RNG) &&
               (reader->rngValidCtxt != NULL)) {
	int ret;

	if (reader->rngFullNode != NULL) return(0);
	ret = xmlRelaxNGValidatePushElement(reader->rngValidCtxt,
	                                    reader->ctxt->myDoc,
					    node);
	if (ret == 0) {
	    /*
	     * this element requires a full tree
	     */
	    node = xmlTextReaderExpand(reader);
	    if (node == NULL) {
	        ret = -1;
	    } else {
		ret = xmlRelaxNGValidateFullElement(reader->rngValidCtxt,
						    reader->ctxt->myDoc,
						    node);
		reader->rngFullNode = node;
	    }
	}
	if (ret != 1)
	    reader->rngValidErrors++;
    }
#endif

    return(0);
}

/**
 * Push some CData for validation
 *
 * @param reader  the xmlTextReader used
 * @param data  pointer to the CData
 * @param len  length of the CData block in bytes.
 */
static void
xmlTextReaderValidateCData(xmlTextReaderPtr reader,
                           const xmlChar *data, int len) {
#ifdef LIBXML_VALID_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_DTD) &&
        (reader->ctxt != NULL) && (reader->ctxt->validate == 1)) {
	reader->ctxt->valid &= xmlValidatePushCData(&reader->ctxt->vctxt,
	                                            data, len);
    }
#endif /* LIBXML_VALID_ENABLED */
#ifdef LIBXML_RELAXNG_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_RNG) &&
               (reader->rngValidCtxt != NULL)) {
	int ret;

	if (reader->rngFullNode != NULL) return;
	ret = xmlRelaxNGValidatePushCData(reader->rngValidCtxt, data, len);
	if (ret != 1)
	    reader->rngValidErrors++;
    }
#endif
}

/**
 * Pop the current node from validation
 *
 * @param reader  the xmlTextReader used
 */
static int
xmlTextReaderValidatePop(xmlTextReaderPtr reader) {
    xmlNodePtr node = reader->node;

#ifdef LIBXML_VALID_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_DTD) &&
        (reader->ctxt != NULL) && (reader->ctxt->validate == 1)) {
	if ((node->ns == NULL) || (node->ns->prefix == NULL)) {
	    reader->ctxt->valid &= xmlValidatePopElement(&reader->ctxt->vctxt,
				    reader->ctxt->myDoc, node, node->name);
	} else {
            xmlChar buf[50];
	    xmlChar *qname;

	    qname = xmlBuildQName(node->name, node->ns->prefix, buf, 50);
            if (qname == NULL) {
                xmlTextReaderErrMemory(reader);
                return(-1);
            }
	    reader->ctxt->valid &= xmlValidatePopElement(&reader->ctxt->vctxt,
				    reader->ctxt->myDoc, node, qname);
            if (qname != buf)
	        xmlFree(qname);
	}
        /*if (reader->ctxt->errNo == XML_ERR_NO_MEMORY) {
            reader->mode = XML_TEXTREADER_MODE_ERROR;
            reader->state = XML_TEXTREADER_ERROR;
            return(-1);
        }*/
    }
#endif /* LIBXML_VALID_ENABLED */
#ifdef LIBXML_RELAXNG_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_RNG) &&
               (reader->rngValidCtxt != NULL)) {
	int ret;

	if (reader->rngFullNode != NULL) {
	    if (node == reader->rngFullNode)
	        reader->rngFullNode = NULL;
	    return(0);
	}
	ret = xmlRelaxNGValidatePopElement(reader->rngValidCtxt,
	                                   reader->ctxt->myDoc,
					   node);
	if (ret != 1)
	    reader->rngValidErrors++;
    }
#endif

    return(0);
}

/**
 * Handle the validation when an entity reference is encountered and
 * entity substitution is not activated. As a result the parser interface
 * must walk through the entity and do the validation calls
 *
 * @param reader  the xmlTextReader used
 */
static int
xmlTextReaderValidateEntity(xmlTextReaderPtr reader) {
    xmlNodePtr oldnode = reader->node;
    xmlNodePtr node = reader->node;

    do {
	if (node->type == XML_ENTITY_REF_NODE) {
	    if ((node->children != NULL) &&
		(node->children->type == XML_ENTITY_DECL) &&
		(node->children->children != NULL)) {
		if (xmlTextReaderEntPush(reader, node) < 0) {
                    if (node == oldnode)
                        break;
                    goto skip_children;
                }
		node = node->children->children;
		continue;
	    } else {
		/*
		 * The error has probably been raised already.
		 */
		if (node == oldnode)
		    break;
                goto skip_children;
	    }
#ifdef LIBXML_REGEXP_ENABLED
	} else if (node->type == XML_ELEMENT_NODE) {
	    reader->node = node;
	    if (xmlTextReaderValidatePush(reader) < 0)
                return(-1);
	} else if ((node->type == XML_TEXT_NODE) ||
		   (node->type == XML_CDATA_SECTION_NODE)) {
            xmlTextReaderValidateCData(reader, node->content,
	                               xmlStrlen(node->content));
#endif
	}

	/*
	 * go to next node
	 */
	if (node->children != NULL) {
	    node = node->children;
	    continue;
	} else if (node->type == XML_ELEMENT_NODE) {
	    if (xmlTextReaderValidatePop(reader) < 0)
                return(-1);
	}
skip_children:
	if (node->next != NULL) {
	    node = node->next;
	    continue;
	}
	do {
	    node = node->parent;
	    if (node->type == XML_ELEMENT_NODE) {
	        xmlNodePtr tmp;
		if (reader->entNr == 0) {
		    while ((tmp = node->last) != NULL) {
			if ((tmp->extra & NODE_IS_PRESERVED) == 0) {
			    xmlUnlinkNode(tmp);
			    xmlTextReaderFreeNode(reader, tmp);
			} else
			    break;
		    }
		}
		reader->node = node;
		if (xmlTextReaderValidatePop(reader) < 0)
                    return(-1);
	    }
	    if ((node->type == XML_ENTITY_DECL) &&
		(reader->ent != NULL) && (reader->ent->children == node)) {
		node = xmlTextReaderEntPop(reader);
	    }
	    if (node == oldnode)
		break;
	    if (node->next != NULL) {
		node = node->next;
		break;
	    }
	} while ((node != NULL) && (node != oldnode));
    } while ((node != NULL) && (node != oldnode));
    reader->node = oldnode;

    return(0);
}
#endif /* LIBXML_REGEXP_ENABLED */


/**
 * Get the successor of a node if available.
 *
 * @param cur  the current node
 * @returns the successor node or NULL
 */
static xmlNodePtr
xmlTextReaderGetSuccessor(xmlNodePtr cur) {
    if (cur == NULL) return(NULL) ; /* ERROR */
    if (cur->next != NULL) return(cur->next) ;
    do {
        cur = cur->parent;
        if (cur == NULL) break;
        if (cur->next != NULL) return(cur->next);
    } while (cur != NULL);
    return(cur);
}

/**
 * Makes sure that the current node is fully read as well as all its
 * descendant. It means the full DOM subtree must be available at the
 * end of the call.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the node was expanded successfully, 0 if there is no more
 *          nodes to read, or -1 in case of error
 */
static int
xmlTextReaderDoExpand(xmlTextReaderPtr reader) {
    int val;

    if ((reader == NULL) || (reader->node == NULL) || (reader->ctxt == NULL))
        return(-1);
    do {
	if (PARSER_STOPPED(reader->ctxt))
            return(1);

        if (xmlTextReaderGetSuccessor(reader->node) != NULL)
	    return(1);
	if (reader->ctxt->nodeNr < reader->depth)
	    return(1);
	if (reader->mode == XML_TEXTREADER_MODE_EOF)
	    return(1);
	val = xmlTextReaderPushData(reader);
	if (val < 0){
	    reader->mode = XML_TEXTREADER_MODE_ERROR;
            reader->state = XML_TEXTREADER_ERROR;
	    return(-1);
	}
    } while(reader->mode != XML_TEXTREADER_MODE_EOF);
    return(1);
}

/**
 *  Moves the position of the current instance to the next node in
 *  the stream, exposing its properties.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the node was read successfully, 0 if there is no more
 *          nodes to read, or -1 in case of error
 */
int
xmlTextReaderRead(xmlTextReader *reader) {
    int val, olddepth = 0;
    xmlTextReaderState oldstate = XML_TEXTREADER_START;
    xmlNodePtr oldnode = NULL;

    if (reader == NULL)
	return(-1);
    if (reader->state == XML_TEXTREADER_ERROR)
        return(-1);

    reader->curnode = NULL;
    if (reader->doc != NULL)
        return(xmlTextReaderReadTree(reader));
    if (reader->ctxt == NULL)
	return(-1);

    if (reader->mode == XML_TEXTREADER_MODE_INITIAL) {
	reader->mode = XML_TEXTREADER_MODE_INTERACTIVE;
	/*
	 * Initial state
	 */
	do {
	    val = xmlTextReaderPushData(reader);
            if (val < 0) {
                reader->mode = XML_TEXTREADER_MODE_ERROR;
                reader->state = XML_TEXTREADER_ERROR;
                return(-1);
            }
	} while ((reader->ctxt->node == NULL) &&
		 ((reader->mode != XML_TEXTREADER_MODE_EOF) &&
		  (reader->state != XML_TEXTREADER_DONE)));
	if (reader->ctxt->node == NULL) {
	    if (reader->ctxt->myDoc != NULL) {
		reader->node = reader->ctxt->myDoc->children;
	    }
	    if (reader->node == NULL) {
                reader->mode = XML_TEXTREADER_MODE_ERROR;
                reader->state = XML_TEXTREADER_ERROR;
		return(-1);
	    }
	    reader->state = XML_TEXTREADER_ELEMENT;
	} else {
	    if (reader->ctxt->myDoc != NULL) {
		reader->node = reader->ctxt->myDoc->children;
	    }
	    if (reader->node == NULL)
		reader->node = reader->ctxt->nodeTab[0];
	    reader->state = XML_TEXTREADER_ELEMENT;
	}
	reader->depth = 0;
	reader->ctxt->parseMode = XML_PARSE_READER;
	goto node_found;
    }
    oldstate = reader->state;
    olddepth = reader->ctxt->nodeNr;
    oldnode = reader->node;

get_next_node:
    if (reader->node == NULL) {
	if (reader->mode == XML_TEXTREADER_MODE_EOF) {
	    return(0);
        } else {
            reader->mode = XML_TEXTREADER_MODE_ERROR;
            reader->state = XML_TEXTREADER_ERROR;
	    return(-1);
        }
    }

    /*
     * If we are not backtracking on ancestors or examined nodes,
     * that the parser didn't finished or that we aren't at the end
     * of stream, continue processing.
     */
    while ((reader->node != NULL) && (reader->node->next == NULL) &&
	   (reader->ctxt->nodeNr == olddepth) &&
           ((oldstate == XML_TEXTREADER_BACKTRACK) ||
            (reader->node->children == NULL) ||
	    (reader->node->type == XML_ENTITY_REF_NODE) ||
	    ((reader->node->children != NULL) &&
	     (reader->node->children->type == XML_TEXT_NODE) &&
	     (reader->node->children->next == NULL)) ||
	    (reader->node->type == XML_DTD_NODE) ||
	    (reader->node->type == XML_DOCUMENT_NODE) ||
	    (reader->node->type == XML_HTML_DOCUMENT_NODE)) &&
	   ((reader->ctxt->node == NULL) ||
	    (reader->ctxt->node == reader->node) ||
	    (reader->ctxt->node == reader->node->parent)) &&
	   (reader->ctxt->instate != XML_PARSER_EOF) &&
	   (PARSER_STOPPED(reader->ctxt) == 0)) {
	val = xmlTextReaderPushData(reader);
	if (val < 0) {
            reader->mode = XML_TEXTREADER_MODE_ERROR;
            reader->state = XML_TEXTREADER_ERROR;
	    return(-1);
        }
	if (reader->node == NULL)
	    goto node_end;
    }
    if (oldstate != XML_TEXTREADER_BACKTRACK) {
	if ((reader->node->children != NULL) &&
	    (reader->node->type != XML_ENTITY_REF_NODE) &&
	    (reader->node->type != XML_XINCLUDE_START) &&
	    (reader->node->type != XML_DTD_NODE)) {
	    reader->node = reader->node->children;
	    reader->depth++;
	    reader->state = XML_TEXTREADER_ELEMENT;
	    goto node_found;
	}
    }
    if (reader->node->next != NULL) {
	if ((oldstate == XML_TEXTREADER_ELEMENT) &&
            (reader->node->type == XML_ELEMENT_NODE) &&
	    (reader->node->children == NULL) &&
	    ((reader->node->extra & NODE_IS_EMPTY) == 0)
#ifdef LIBXML_XINCLUDE_ENABLED
	    && (reader->in_xinclude <= 0)
#endif
	    ) {
	    reader->state = XML_TEXTREADER_END;
	    goto node_found;
	}
#ifdef LIBXML_REGEXP_ENABLED
	if ((reader->validate) &&
	    (reader->node->type == XML_ELEMENT_NODE))
	    if (xmlTextReaderValidatePop(reader) < 0)
                return(-1);
#endif /* LIBXML_REGEXP_ENABLED */
        if ((reader->preserves > 0) &&
	    (reader->node->extra & NODE_IS_SPRESERVED))
	    reader->preserves--;
	reader->node = reader->node->next;
	reader->state = XML_TEXTREADER_ELEMENT;

	/*
	 * Cleanup of the old node
	 */
	if ((reader->preserves == 0) &&
#ifdef LIBXML_XINCLUDE_ENABLED
	    (reader->in_xinclude == 0) &&
#endif
	    (reader->entNr == 0) &&
	    (reader->node->prev != NULL) &&
            (reader->node->prev->type != XML_DTD_NODE)) {
	    xmlNodePtr tmp = reader->node->prev;
	    if ((tmp->extra & NODE_IS_PRESERVED) == 0) {
                if (oldnode == tmp)
                    oldnode = NULL;
		xmlUnlinkNode(tmp);
		xmlTextReaderFreeNode(reader, tmp);
	    }
	}

	goto node_found;
    }
    if ((oldstate == XML_TEXTREADER_ELEMENT) &&
	(reader->node->type == XML_ELEMENT_NODE) &&
	(reader->node->children == NULL) &&
	((reader->node->extra & NODE_IS_EMPTY) == 0)) {;
	reader->state = XML_TEXTREADER_END;
	goto node_found;
    }
#ifdef LIBXML_REGEXP_ENABLED
    if ((reader->validate != XML_TEXTREADER_NOT_VALIDATE) &&
        (reader->node->type == XML_ELEMENT_NODE)) {
        if (xmlTextReaderValidatePop(reader) < 0)
            return(-1);
    }
#endif /* LIBXML_REGEXP_ENABLED */
    if ((reader->preserves > 0) &&
	(reader->node->extra & NODE_IS_SPRESERVED))
	reader->preserves--;
    reader->node = reader->node->parent;
    if ((reader->node == NULL) ||
	(reader->node->type == XML_DOCUMENT_NODE) ||
	(reader->node->type == XML_HTML_DOCUMENT_NODE)) {
	if (reader->mode != XML_TEXTREADER_MODE_EOF) {
	    val = xmlParseChunk(reader->ctxt, "", 0, 1);
	    reader->state = XML_TEXTREADER_DONE;
	    if (val != 0) {
                reader->mode = XML_TEXTREADER_MODE_ERROR;
                reader->state = XML_TEXTREADER_ERROR;
	        return(-1);
            }
	}
	reader->node = NULL;
	reader->depth = -1;

	/*
	 * Cleanup of the old node
	 */
	if ((oldnode != NULL) && (reader->preserves == 0) &&
#ifdef LIBXML_XINCLUDE_ENABLED
	    (reader->in_xinclude == 0) &&
#endif
	    (reader->entNr == 0) &&
	    (oldnode->type != XML_DTD_NODE) &&
	    ((oldnode->extra & NODE_IS_PRESERVED) == 0)) {
	    xmlUnlinkNode(oldnode);
	    xmlTextReaderFreeNode(reader, oldnode);
	}

	goto node_end;
    }
    if ((reader->preserves == 0) &&
#ifdef LIBXML_XINCLUDE_ENABLED
        (reader->in_xinclude == 0) &&
#endif
	(reader->entNr == 0) &&
        (reader->node->last != NULL) &&
        ((reader->node->last->extra & NODE_IS_PRESERVED) == 0)) {
	xmlNodePtr tmp = reader->node->last;
	xmlUnlinkNode(tmp);
	xmlTextReaderFreeNode(reader, tmp);
    }
    reader->depth--;
    reader->state = XML_TEXTREADER_BACKTRACK;

node_found:
    /*
     * If we are in the middle of a piece of CDATA make sure it's finished
     */
    if ((reader->node != NULL) &&
        (reader->node->next == NULL) &&
        ((reader->node->type == XML_TEXT_NODE) ||
	 (reader->node->type == XML_CDATA_SECTION_NODE))) {
            if (xmlTextReaderExpand(reader) == NULL)
	        return -1;
    }

#ifdef LIBXML_XINCLUDE_ENABLED
    /*
     * Handle XInclude if asked for
     */
    if ((reader->xinclude) && (reader->in_xinclude == 0) &&
        (reader->state != XML_TEXTREADER_BACKTRACK) &&
        (reader->node != NULL) &&
	(reader->node->type == XML_ELEMENT_NODE) &&
	(reader->node->ns != NULL) &&
	((xmlStrEqual(reader->node->ns->href, XINCLUDE_NS)) ||
	 (xmlStrEqual(reader->node->ns->href, XINCLUDE_OLD_NS)))) {
	if (reader->xincctxt == NULL) {
	    reader->xincctxt = xmlXIncludeNewContext(reader->ctxt->myDoc);
            if (reader->xincctxt == NULL) {
                xmlTextReaderErrMemory(reader);
                return(-1);
            }
	    xmlXIncludeSetFlags(reader->xincctxt,
	                        reader->parserFlags & (~XML_PARSE_NOXINCNODE));
            xmlXIncludeSetStreamingMode(reader->xincctxt, 1);
            if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
                xmlXIncludeSetErrorHandler(reader->xincctxt,
                        xmlTextReaderStructuredRelay, reader);
            if (reader->resourceLoader != NULL)
                xmlXIncludeSetResourceLoader(reader->xincctxt,
                        reader->resourceLoader, reader->resourceCtxt);
	}
	/*
	 * expand that node and process it
	 */
	if (xmlTextReaderExpand(reader) == NULL)
	    return(-1);
        if (xmlXIncludeProcessNode(reader->xincctxt, reader->node) < 0) {
            int err = xmlXIncludeGetLastError(reader->xincctxt);

            if (xmlIsCatastrophicError(XML_ERR_FATAL, err)) {
                xmlFatalErr(reader->ctxt, err, NULL);
                reader->mode = XML_TEXTREADER_MODE_ERROR;
                reader->state = XML_TEXTREADER_ERROR;
            }
            return(-1);
        }
    }
    if ((reader->node != NULL) && (reader->node->type == XML_XINCLUDE_START)) {
        reader->in_xinclude++;
	goto get_next_node;
    }
    if ((reader->node != NULL) && (reader->node->type == XML_XINCLUDE_END)) {
        reader->in_xinclude--;
	goto get_next_node;
    }
#endif
    /*
     * Handle entities enter and exit when in entity replacement mode
     */
    if ((reader->node != NULL) &&
	(reader->node->type == XML_ENTITY_REF_NODE) &&
	(reader->ctxt != NULL) && (reader->ctxt->replaceEntities == 1)) {
	if ((reader->node->children != NULL) &&
	    (reader->node->children->type == XML_ENTITY_DECL) &&
	    (reader->node->children->children != NULL)) {
	    if (xmlTextReaderEntPush(reader, reader->node) < 0)
                goto get_next_node;
	    reader->node = reader->node->children->children;
	}
#ifdef LIBXML_REGEXP_ENABLED
    } else if ((reader->node != NULL) &&
	       (reader->node->type == XML_ENTITY_REF_NODE) &&
	       (reader->ctxt != NULL) && (reader->validate)) {
	if (xmlTextReaderValidateEntity(reader) < 0)
            return(-1);
#endif /* LIBXML_REGEXP_ENABLED */
    }
    if ((reader->node != NULL) &&
	(reader->node->type == XML_ENTITY_DECL) &&
	(reader->ent != NULL) && (reader->ent->children == reader->node)) {
	reader->node = xmlTextReaderEntPop(reader);
	reader->depth++;
        goto get_next_node;
    }
#ifdef LIBXML_REGEXP_ENABLED
    if ((reader->validate != XML_TEXTREADER_NOT_VALIDATE) && (reader->node != NULL)) {
	xmlNodePtr node = reader->node;

	if ((node->type == XML_ELEMENT_NODE) &&
            ((reader->state != XML_TEXTREADER_END) &&
	     (reader->state != XML_TEXTREADER_BACKTRACK))) {
	    if (xmlTextReaderValidatePush(reader) < 0)
                return(-1);
	} else if ((node->type == XML_TEXT_NODE) ||
		   (node->type == XML_CDATA_SECTION_NODE)) {
            xmlTextReaderValidateCData(reader, node->content,
	                               xmlStrlen(node->content));
	}
    }
#endif /* LIBXML_REGEXP_ENABLED */
#ifdef LIBXML_PATTERN_ENABLED
    if ((reader->patternNr > 0) && (reader->state != XML_TEXTREADER_END) &&
        (reader->state != XML_TEXTREADER_BACKTRACK)) {
        int i;
	for (i = 0;i < reader->patternNr;i++) {
	     if (xmlPatternMatch(reader->patternTab[i], reader->node) == 1) {
	         xmlTextReaderPreserve(reader);
		 break;
             }
	}
    }
#endif /* LIBXML_PATTERN_ENABLED */
#ifdef LIBXML_SCHEMAS_ENABLED
    if ((reader->validate == XML_TEXTREADER_VALIDATE_XSD) &&
        (reader->xsdValidErrors == 0) &&
	(reader->xsdValidCtxt != NULL)) {
	reader->xsdValidErrors = !xmlSchemaIsValid(reader->xsdValidCtxt);
    }
#endif /* LIBXML_PATTERN_ENABLED */
    return(1);
node_end:
    reader->state = XML_TEXTREADER_DONE;
    return(0);
}

/**
 * Gets the read state of the reader.
 *
 * @param reader  the xmlTextReader used
 * @returns the state value, or -1 in case of error
 */
int
xmlTextReaderReadState(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    return(reader->mode);
}

/**
 * Reads the contents of the current node and the full subtree. It then makes
 * the subtree available until the next #xmlTextReaderRead call
 *
 * @param reader  the xmlTextReader used
 * @returns a node pointer valid until the next #xmlTextReaderRead call
 *         or NULL in case of error.
 */
xmlNode *
xmlTextReaderExpand(xmlTextReader *reader) {
    if ((reader == NULL) || (reader->node == NULL))
        return(NULL);
    if (reader->doc != NULL)
        return(reader->node);
    if (reader->ctxt == NULL)
        return(NULL);
    if (xmlTextReaderDoExpand(reader) < 0)
        return(NULL);
    return(reader->node);
}

/**
 * Skip to the node following the current one in document order while
 * avoiding the subtree if any.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the node was read successfully, 0 if there is no more
 *          nodes to read, or -1 in case of error
 */
int
xmlTextReaderNext(xmlTextReader *reader) {
    int ret;
    xmlNodePtr cur;

    if (reader == NULL)
	return(-1);
    if (reader->doc != NULL)
        return(xmlTextReaderNextTree(reader));
    cur = reader->node;
    if ((cur == NULL) || (cur->type != XML_ELEMENT_NODE))
        return(xmlTextReaderRead(reader));
    if (reader->state == XML_TEXTREADER_END || reader->state == XML_TEXTREADER_BACKTRACK)
        return(xmlTextReaderRead(reader));
    if (cur->extra & NODE_IS_EMPTY)
        return(xmlTextReaderRead(reader));
    do {
        ret = xmlTextReaderRead(reader);
	if (ret != 1)
	    return(ret);
    } while (reader->node != cur);
    return(xmlTextReaderRead(reader));
}

#ifdef LIBXML_WRITER_ENABLED
static void
xmlTextReaderDumpCopy(xmlTextReaderPtr reader, xmlOutputBufferPtr output,
                      xmlNodePtr node) {
    if ((node->type == XML_DTD_NODE) ||
        (node->type == XML_ELEMENT_DECL) ||
        (node->type == XML_ATTRIBUTE_DECL) ||
        (node->type == XML_ENTITY_DECL))
        return;

    if ((node->type == XML_DOCUMENT_NODE) ||
        (node->type == XML_HTML_DOCUMENT_NODE)) {
        xmlNodeDumpOutput(output, node->doc, node, 0, 0, NULL);
    } else {
        xmlNodePtr copy;

        /*
         * Create a copy to make sure that namespace declarations from
         * ancestors are added.
         */
        copy = xmlDocCopyNode(node, node->doc, 1);
        if (copy == NULL) {
            xmlTextReaderErrMemory(reader);
            return;
        }

        xmlNodeDumpOutput(output, copy->doc, copy, 0, 0, NULL);

        xmlFreeNode(copy);
    }
}

/**
 * Reads the contents of the current node, including child nodes and markup.
 *
 * @param reader  the xmlTextReader used
 * @returns a string containing the XML content, or NULL if the current node
 *         is neither an element nor attribute, or has no child nodes. The
 *         string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderReadInnerXml(xmlTextReader *reader)
{
    xmlOutputBufferPtr output;
    xmlNodePtr cur;
    xmlChar *ret;

    if (xmlTextReaderExpand(reader) == NULL)
        return(NULL);

    if (reader->node == NULL)
        return(NULL);

    output = xmlAllocOutputBuffer(NULL);
    if (output == NULL) {
        xmlTextReaderErrMemory(reader);
        return(NULL);
    }

    for (cur = reader->node->children; cur != NULL; cur = cur->next)
        xmlTextReaderDumpCopy(reader, output, cur);

    if (output->error)
        xmlCtxtErrIO(reader->ctxt, output->error, NULL);

    ret = xmlBufDetach(output->buffer);
    xmlOutputBufferClose(output);

    return(ret);
}

/**
 * Reads the contents of the current node, including child nodes and markup.
 *
 * @param reader  the xmlTextReader used
 * @returns a string containing the node and any XML content, or NULL if the
 *         current node cannot be serialized. The string must be deallocated
 *         by the caller.
 */
xmlChar *
xmlTextReaderReadOuterXml(xmlTextReader *reader)
{
    xmlOutputBufferPtr output;
    xmlNodePtr node;
    xmlChar *ret;

    if (xmlTextReaderExpand(reader) == NULL)
        return(NULL);

    node = reader->node;
    if (node == NULL)
        return(NULL);

    output = xmlAllocOutputBuffer(NULL);
    if (output == NULL) {
        xmlTextReaderErrMemory(reader);
        return(NULL);
    }

    xmlTextReaderDumpCopy(reader, output, node);
    if (output->error)
        xmlCtxtErrIO(reader->ctxt, output->error, NULL);

    ret = xmlBufDetach(output->buffer);
    xmlOutputBufferClose(output);

    return(ret);
}
#endif

/**
 * Reads the contents of an element or a text node as a string.
 *
 * @param reader  the xmlTextReader used
 * @returns a string containing the contents of the non-empty Element or
 *         Text node (including CDATA sections), or NULL if the reader
 *         is positioned on any other type of node.
 *         The string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderReadString(xmlTextReader *reader)
{
    xmlNodePtr node, cur;
    xmlBufPtr buf;
    xmlChar *ret;

    if ((reader == NULL) || (reader->node == NULL))
       return(NULL);

    node = (reader->curnode != NULL) ? reader->curnode : reader->node;
    switch (node->type) {
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
            break;
        case XML_ELEMENT_NODE:
            if ((xmlTextReaderDoExpand(reader) == -1) ||
                (node->children == NULL))
                return(NULL);
            break;
        default:
            return(NULL);
    }

    buf = xmlBufCreate(50);
    if (buf == NULL) {
        xmlTextReaderErrMemory(reader);
        return(NULL);
    }

    cur = node;
    while (cur != NULL) {
        switch (cur->type) {
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
                xmlBufCat(buf, cur->content);
                break;

            case XML_ELEMENT_NODE:
                if (cur->children != NULL) {
                    cur = cur->children;
                    continue;
                }
                break;

            default:
                break;
        }

        if (cur == node)
            goto done;

        while (cur->next == NULL) {
            cur = cur->parent;
            if (cur == node)
                goto done;
        }
        cur = cur->next;
    }

done:
    ret = xmlBufDetach(buf);
    if (ret == NULL)
        xmlTextReaderErrMemory(reader);

    xmlBufFree(buf);
    return(ret);
}

/************************************************************************
 *									*
 *			Operating on a preparsed tree			*
 *									*
 ************************************************************************/
static int
xmlTextReaderNextTree(xmlTextReaderPtr reader)
{
    if (reader == NULL)
        return(-1);

    if (reader->state == XML_TEXTREADER_END)
        return(0);

    if (reader->node == NULL) {
        if (reader->doc->children == NULL) {
            reader->state = XML_TEXTREADER_END;
            return(0);
        }

        reader->node = reader->doc->children;
        reader->state = XML_TEXTREADER_START;
        return(1);
    }

    if (reader->state != XML_TEXTREADER_BACKTRACK) {
	/* Here removed traversal to child, because we want to skip the subtree,
	replace with traversal to sibling to skip subtree */
        if (reader->node->next != 0) {
	    /* Move to sibling if present,skipping sub-tree */
            reader->node = reader->node->next;
            reader->state = XML_TEXTREADER_START;
            return(1);
        }

	/* if reader->node->next is NULL mean no subtree for current node,
	so need to move to sibling of parent node if present */
	reader->state = XML_TEXTREADER_BACKTRACK;
	/* This will move to parent if present */
	xmlTextReaderRead(reader);
    }

    if (reader->node->next != 0) {
        reader->node = reader->node->next;
        reader->state = XML_TEXTREADER_START;
        return(1);
    }

    if (reader->node->parent != 0) {
        if (reader->node->parent->type == XML_DOCUMENT_NODE) {
            reader->state = XML_TEXTREADER_END;
            return(0);
        }

        reader->node = reader->node->parent;
        reader->depth--;
        reader->state = XML_TEXTREADER_BACKTRACK;
	/* Repeat process to move to sibling of parent node if present */
        xmlTextReaderNextTree(reader);
    }

    reader->state = XML_TEXTREADER_END;

    return(1);
}

/**
 *  Moves the position of the current instance to the next node in
 *  the stream, exposing its properties.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the node was read successfully, 0 if there is no more
 *          nodes to read, or -1 in case of error
 */
static int
xmlTextReaderReadTree(xmlTextReaderPtr reader) {
    if (reader->state == XML_TEXTREADER_END)
        return(0);

next_node:
    if (reader->node == NULL) {
        if (reader->doc->children == NULL) {
            reader->state = XML_TEXTREADER_END;
            return(0);
        }

        reader->node = reader->doc->children;
        reader->state = XML_TEXTREADER_START;
        goto found_node;
    }

    if ((reader->state != XML_TEXTREADER_BACKTRACK) &&
        (reader->node->type != XML_DTD_NODE) &&
        (reader->node->type != XML_XINCLUDE_START) &&
	(reader->node->type != XML_ENTITY_REF_NODE)) {
        if (reader->node->children != NULL) {
            reader->node = reader->node->children;
            reader->depth++;
            reader->state = XML_TEXTREADER_START;
            goto found_node;
        }

        if (reader->node->type == XML_ATTRIBUTE_NODE) {
            reader->state = XML_TEXTREADER_BACKTRACK;
            goto found_node;
        }
    }

    if (reader->node->next != NULL) {
        reader->node = reader->node->next;
        reader->state = XML_TEXTREADER_START;
        goto found_node;
    }

    if (reader->node->parent != NULL) {
        if ((reader->node->parent->type == XML_DOCUMENT_NODE) ||
	    (reader->node->parent->type == XML_HTML_DOCUMENT_NODE)) {
            reader->state = XML_TEXTREADER_END;
            return(0);
        }

        reader->node = reader->node->parent;
        reader->depth--;
        reader->state = XML_TEXTREADER_BACKTRACK;
        goto found_node;
    }

    reader->state = XML_TEXTREADER_END;

found_node:
    if ((reader->node->type == XML_XINCLUDE_START) ||
        (reader->node->type == XML_XINCLUDE_END))
	goto next_node;

    return(1);
}

/**
 * Skip to the node following the current one in document order while
 * avoiding the subtree if any.
 * Currently implemented only for Readers built on a document
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the node was read successfully, 0 if there is no more
 *          nodes to read, or -1 in case of error
 */
int
xmlTextReaderNextSibling(xmlTextReader *reader) {
    if (reader == NULL)
        return(-1);
    if (reader->doc == NULL) {
        /* TODO */
	return(-1);
    }

    if (reader->state == XML_TEXTREADER_END)
        return(0);

    if (reader->node == NULL)
        return(xmlTextReaderNextTree(reader));

    if (reader->node->next != NULL) {
        reader->node = reader->node->next;
        reader->state = XML_TEXTREADER_START;
        return(1);
    }

    return(0);
}

/************************************************************************
 *									*
 *			Constructor and destructors			*
 *									*
 ************************************************************************/
/**
 * Create an xmlTextReader structure fed with `input`
 *
 * @param input  the xmlParserInputBuffer used to read data
 * @param URI  the URI information for the source if available
 * @returns the new xmlTextReader or NULL in case of error
 */
xmlTextReader *
xmlNewTextReader(xmlParserInputBuffer *input, const char *URI) {
    xmlTextReaderPtr ret;

    if (input == NULL)
	return(NULL);
    ret = xmlMalloc(sizeof(xmlTextReader));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlTextReader));
    ret->doc = NULL;
    ret->entTab = NULL;
    ret->entMax = 0;
    ret->entNr = 0;
    ret->input = input;
    ret->buffer = xmlBufCreate(50);
    if (ret->buffer == NULL) {
        xmlFree(ret);
	return(NULL);
    }
    ret->sax = (xmlSAXHandler *) xmlMalloc(sizeof(xmlSAXHandler));
    if (ret->sax == NULL) {
	xmlBufFree(ret->buffer);
	xmlFree(ret);
	return(NULL);
    }
    xmlSAXVersion(ret->sax, 2);
    ret->startElement = ret->sax->startElement;
    ret->sax->startElement = xmlTextReaderStartElement;
    ret->endElement = ret->sax->endElement;
    ret->sax->endElement = xmlTextReaderEndElement;
#ifdef LIBXML_SAX1_ENABLED
    if (ret->sax->initialized == XML_SAX2_MAGIC) {
#endif /* LIBXML_SAX1_ENABLED */
	ret->startElementNs = ret->sax->startElementNs;
	ret->sax->startElementNs = xmlTextReaderStartElementNs;
	ret->endElementNs = ret->sax->endElementNs;
	ret->sax->endElementNs = xmlTextReaderEndElementNs;
#ifdef LIBXML_SAX1_ENABLED
    } else {
	ret->startElementNs = NULL;
	ret->endElementNs = NULL;
    }
#endif /* LIBXML_SAX1_ENABLED */
    ret->characters = ret->sax->characters;
    ret->sax->characters = xmlTextReaderCharacters;
    ret->sax->ignorableWhitespace = xmlTextReaderCharacters;
    ret->cdataBlock = ret->sax->cdataBlock;
    ret->sax->cdataBlock = xmlTextReaderCDataBlock;

    ret->mode = XML_TEXTREADER_MODE_INITIAL;
    ret->node = NULL;
    ret->curnode = NULL;
    if (xmlBufUse(ret->input->buffer) < 4) {
	xmlParserInputBufferRead(input, 4);
    }
    if (xmlBufUse(ret->input->buffer) >= 4) {
	ret->ctxt = xmlCreatePushParserCtxt(ret->sax, NULL,
			     (const char *) xmlBufContent(ret->input->buffer),
                                            4, URI);
	ret->base = 0;
	ret->cur = 4;
    } else {
	ret->ctxt = xmlCreatePushParserCtxt(ret->sax, NULL, NULL, 0, URI);
	ret->base = 0;
	ret->cur = 0;
    }

    if (ret->ctxt == NULL) {
	xmlBufFree(ret->buffer);
	xmlFree(ret->sax);
	xmlFree(ret);
	return(NULL);
    }
    ret->ctxt->parseMode = XML_PARSE_READER;
    ret->ctxt->_private = ret;
    ret->ctxt->dictNames = 1;
    ret->allocs = XML_TEXTREADER_CTXT;
    /*
     * use the parser dictionary to allocate all elements and attributes names
     */
    ret->dict = ret->ctxt->dict;
#ifdef LIBXML_XINCLUDE_ENABLED
    ret->xinclude = 0;
#endif
#ifdef LIBXML_PATTERN_ENABLED
    ret->patternMax = 0;
    ret->patternTab = NULL;
#endif
    return(ret);
}

/**
 * Create an xmlTextReader structure fed with the resource at `URI`
 *
 * @param URI  the URI of the resource to process
 * @returns the new xmlTextReader or NULL in case of error
 */
xmlTextReader *
xmlNewTextReaderFilename(const char *URI) {
    xmlParserInputBufferPtr input;
    xmlTextReaderPtr ret;

    if (xmlParserInputBufferCreateFilenameValue != NULL) {
        input = xmlParserInputBufferCreateFilenameValue(URI,
                XML_CHAR_ENCODING_NONE);
        if (input == NULL) {
            xmlTextReaderErr(XML_IO_ENOENT, "filaed to open %s", URI);
            return(NULL);
        }
    } else {
        xmlParserErrors code;

        /*
         * TODO: Remove XML_INPUT_UNZIP
         */
        code = xmlParserInputBufferCreateUrl(URI, XML_CHAR_ENCODING_NONE,
                                             XML_INPUT_UNZIP, &input);
        if (code != XML_ERR_OK) {
            xmlTextReaderErr(code, "failed to open %s", URI);
            return(NULL);
        }
    }

    ret = xmlNewTextReader(input, URI);
    if (ret == NULL) {
        xmlTextReaderErrMemory(NULL);
	xmlFreeParserInputBuffer(input);
	return(NULL);
    }
    ret->allocs |= XML_TEXTREADER_INPUT;
    return(ret);
}

/**
 * Deallocate all the resources associated to the reader
 *
 * @param reader  the xmlTextReader
 */
void
xmlFreeTextReader(xmlTextReader *reader) {
    if (reader == NULL)
	return;
#ifdef LIBXML_RELAXNG_ENABLED
    if (reader->rngSchemas != NULL) {
	xmlRelaxNGFree(reader->rngSchemas);
	reader->rngSchemas = NULL;
    }
    if (reader->rngValidCtxt != NULL) {
	if (! reader->rngPreserveCtxt)
	    xmlRelaxNGFreeValidCtxt(reader->rngValidCtxt);
	reader->rngValidCtxt = NULL;
    }
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
    if (reader->xsdPlug != NULL) {
	xmlSchemaSAXUnplug(reader->xsdPlug);
	reader->xsdPlug = NULL;
    }
    if (reader->xsdValidCtxt != NULL) {
	if (! reader->xsdPreserveCtxt)
	    xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	reader->xsdValidCtxt = NULL;
    }
    if (reader->xsdSchemas != NULL) {
	xmlSchemaFree(reader->xsdSchemas);
	reader->xsdSchemas = NULL;
    }
#endif
#ifdef LIBXML_XINCLUDE_ENABLED
    if (reader->xincctxt != NULL)
	xmlXIncludeFreeContext(reader->xincctxt);
#endif
#ifdef LIBXML_PATTERN_ENABLED
    if (reader->patternTab != NULL) {
        int i;
	for (i = 0;i < reader->patternNr;i++) {
	    if (reader->patternTab[i] != NULL)
	        xmlFreePattern(reader->patternTab[i]);
	}
	xmlFree(reader->patternTab);
    }
#endif
    if (reader->mode != XML_TEXTREADER_MODE_CLOSED)
        xmlTextReaderClose(reader);
    if (reader->ctxt != NULL) {
        if (reader->dict == reader->ctxt->dict)
	    reader->dict = NULL;
	if (reader->allocs & XML_TEXTREADER_CTXT)
	    xmlFreeParserCtxt(reader->ctxt);
    }
    if (reader->sax != NULL)
	xmlFree(reader->sax);
    if (reader->buffer != NULL)
        xmlBufFree(reader->buffer);
    if (reader->entTab != NULL)
	xmlFree(reader->entTab);
    if (reader->dict != NULL)
        xmlDictFree(reader->dict);
    xmlFree(reader);
}

/************************************************************************
 *									*
 *			Methods for XmlTextReader			*
 *									*
 ************************************************************************/

/**
 * This method releases any resources allocated by the current instance
 * changes the state to Closed and close any underlying input.
 *
 * @param reader  the xmlTextReader used
 * @returns 0 or -1 in case of error
 */
int
xmlTextReaderClose(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    reader->node = NULL;
    reader->curnode = NULL;
    reader->mode = XML_TEXTREADER_MODE_CLOSED;
    if (reader->faketext != NULL) {
        xmlFreeNode(reader->faketext);
        reader->faketext = NULL;
    }
    if (reader->ctxt != NULL) {
#ifdef LIBXML_VALID_ENABLED
	if ((reader->ctxt->vctxt.vstateTab != NULL) &&
	    (reader->ctxt->vctxt.vstateMax > 0)){
#ifdef LIBXML_REGEXP_ENABLED
            while (reader->ctxt->vctxt.vstateNr > 0)
                xmlValidatePopElement(&reader->ctxt->vctxt, NULL, NULL, NULL);
#endif /* LIBXML_REGEXP_ENABLED */
	    xmlFree(reader->ctxt->vctxt.vstateTab);
	    reader->ctxt->vctxt.vstateTab = NULL;
	    reader->ctxt->vctxt.vstateMax = 0;
	}
#endif /* LIBXML_VALID_ENABLED */
	xmlStopParser(reader->ctxt);
	if (reader->ctxt->myDoc != NULL) {
	    if (reader->preserve == 0)
		xmlTextReaderFreeDoc(reader, reader->ctxt->myDoc);
	    reader->ctxt->myDoc = NULL;
	}
    }
    if ((reader->input != NULL)  && (reader->allocs & XML_TEXTREADER_INPUT)) {
	xmlFreeParserInputBuffer(reader->input);
	reader->allocs -= XML_TEXTREADER_INPUT;
    }
    return(0);
}

/**
 * Provides the value of the attribute with the specified index relative
 * to the containing element.
 *
 * @param reader  the xmlTextReader used
 * @param no  the zero-based index of the attribute relative to the containing element
 * @returns a string containing the value of the specified attribute, or NULL
 *    in case of error. The string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderGetAttributeNo(xmlTextReader *reader, int no) {
    xmlChar *ret;
    int i;
    xmlAttrPtr cur;
    xmlNsPtr ns;

    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    if (reader->curnode != NULL)
	return(NULL);
    /* TODO: handle the xmlDecl */
    if (reader->node->type != XML_ELEMENT_NODE)
	return(NULL);

    ns = reader->node->nsDef;
    for (i = 0;(i < no) && (ns != NULL);i++) {
	ns = ns->next;
    }
    if (ns != NULL)
	return(readerStrdup(reader, ns->href));

    cur = reader->node->properties;
    if (cur == NULL)
	return(NULL);
    for (;i < no;i++) {
	cur = cur->next;
	if (cur == NULL)
	    return(NULL);
    }
    /* TODO walk the DTD if present */

    if (cur->children == NULL)
        return(NULL);
    ret = xmlNodeListGetString(reader->node->doc, cur->children, 1);
    if (ret == NULL)
        xmlTextReaderErrMemory(reader);
    return(ret);
}

/**
 * Provides the value of the attribute with the specified qualified name.
 *
 * @param reader  the xmlTextReader used
 * @param name  the qualified name of the attribute.
 * @returns a string containing the value of the specified attribute, or NULL
 *    in case of error. The string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderGetAttribute(xmlTextReader *reader, const xmlChar *name) {
    xmlChar *prefix = NULL;
    const xmlChar *localname;
    xmlNsPtr ns;
    xmlChar *ret = NULL;
    int result;

    if ((reader == NULL) || (name == NULL))
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    if (reader->curnode != NULL)
	return(NULL);

    /* TODO: handle the xmlDecl */
    if (reader->node->type != XML_ELEMENT_NODE)
	return(NULL);

    localname = xmlSplitQName4(name, &prefix);
    if (localname == NULL) {
        xmlTextReaderErrMemory(reader);
        return(NULL);
    }
    if (prefix == NULL) {
        /*
         * Namespace default decl
         */
        if (xmlStrEqual(name, BAD_CAST "xmlns")) {
            ns = reader->node->nsDef;
            while (ns != NULL) {
                if (ns->prefix == NULL) {
                    return(readerStrdup(reader, ns->href));
                }
                ns = ns->next;
            }
            return NULL;
        }

        result = xmlNodeGetAttrValue(reader->node, name, NULL, &ret);
        if (result < 0)
            xmlTextReaderErrMemory(reader);
        return(ret);
    }

    /*
     * Namespace default decl
     */
    if (xmlStrEqual(prefix, BAD_CAST "xmlns")) {
        ns = reader->node->nsDef;
        while (ns != NULL) {
            if ((ns->prefix != NULL) && (xmlStrEqual(ns->prefix, localname))) {
                ret = readerStrdup(reader, ns->href);
                break;
            }
            ns = ns->next;
        }
    } else {
        result = xmlSearchNsSafe(reader->node, prefix, &ns);
        if (result < 0)
            xmlTextReaderErrMemory(reader);
        if (ns != NULL) {
            result = xmlNodeGetAttrValue(reader->node, localname, ns->href,
                                         &ret);
            if (result < 0)
                xmlTextReaderErrMemory(reader);
        }
    }

    if (prefix != NULL)
        xmlFree(prefix);
    return(ret);
}


/**
 * Provides the value of the specified attribute
 *
 * @param reader  the xmlTextReader used
 * @param localName  the local name of the attribute.
 * @param namespaceURI  the namespace URI of the attribute.
 * @returns a string containing the value of the specified attribute, or NULL
 *    in case of error. The string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderGetAttributeNs(xmlTextReader *reader, const xmlChar *localName,
			    const xmlChar *namespaceURI) {
    xmlChar *ret = NULL;
    xmlChar *prefix = NULL;
    xmlNsPtr ns;
    int result;

    if ((reader == NULL) || (localName == NULL))
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    if (reader->curnode != NULL)
	return(NULL);

    /* TODO: handle the xmlDecl */
    if (reader->node->type != XML_ELEMENT_NODE)
	return(NULL);

    if (xmlStrEqual(namespaceURI, BAD_CAST "http://www.w3.org/2000/xmlns/")) {
        if (! xmlStrEqual(localName, BAD_CAST "xmlns")) {
            prefix = BAD_CAST localName;
        }
        ns = reader->node->nsDef;
        while (ns != NULL) {
            if ((prefix == NULL && ns->prefix == NULL) ||
                ((ns->prefix != NULL) && (xmlStrEqual(ns->prefix, localName)))) {
                return readerStrdup(reader, ns->href);
            }
            ns = ns->next;
        }
        return NULL;
    }

    result = xmlNodeGetAttrValue(reader->node, localName, namespaceURI, &ret);
    if (result < 0)
        xmlTextReaderErrMemory(reader);

    return(ret);
}

/**
 * Method to get the remainder of the buffered XML. this method stops the
 * parser, set its state to End Of File and return the input stream with
 * what is left that the parser did not use.
 *
 * The implementation is not good, the parser certainly progressed past
 * what's left in reader->input, and there is an allocation problem. Best
 * would be to rewrite it differently.
 *
 * @param reader  the xmlTextReader used
 * @returns the xmlParserInputBuffer attached to the XML or NULL
 *    in case of error.
 */
xmlParserInputBuffer *
xmlTextReaderGetRemainder(xmlTextReader *reader) {
    xmlParserInputBufferPtr ret = NULL;

    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);

    reader->node = NULL;
    reader->curnode = NULL;
    reader->mode = XML_TEXTREADER_MODE_EOF;
    if (reader->ctxt != NULL) {
	xmlStopParser(reader->ctxt);
	if (reader->ctxt->myDoc != NULL) {
	    if (reader->preserve == 0)
		xmlTextReaderFreeDoc(reader, reader->ctxt->myDoc);
	    reader->ctxt->myDoc = NULL;
	}
    }
    if (reader->allocs & XML_TEXTREADER_INPUT) {
	ret = reader->input;
	reader->input = NULL;
	reader->allocs -= XML_TEXTREADER_INPUT;
    } else {
	/*
	 * Hum, one may need to duplicate the data structure because
	 * without reference counting the input may be freed twice:
	 *   - by the layer which allocated it.
	 *   - by the layer to which would have been returned to.
	 */
	return(NULL);
    }
    return(ret);
}

/**
 * Resolves a namespace prefix in the scope of the current element.
 *
 * @param reader  the xmlTextReader used
 * @param prefix  the prefix whose namespace URI is to be resolved. To return
 *          the default namespace, specify NULL
 * @returns a string containing the namespace URI to which the prefix maps
 *    or NULL in case of error. The string must be deallocated by the caller.
 */
xmlChar *
xmlTextReaderLookupNamespace(xmlTextReader *reader, const xmlChar *prefix) {
    xmlNsPtr ns;
    int result;

    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);

    result = xmlSearchNsSafe(reader->node, prefix, &ns);
    if (result < 0) {
        xmlTextReaderErrMemory(reader);
        return(NULL);
    }
    if (ns == NULL)
	return(NULL);
    return(readerStrdup(reader, ns->href));
}

/**
 * Moves the position of the current instance to the attribute with
 * the specified index relative to the containing element.
 *
 * @param reader  the xmlTextReader used
 * @param no  the zero-based index of the attribute relative to the containing
 *      element.
 * @returns 1 in case of success, -1 in case of error, 0 if not found
 */
int
xmlTextReaderMoveToAttributeNo(xmlTextReader *reader, int no) {
    int i;
    xmlAttrPtr cur;
    xmlNsPtr ns;

    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    /* TODO: handle the xmlDecl */
    if (reader->node->type != XML_ELEMENT_NODE)
	return(-1);

    reader->curnode = NULL;

    ns = reader->node->nsDef;
    for (i = 0;(i < no) && (ns != NULL);i++) {
	ns = ns->next;
    }
    if (ns != NULL) {
	reader->curnode = (xmlNodePtr) ns;
	return(1);
    }

    cur = reader->node->properties;
    if (cur == NULL)
	return(0);
    for (;i < no;i++) {
	cur = cur->next;
	if (cur == NULL)
	    return(0);
    }
    /* TODO walk the DTD if present */

    reader->curnode = (xmlNodePtr) cur;
    return(1);
}

/**
 * Moves the position of the current instance to the attribute with
 * the specified qualified name.
 *
 * @param reader  the xmlTextReader used
 * @param name  the qualified name of the attribute.
 * @returns 1 in case of success, -1 in case of error, 0 if not found
 */
int
xmlTextReaderMoveToAttribute(xmlTextReader *reader, const xmlChar *name) {
    xmlChar *prefix = NULL;
    const xmlChar *localname;
    xmlNsPtr ns;
    xmlAttrPtr prop;

    if ((reader == NULL) || (name == NULL))
	return(-1);
    if (reader->node == NULL)
	return(-1);

    /* TODO: handle the xmlDecl */
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);

    localname = xmlSplitQName4(name, &prefix);
    if (localname == NULL) {
        xmlTextReaderErrMemory(reader);
        return(-1);
    }
    if (prefix == NULL) {
	/*
	 * Namespace default decl
	 */
	if (xmlStrEqual(name, BAD_CAST "xmlns")) {
	    ns = reader->node->nsDef;
	    while (ns != NULL) {
		if (ns->prefix == NULL) {
		    reader->curnode = (xmlNodePtr) ns;
		    return(1);
		}
		ns = ns->next;
	    }
	    return(0);
	}

	prop = reader->node->properties;
	while (prop != NULL) {
	    /*
	     * One need to have
	     *   - same attribute names
	     *   - and the attribute carrying that namespace
	     */
	    if ((xmlStrEqual(prop->name, name)) &&
		((prop->ns == NULL) || (prop->ns->prefix == NULL))) {
		reader->curnode = (xmlNodePtr) prop;
		return(1);
	    }
	    prop = prop->next;
	}
	return(0);
    }

    /*
     * Namespace default decl
     */
    if (xmlStrEqual(prefix, BAD_CAST "xmlns")) {
	ns = reader->node->nsDef;
	while (ns != NULL) {
	    if ((ns->prefix != NULL) && (xmlStrEqual(ns->prefix, localname))) {
		reader->curnode = (xmlNodePtr) ns;
		goto found;
	    }
	    ns = ns->next;
	}
	goto not_found;
    }
    prop = reader->node->properties;
    while (prop != NULL) {
	/*
	 * One need to have
	 *   - same attribute names
	 *   - and the attribute carrying that namespace
	 */
	if ((xmlStrEqual(prop->name, localname)) &&
	    (prop->ns != NULL) && (xmlStrEqual(prop->ns->prefix, prefix))) {
	    reader->curnode = (xmlNodePtr) prop;
	    goto found;
	}
	prop = prop->next;
    }
not_found:
    if (prefix != NULL)
        xmlFree(prefix);
    return(0);

found:
    if (prefix != NULL)
        xmlFree(prefix);
    return(1);
}

/**
 * Moves the position of the current instance to the attribute with the
 * specified local name and namespace URI.
 *
 * @param reader  the xmlTextReader used
 * @param localName  the local name of the attribute.
 * @param namespaceURI  the namespace URI of the attribute.
 * @returns 1 in case of success, -1 in case of error, 0 if not found
 */
int
xmlTextReaderMoveToAttributeNs(xmlTextReader *reader,
	const xmlChar *localName, const xmlChar *namespaceURI) {
    xmlAttrPtr prop;
    xmlNodePtr node;
    xmlNsPtr ns;
    xmlChar *prefix = NULL;

    if ((reader == NULL) || (localName == NULL) || (namespaceURI == NULL))
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);
    node = reader->node;

    if (xmlStrEqual(namespaceURI, BAD_CAST "http://www.w3.org/2000/xmlns/")) {
		if (! xmlStrEqual(localName, BAD_CAST "xmlns")) {
			prefix = BAD_CAST localName;
		}
		ns = reader->node->nsDef;
		while (ns != NULL) {
			if ((prefix == NULL && ns->prefix == NULL) ||
				((ns->prefix != NULL) && (xmlStrEqual(ns->prefix, localName)))) {
				reader->curnode = (xmlNodePtr) ns;
				return(1);
			}
			ns = ns->next;
		}
		return(0);
    }

    prop = node->properties;
    while (prop != NULL) {
	/*
	 * One need to have
	 *   - same attribute names
	 *   - and the attribute carrying that namespace
	 */
        if (xmlStrEqual(prop->name, localName) &&
	    ((prop->ns != NULL) &&
	     (xmlStrEqual(prop->ns->href, namespaceURI)))) {
	    reader->curnode = (xmlNodePtr) prop;
	    return(1);
        }
	prop = prop->next;
    }
    return(0);
}

/**
 * Moves the position of the current instance to the first attribute
 * associated with the current node.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 in case of success, -1 in case of error, 0 if not found
 */
int
xmlTextReaderMoveToFirstAttribute(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);

    if (reader->node->nsDef != NULL) {
	reader->curnode = (xmlNodePtr) reader->node->nsDef;
	return(1);
    }
    if (reader->node->properties != NULL) {
	reader->curnode = (xmlNodePtr) reader->node->properties;
	return(1);
    }
    return(0);
}

/**
 * Moves the position of the current instance to the next attribute
 * associated with the current node.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 in case of success, -1 in case of error, 0 if not found
 */
int
xmlTextReaderMoveToNextAttribute(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);
    if (reader->curnode == NULL)
	return(xmlTextReaderMoveToFirstAttribute(reader));

    if (reader->curnode->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) reader->curnode;
	if (ns->next != NULL) {
	    reader->curnode = (xmlNodePtr) ns->next;
	    return(1);
	}
	if (reader->node->properties != NULL) {
	    reader->curnode = (xmlNodePtr) reader->node->properties;
	    return(1);
	}
	return(0);
    } else if ((reader->curnode->type == XML_ATTRIBUTE_NODE) &&
	       (reader->curnode->next != NULL)) {
	reader->curnode = reader->curnode->next;
	return(1);
    }
    return(0);
}

/**
 * Moves the position of the current instance to the node that
 * contains the current Attribute  node.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 in case of success, -1 in case of error, 0 if not moved
 */
int
xmlTextReaderMoveToElement(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);
    if (reader->curnode != NULL) {
	reader->curnode = NULL;
	return(1);
    }
    return(0);
}

/**
 * Parses an attribute value into one or more Text and EntityReference nodes.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 in case of success, 0 if the reader was not positioned on an
 *         attribute node or all the attribute values have been read, or -1
 *         in case of error.
 */
int
xmlTextReaderReadAttributeValue(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->curnode == NULL)
	return(0);
    if (reader->curnode->type == XML_ATTRIBUTE_NODE) {
	if (reader->curnode->children == NULL)
	    return(0);
	reader->curnode = reader->curnode->children;
    } else if (reader->curnode->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) reader->curnode;

	if (reader->faketext == NULL) {
	    reader->faketext = xmlNewDocText(reader->node->doc,
		                             ns->href);
            if (reader->faketext == NULL) {
                xmlTextReaderErrMemory(reader);
                return(-1);
            }
	} else {
            if ((reader->faketext->content != NULL) &&
	        (reader->faketext->content !=
		 (xmlChar *) &(reader->faketext->properties)))
		xmlFree(reader->faketext->content);
            if (ns->href == NULL) {
                reader->faketext->content = NULL;
            } else {
                reader->faketext->content = xmlStrdup(ns->href);
                if (reader->faketext->content == NULL) {
                    xmlTextReaderErrMemory(reader);
                    return(-1);
                }
            }
	}
	reader->curnode = reader->faketext;
    } else {
	if (reader->curnode->next == NULL)
	    return(0);
	reader->curnode = reader->curnode->next;
    }
    return(1);
}

/**
 * Determine the encoding of the document being read.
 *
 * @param reader  the xmlTextReader used
 * @returns a string containing the encoding of the document or NULL in
 * case of error.  The string is deallocated with the reader.
 */
const xmlChar *
xmlTextReaderConstEncoding(xmlTextReader *reader) {
    const xmlChar *encoding = NULL;

    if (reader == NULL)
        return(NULL);

    if (reader->ctxt != NULL)
        encoding = xmlGetActualEncoding(reader->ctxt);
    else if (reader->doc != NULL)
        encoding = reader->doc->encoding;

    return(constString(reader, encoding));
}


/************************************************************************
 *									*
 *			Access API to the current node			*
 *									*
 ************************************************************************/
/**
 * Provides the number of attributes of the current node
 *
 * @param reader  the xmlTextReader used
 * @returns 0 i no attributes, -1 in case of error or the attribute count
 */
int
xmlTextReaderAttributeCount(xmlTextReader *reader) {
    int ret;
    xmlAttrPtr attr;
    xmlNsPtr ns;
    xmlNodePtr node;

    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(0);

    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    if (node->type != XML_ELEMENT_NODE)
	return(0);
    if ((reader->state == XML_TEXTREADER_END) ||
	(reader->state == XML_TEXTREADER_BACKTRACK))
	return(0);
    ret = 0;
    attr = node->properties;
    while (attr != NULL) {
	ret++;
	attr = attr->next;
    }
    ns = node->nsDef;
    while (ns != NULL) {
	ret++;
	ns = ns->next;
    }
    return(ret);
}

/**
 * Get the node type of the current node
 * Reference:
 * http://www.gnu.org/software/dotgnu/pnetlib-doc/System/Xml/XmlNodeType.html
 *
 * @param reader  the xmlTextReader used
 * @returns the xmlReaderTypes of the current node or -1 in case of error
 */
int
xmlTextReaderNodeType(xmlTextReader *reader) {
    xmlNodePtr node;

    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(XML_READER_TYPE_NONE);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    switch (node->type) {
        case XML_ELEMENT_NODE:
	    if ((reader->state == XML_TEXTREADER_END) ||
		(reader->state == XML_TEXTREADER_BACKTRACK))
		return(XML_READER_TYPE_END_ELEMENT);
	    return(XML_READER_TYPE_ELEMENT);
        case XML_NAMESPACE_DECL:
        case XML_ATTRIBUTE_NODE:
	    return(XML_READER_TYPE_ATTRIBUTE);
        case XML_TEXT_NODE:
	    if (xmlIsBlankNode(reader->node)) {
		if (xmlNodeGetSpacePreserve(reader->node))
		    return(XML_READER_TYPE_SIGNIFICANT_WHITESPACE);
		else
		    return(XML_READER_TYPE_WHITESPACE);
	    } else {
		return(XML_READER_TYPE_TEXT);
	    }
        case XML_CDATA_SECTION_NODE:
	    return(XML_READER_TYPE_CDATA);
        case XML_ENTITY_REF_NODE:
	    return(XML_READER_TYPE_ENTITY_REFERENCE);
        case XML_ENTITY_NODE:
	    return(XML_READER_TYPE_ENTITY);
        case XML_PI_NODE:
	    return(XML_READER_TYPE_PROCESSING_INSTRUCTION);
        case XML_COMMENT_NODE:
	    return(XML_READER_TYPE_COMMENT);
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
	    return(XML_READER_TYPE_DOCUMENT);
        case XML_DOCUMENT_FRAG_NODE:
	    return(XML_READER_TYPE_DOCUMENT_FRAGMENT);
        case XML_NOTATION_NODE:
	    return(XML_READER_TYPE_NOTATION);
        case XML_DOCUMENT_TYPE_NODE:
        case XML_DTD_NODE:
	    return(XML_READER_TYPE_DOCUMENT_TYPE);

        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
	    return(XML_READER_TYPE_NONE);
    }
    return(-1);
}

/**
 * Check if the current node is empty
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if empty, 0 if not and -1 in case of error
 */
int
xmlTextReaderIsEmptyElement(xmlTextReader *reader) {
    if ((reader == NULL) || (reader->node == NULL))
	return(-1);
    if (reader->node->type != XML_ELEMENT_NODE)
	return(0);
    if (reader->curnode != NULL)
	return(0);
    if (reader->node->children != NULL)
	return(0);
    if (reader->state == XML_TEXTREADER_END)
	return(0);
    if (reader->doc != NULL)
        return(1);
#ifdef LIBXML_XINCLUDE_ENABLED
    if (reader->in_xinclude > 0)
        return(1);
#endif
    return((reader->node->extra & NODE_IS_EMPTY) != 0);
}

/**
 * The local name of the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the local name or NULL if not available,
 *   if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderLocalName(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) node;
	if (ns->prefix == NULL)
	    return(readerStrdup(reader, BAD_CAST "xmlns"));
	else
	    return(readerStrdup(reader, ns->prefix));
    }
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(xmlTextReaderName(reader));
    return(readerStrdup(reader, node->name));
}

/**
 * The local name of the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the local name or NULL if not available, the
 *         string will be deallocated with the reader.
 */
const xmlChar *
xmlTextReaderConstLocalName(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) node;
	if (ns->prefix == NULL)
	    return(constString(reader, BAD_CAST "xmlns"));
	else
	    return(ns->prefix);
    }
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(xmlTextReaderConstName(reader));
    return(node->name);
}

/**
 * The qualified name of the node, equal to Prefix :LocalName.
 *
 * @param reader  the xmlTextReader used
 * @returns the local name or NULL if not available,
 *   if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderName(xmlTextReader *reader) {
    xmlNodePtr node;
    xmlChar *ret;

    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
	    if ((node->ns == NULL) ||
		(node->ns->prefix == NULL))
		return(readerStrdup(reader, node->name));

            ret = xmlBuildQName(node->name, node->ns->prefix, NULL, 0);
            if (ret == NULL)
                xmlTextReaderErrMemory(reader);
	    return(ret);
        case XML_TEXT_NODE:
	    return(readerStrdup(reader, BAD_CAST "#text"));
        case XML_CDATA_SECTION_NODE:
	    return(readerStrdup(reader, BAD_CAST "#cdata-section"));
        case XML_ENTITY_NODE:
        case XML_ENTITY_REF_NODE:
	    return(readerStrdup(reader, node->name));
        case XML_PI_NODE:
	    return(readerStrdup(reader, node->name));
        case XML_COMMENT_NODE:
	    return(readerStrdup(reader, BAD_CAST "#comment"));
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
	    return(readerStrdup(reader, BAD_CAST "#document"));
        case XML_DOCUMENT_FRAG_NODE:
	    return(readerStrdup(reader, BAD_CAST "#document-fragment"));
        case XML_NOTATION_NODE:
	    return(readerStrdup(reader, node->name));
        case XML_DOCUMENT_TYPE_NODE:
        case XML_DTD_NODE:
	    return(readerStrdup(reader, node->name));
        case XML_NAMESPACE_DECL: {
	    xmlNsPtr ns = (xmlNsPtr) node;

	    if (ns->prefix == NULL)
		return(readerStrdup(reader, BAD_CAST "xmlns"));
            ret = xmlBuildQName(ns->prefix, BAD_CAST "xmlns", NULL, 0);
            if (ret == NULL)
                xmlTextReaderErrMemory(reader);
	    return(ret);
	}

        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
	    return(NULL);
    }
    return(NULL);
}

/**
 * The qualified name of the node, equal to Prefix :LocalName.
 *
 * @param reader  the xmlTextReader used
 * @returns the local name or NULL if not available, the string is
 *         deallocated with the reader.
 */
const xmlChar *
xmlTextReaderConstName(xmlTextReader *reader) {
    xmlNodePtr node;

    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
	    if ((node->ns == NULL) ||
		(node->ns->prefix == NULL))
		return(node->name);
	    return(constQString(reader, node->ns->prefix, node->name));
        case XML_TEXT_NODE:
	    return(constString(reader, BAD_CAST "#text"));
        case XML_CDATA_SECTION_NODE:
	    return(constString(reader, BAD_CAST "#cdata-section"));
        case XML_ENTITY_NODE:
        case XML_ENTITY_REF_NODE:
	    return(constString(reader, node->name));
        case XML_PI_NODE:
	    return(constString(reader, node->name));
        case XML_COMMENT_NODE:
	    return(constString(reader, BAD_CAST "#comment"));
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
	    return(constString(reader, BAD_CAST "#document"));
        case XML_DOCUMENT_FRAG_NODE:
	    return(constString(reader, BAD_CAST "#document-fragment"));
        case XML_NOTATION_NODE:
	    return(constString(reader, node->name));
        case XML_DOCUMENT_TYPE_NODE:
        case XML_DTD_NODE:
	    return(constString(reader, node->name));
        case XML_NAMESPACE_DECL: {
	    xmlNsPtr ns = (xmlNsPtr) node;

	    if (ns->prefix == NULL)
		return(constString(reader, BAD_CAST "xmlns"));
	    return(constQString(reader, BAD_CAST "xmlns", ns->prefix));
	}

        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
	    return(NULL);
    }
    return(NULL);
}

/**
 * A shorthand reference to the namespace associated with the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the prefix or NULL if not available,
 *    if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderPrefix(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) node;
	if (ns->prefix == NULL)
	    return(NULL);
	return(readerStrdup(reader, BAD_CAST "xmlns"));
    }
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(NULL);
    if ((node->ns != NULL) && (node->ns->prefix != NULL))
	return(readerStrdup(reader, node->ns->prefix));
    return(NULL);
}

/**
 * A shorthand reference to the namespace associated with the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the prefix or NULL if not available, the string is deallocated
 *         with the reader.
 */
const xmlChar *
xmlTextReaderConstPrefix(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL) {
	xmlNsPtr ns = (xmlNsPtr) node;
	if (ns->prefix == NULL)
	    return(NULL);
	return(constString(reader, BAD_CAST "xmlns"));
    }
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(NULL);
    if ((node->ns != NULL) && (node->ns->prefix != NULL))
	return(constString(reader, node->ns->prefix));
    return(NULL);
}

/**
 * The URI defining the namespace associated with the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the namespace URI or NULL if not available,
 *    if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderNamespaceUri(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL)
	return(readerStrdup(reader, BAD_CAST "http://www.w3.org/2000/xmlns/"));
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(NULL);
    if (node->ns != NULL)
	return(readerStrdup(reader, node->ns->href));
    return(NULL);
}

/**
 * The URI defining the namespace associated with the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the namespace URI or NULL if not available, the string
 *         will be deallocated with the reader
 */
const xmlChar *
xmlTextReaderConstNamespaceUri(xmlTextReader *reader) {
    xmlNodePtr node;
    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;
    if (node->type == XML_NAMESPACE_DECL)
	return(constString(reader, BAD_CAST "http://www.w3.org/2000/xmlns/"));
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE))
	return(NULL);
    if (node->ns != NULL)
	return(constString(reader, node->ns->href));
    return(NULL);
}

/**
 * The base URI of the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the base URI or NULL if not available,
 *    if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderBaseUri(xmlTextReader *reader) {
    xmlChar *ret = NULL;
    int result;

    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    result = xmlNodeGetBaseSafe(NULL, reader->node, &ret);
    if (result < 0)
        xmlTextReaderErrMemory(reader);

    return(ret);
}

/**
 * The base URI of the node.
 *
 * @param reader  the xmlTextReader used
 * @returns the base URI or NULL if not available, the string
 *         will be deallocated with the reader
 */
const xmlChar *
xmlTextReaderConstBaseUri(xmlTextReader *reader) {
    xmlChar *tmp;
    const xmlChar *ret;
    int result;

    if ((reader == NULL) || (reader->node == NULL))
	return(NULL);
    result = xmlNodeGetBaseSafe(NULL, reader->node, &tmp);
    if (result < 0)
        xmlTextReaderErrMemory(reader);
    if (tmp == NULL)
        return(NULL);
    ret = constString(reader, tmp);
    xmlFree(tmp);
    return(ret);
}

/**
 * The depth of the node in the tree.
 *
 * @param reader  the xmlTextReader used
 * @returns the depth or -1 in case of error
 */
int
xmlTextReaderDepth(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(0);

    if (reader->curnode != NULL) {
	if ((reader->curnode->type == XML_ATTRIBUTE_NODE) ||
	    (reader->curnode->type == XML_NAMESPACE_DECL))
	    return(reader->depth + 1);
	return(reader->depth + 2);
    }
    return(reader->depth);
}

/**
 * Whether the node has attributes.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if true, 0 if false, and -1 in case or error
 */
int
xmlTextReaderHasAttributes(xmlTextReader *reader) {
    xmlNodePtr node;
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(0);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    if ((node->type == XML_ELEMENT_NODE) &&
	((node->properties != NULL) || (node->nsDef != NULL)))
	return(1);
    /* TODO: handle the xmlDecl */
    return(0);
}

/**
 * Whether the node can have a text value.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if true, 0 if false, and -1 in case or error
 */
int
xmlTextReaderHasValue(xmlTextReader *reader) {
    xmlNodePtr node;
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(0);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    switch (node->type) {
        case XML_ATTRIBUTE_NODE:
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
        case XML_NAMESPACE_DECL:
	    return(1);
	default:
	    break;
    }
    return(0);
}

/**
 * Provides the text value of the node if present
 *
 * @param reader  the xmlTextReader used
 * @returns the string or NULL if not available. The result must be deallocated
 *     with #xmlFree
 */
xmlChar *
xmlTextReaderValue(xmlTextReader *reader) {
    xmlNodePtr node;
    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    switch (node->type) {
        case XML_NAMESPACE_DECL:
	    return(readerStrdup(reader, ((xmlNsPtr) node)->href));
        case XML_ATTRIBUTE_NODE:{
	    xmlAttrPtr attr = (xmlAttrPtr) node;
            xmlDocPtr doc = NULL;
            xmlChar *ret;

            if (attr->children == NULL)
                return(NULL);
	    if (attr->parent != NULL)
                doc = attr->parent->doc;
	    ret = xmlNodeListGetString(doc, attr->children, 1);
            if (ret == NULL)
                xmlTextReaderErrMemory(reader);
	    return(ret);
	}
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
            return(readerStrdup(reader, node->content));
	default:
	    break;
    }
    return(NULL);
}

/**
 * Provides the text value of the node if present
 *
 * @param reader  the xmlTextReader used
 * @returns the string or NULL if not available. The result will be
 *     deallocated on the next Read() operation.
 */
const xmlChar *
xmlTextReaderConstValue(xmlTextReader *reader) {
    xmlNodePtr node;
    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    switch (node->type) {
        case XML_NAMESPACE_DECL:
	    return(((xmlNsPtr) node)->href);
        case XML_ATTRIBUTE_NODE:{
	    xmlAttrPtr attr = (xmlAttrPtr) node;
	    const xmlChar *ret;

	    if ((attr->children != NULL) &&
	        (attr->children->type == XML_TEXT_NODE) &&
		(attr->children->next == NULL))
		return(attr->children->content);
	    else {
		if (reader->buffer == NULL) {
		    reader->buffer = xmlBufCreate(50);
                    if (reader->buffer == NULL)
                        return (NULL);
                } else
                    xmlBufEmpty(reader->buffer);
	        xmlBufGetNodeContent(reader->buffer, node);
		ret = xmlBufContent(reader->buffer);
		if (ret == NULL) {
                    xmlTextReaderErrMemory(reader);
		    /* error on the buffer best to reallocate */
		    xmlBufFree(reader->buffer);
		    reader->buffer = xmlBufCreate(50);
		}
		return(ret);
	    }
	    break;
	}
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
	    return(node->content);
	default:
	    break;
    }
    return(NULL);
}

/**
 * Whether an Attribute  node was generated from the default value
 * defined in the DTD or schema.
 *
 * @param reader  the xmlTextReader used
 * @returns 0 if not defaulted, 1 if defaulted, and -1 in case of error
 */
int
xmlTextReaderIsDefault(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    return(0);
}

/**
 * The quotation mark character used to enclose the value of an attribute.
 *
 * @param reader  the xmlTextReader used
 * @returns " or ' and -1 in case of error
 */
int
xmlTextReaderQuoteChar(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    /* TODO maybe lookup the attribute value for " first */
    return('"');
}

/**
 * The xml:lang scope within which the node resides.
 *
 * @param reader  the xmlTextReader used
 * @returns the xml:lang value or NULL if none exists.,
 *    if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderXmlLang(xmlTextReader *reader) {
    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    return(xmlNodeGetLang(reader->node));
}

/**
 * The xml:lang scope within which the node resides.
 *
 * @param reader  the xmlTextReader used
 * @returns the xml:lang value or NULL if none exists.
 */
const xmlChar *
xmlTextReaderConstXmlLang(xmlTextReader *reader) {
    xmlChar *tmp;
    const xmlChar *ret;

    if (reader == NULL)
	return(NULL);
    if (reader->node == NULL)
	return(NULL);
    tmp = xmlNodeGetLang(reader->node);
    if (tmp == NULL)
        return(NULL);
    ret = constString(reader, tmp);
    xmlFree(tmp);
    return(ret);
}

/**
 * Get an interned string from the reader, allows for example to
 * speedup string name comparisons
 *
 * @param reader  the xmlTextReader used
 * @param str  the string to intern.
 * @returns an interned copy of the string or NULL in case of error. The
 *         string will be deallocated with the reader.
 */
const xmlChar *
xmlTextReaderConstString(xmlTextReader *reader, const xmlChar *str) {
    if (reader == NULL)
	return(NULL);
    return(constString(reader, str));
}

/**
 * The value indicating whether to normalize white space and attribute values.
 * Since attribute value and end of line normalizations are a MUST in the XML
 * specification only the value true is accepted. The broken behaviour of
 * accepting out of range character entities like &\#0; is of course not
 * supported either.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 or -1 in case of error.
 */
int
xmlTextReaderNormalization(xmlTextReader *reader) {
    if (reader == NULL)
	return(-1);
    return(1);
}

/************************************************************************
 *									*
 *			Extensions to the base APIs			*
 *									*
 ************************************************************************/

/**
 * Change the parser processing behaviour by changing some of its internal
 * properties. Note that some properties can only be changed before any
 * read has been done.
 *
 * @param reader  the xmlTextReader used
 * @param prop  the xmlParserProperties to set
 * @param value  usually 0 or 1 to (de)activate it
 * @returns 0 if the call was successful, or -1 in case of error
 */
int
xmlTextReaderSetParserProp(xmlTextReader *reader, int prop, int value) {
    xmlParserProperties p = (xmlParserProperties) prop;
    xmlParserCtxtPtr ctxt;

    if ((reader == NULL) || (reader->ctxt == NULL))
	return(-1);
    ctxt = reader->ctxt;

    switch (p) {
        case XML_PARSER_LOADDTD:
	    if (value != 0) {
		if (ctxt->loadsubset == 0) {
		    if (reader->mode != XML_TEXTREADER_MODE_INITIAL)
			return(-1);
                    ctxt->options |= XML_PARSE_DTDLOAD;
		    ctxt->loadsubset |= XML_DETECT_IDS;
		}
	    } else {
                ctxt->options &= ~XML_PARSE_DTDLOAD;
		ctxt->loadsubset &= ~XML_DETECT_IDS;
	    }
	    return(0);
        case XML_PARSER_DEFAULTATTRS:
	    if (value != 0) {
                ctxt->options |= XML_PARSE_DTDATTR;
		ctxt->loadsubset |= XML_COMPLETE_ATTRS;
	    } else {
                ctxt->options &= ~XML_PARSE_DTDATTR;
		ctxt->loadsubset &= ~XML_COMPLETE_ATTRS;
	    }
	    return(0);
        case XML_PARSER_VALIDATE:
	    if (value != 0) {
                ctxt->options |= XML_PARSE_DTDVALID;
		ctxt->validate = 1;
		reader->validate = XML_TEXTREADER_VALIDATE_DTD;
	    } else {
                ctxt->options &= ~XML_PARSE_DTDVALID;
		ctxt->validate = 0;
	    }
	    return(0);
        case XML_PARSER_SUBST_ENTITIES:
	    if (value != 0) {
                ctxt->options |= XML_PARSE_NOENT;
		ctxt->replaceEntities = 1;
	    } else {
                ctxt->options &= ~XML_PARSE_NOENT;
		ctxt->replaceEntities = 0;
	    }
	    return(0);
    }
    return(-1);
}

/**
 * Read the parser internal property.
 *
 * @param reader  the xmlTextReader used
 * @param prop  the xmlParserProperties to get
 * @returns the value, usually 0 or 1, or -1 in case of error.
 */
int
xmlTextReaderGetParserProp(xmlTextReader *reader, int prop) {
    xmlParserProperties p = (xmlParserProperties) prop;
    xmlParserCtxtPtr ctxt;

    if ((reader == NULL) || (reader->ctxt == NULL))
	return(-1);
    ctxt = reader->ctxt;

    switch (p) {
        case XML_PARSER_LOADDTD:
	    if ((ctxt->loadsubset != 0) || (ctxt->validate != 0))
		return(1);
	    return(0);
        case XML_PARSER_DEFAULTATTRS:
	    if (ctxt->loadsubset & XML_COMPLETE_ATTRS)
		return(1);
	    return(0);
        case XML_PARSER_VALIDATE:
	    return(reader->validate);
	case XML_PARSER_SUBST_ENTITIES:
	    return(ctxt->replaceEntities);
    }
    return(-1);
}


/**
 * Provide the line number of the current parsing point.
 *
 * @param reader  the user data (XML reader context)
 * @returns an int or 0 if not available
 */
int
xmlTextReaderGetParserLineNumber(xmlTextReader *reader)
{
    if ((reader == NULL) || (reader->ctxt == NULL) ||
        (reader->ctxt->input == NULL)) {
        return (0);
    }
    return (reader->ctxt->input->line);
}

/**
 * Provide the column number of the current parsing point.
 *
 * @param reader  the user data (XML reader context)
 * @returns an int or 0 if not available
 */
int
xmlTextReaderGetParserColumnNumber(xmlTextReader *reader)
{
    if ((reader == NULL) || (reader->ctxt == NULL) ||
        (reader->ctxt->input == NULL)) {
        return (0);
    }
    return (reader->ctxt->input->col);
}

/**
 * Hacking interface allowing to get the xmlNode corresponding to the
 * current node being accessed by the xmlTextReader. This is dangerous
 * because the underlying node may be destroyed on the next Reads.
 *
 * @param reader  the xmlTextReader used
 * @returns the xmlNode or NULL in case of error.
 */
xmlNode *
xmlTextReaderCurrentNode(xmlTextReader *reader) {
    if (reader == NULL)
	return(NULL);

    if (reader->curnode != NULL)
	return(reader->curnode);
    return(reader->node);
}

/**
 * This tells the XML Reader to preserve the current node.
 * The caller must also use #xmlTextReaderCurrentDoc to
 * keep an handle on the resulting document once parsing has finished
 *
 * @param reader  the xmlTextReader used
 * @returns the xmlNode or NULL in case of error.
 */
xmlNode *
xmlTextReaderPreserve(xmlTextReader *reader) {
    xmlNodePtr cur, parent;

    if (reader == NULL)
	return(NULL);

    cur = reader->node;
    if (cur == NULL)
        return(NULL);

    if ((cur->type != XML_DOCUMENT_NODE) && (cur->type != XML_DTD_NODE)) {
	cur->extra |= NODE_IS_PRESERVED;
	cur->extra |= NODE_IS_SPRESERVED;
    }
    reader->preserves++;

    parent = cur->parent;;
    while (parent != NULL) {
        if (parent->type == XML_ELEMENT_NODE)
	    parent->extra |= NODE_IS_PRESERVED;
	parent = parent->parent;
    }
    return(cur);
}

#ifdef LIBXML_PATTERN_ENABLED
/**
 * This tells the XML Reader to preserve all nodes matched by the
 * pattern. The caller must also use #xmlTextReaderCurrentDoc to
 * keep an handle on the resulting document once parsing has finished
 *
 * @param reader  the xmlTextReader used
 * @param pattern  an XPath subset pattern
 * @param namespaces  the prefix definitions, array of [URI, prefix] or NULL
 * @returns a non-negative number in case of success and -1 in case of error
 */
int
xmlTextReaderPreservePattern(xmlTextReader *reader, const xmlChar *pattern,
                             const xmlChar **namespaces)
{
    xmlPatternPtr comp;

    if ((reader == NULL) || (pattern == NULL))
	return(-1);

    comp = xmlPatterncompile(pattern, reader->dict, 0, namespaces);
    if (comp == NULL)
        return(-1);

    if (reader->patternNr >= reader->patternMax) {
        xmlPatternPtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(reader->patternMax, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
        if (newSize < 0) {
            xmlTextReaderErrMemory(reader);
            return(-1);
        }
	tmp = xmlRealloc(reader->patternTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlTextReaderErrMemory(reader);
            return(-1);
        }
	reader->patternTab = tmp;
        reader->patternMax = newSize;
    }
    reader->patternTab[reader->patternNr] = comp;
    return(reader->patternNr++);
}
#endif

/**
 * Hacking interface allowing to get the xmlDoc corresponding to the
 * current document being accessed by the xmlTextReader.
 * NOTE: as a result of this call, the reader will not destroy the
 *       associated XML document and calling #xmlFreeDoc on the result
 *       is needed once the reader parsing has finished.
 *
 * @param reader  the xmlTextReader used
 * @returns the xmlDoc or NULL in case of error.
 */
xmlDoc *
xmlTextReaderCurrentDoc(xmlTextReader *reader) {
    if (reader == NULL)
	return(NULL);
    if (reader->doc != NULL)
        return(reader->doc);
    if ((reader->ctxt == NULL) || (reader->ctxt->myDoc == NULL))
	return(NULL);

    reader->preserve = 1;
    return(reader->ctxt->myDoc);
}

#ifdef LIBXML_RELAXNG_ENABLED
/**
 * Use RelaxNG to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * if `schema` is NULL, then RelaxNG validation is deactivated.
 * The `schema` should not be freed until the reader is deallocated
 * or its use has been deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param schema  a precompiled RelaxNG schema
 * @returns 0 in case the RelaxNG validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderRelaxNGSetSchema(xmlTextReader *reader, xmlRelaxNG *schema) {
    if (reader == NULL)
        return(-1);
    if (schema == NULL) {
        if (reader->rngSchemas != NULL) {
	    xmlRelaxNGFree(reader->rngSchemas);
	    reader->rngSchemas = NULL;
	}
        if (reader->rngValidCtxt != NULL) {
	    if (! reader->rngPreserveCtxt)
		xmlRelaxNGFreeValidCtxt(reader->rngValidCtxt);
	    reader->rngValidCtxt = NULL;
        }
	reader->rngPreserveCtxt = 0;
	return(0);
    }
    if (reader->mode != XML_TEXTREADER_MODE_INITIAL)
	return(-1);
    if (reader->rngSchemas != NULL) {
	xmlRelaxNGFree(reader->rngSchemas);
	reader->rngSchemas = NULL;
    }
    if (reader->rngValidCtxt != NULL) {
	if (! reader->rngPreserveCtxt)
	    xmlRelaxNGFreeValidCtxt(reader->rngValidCtxt);
	reader->rngValidCtxt = NULL;
    }
    reader->rngPreserveCtxt = 0;
    reader->rngValidCtxt = xmlRelaxNGNewValidCtxt(schema);
    if (reader->rngValidCtxt == NULL)
        return(-1);
    if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
	xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt,
			xmlTextReaderStructuredRelay, reader);
    reader->rngValidErrors = 0;
    reader->rngFullNode = NULL;
    reader->validate = XML_TEXTREADER_VALIDATE_RNG;
    return(0);
}
#endif /* LIBXML_RELAXNG_ENABLED */

#ifdef LIBXML_SCHEMAS_ENABLED
/**
 * Internal locator function for the readers
 *
 * @param ctx  the xmlTextReader used
 * @param file  returned file information
 * @param line  returned line information
 * @returns 0 in case the Schema validation could be (de)activated and
 *         -1 in case of error.
 */
static int
xmlTextReaderLocator(void *ctx, const char **file, unsigned long *line) {
    xmlTextReaderPtr reader;

    if ((ctx == NULL) || ((file == NULL) && (line == NULL)))
        return(-1);

    if (file != NULL)
        *file = NULL;
    if (line != NULL)
        *line = 0;

    reader = (xmlTextReaderPtr) ctx;
    if ((reader->ctxt != NULL) && (reader->ctxt->input != NULL)) {
	if (file != NULL)
	    *file = reader->ctxt->input->filename;
	if (line != NULL)
	    *line = reader->ctxt->input->line;
	return(0);
    }
    if (reader->node != NULL) {
        long res;
	int ret = 0;

	if (line != NULL) {
	    res = xmlGetLineNo(reader->node);
	    if (res > 0)
	        *line = (unsigned long) res;
	    else
                ret = -1;
	}
        if (file != NULL) {
	    xmlDocPtr doc = reader->node->doc;
	    if ((doc != NULL) && (doc->URL != NULL))
	        *file = (const char *) doc->URL;
	    else
                ret = -1;
	}
	return(ret);
    }
    return(-1);
}

/**
 * Use XSD Schema to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * if `schema` is NULL, then Schema validation is deactivated.
 * The `schema` should not be freed until the reader is deallocated
 * or its use has been deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param schema  a precompiled Schema schema
 * @returns 0 in case the Schema validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderSetSchema(xmlTextReader *reader, xmlSchema *schema) {
    if (reader == NULL)
        return(-1);
    if (schema == NULL) {
	if (reader->xsdPlug != NULL) {
	    xmlSchemaSAXUnplug(reader->xsdPlug);
	    reader->xsdPlug = NULL;
	}
        if (reader->xsdValidCtxt != NULL) {
	    if (! reader->xsdPreserveCtxt)
		xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	    reader->xsdValidCtxt = NULL;
        }
	reader->xsdPreserveCtxt = 0;
        if (reader->xsdSchemas != NULL) {
	    xmlSchemaFree(reader->xsdSchemas);
	    reader->xsdSchemas = NULL;
	}
	return(0);
    }
    if (reader->mode != XML_TEXTREADER_MODE_INITIAL)
	return(-1);
    if (reader->xsdPlug != NULL) {
	xmlSchemaSAXUnplug(reader->xsdPlug);
	reader->xsdPlug = NULL;
    }
    if (reader->xsdValidCtxt != NULL) {
	if (! reader->xsdPreserveCtxt)
	    xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	reader->xsdValidCtxt = NULL;
    }
    reader->xsdPreserveCtxt = 0;
    if (reader->xsdSchemas != NULL) {
	xmlSchemaFree(reader->xsdSchemas);
	reader->xsdSchemas = NULL;
    }
    reader->xsdValidCtxt = xmlSchemaNewValidCtxt(schema);
    if (reader->xsdValidCtxt == NULL) {
	xmlSchemaFree(reader->xsdSchemas);
	reader->xsdSchemas = NULL;
        return(-1);
    }
    reader->xsdPlug = xmlSchemaSAXPlug(reader->xsdValidCtxt,
                                       &(reader->ctxt->sax),
				       &(reader->ctxt->userData));
    if (reader->xsdPlug == NULL) {
	xmlSchemaFree(reader->xsdSchemas);
	reader->xsdSchemas = NULL;
	xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	reader->xsdValidCtxt = NULL;
	return(-1);
    }
    xmlSchemaValidateSetLocator(reader->xsdValidCtxt,
                                xmlTextReaderLocator,
				(void *) reader);

    if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
	xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt,
			xmlTextReaderStructuredRelay, reader);
    reader->xsdValidErrors = 0;
    reader->validate = XML_TEXTREADER_VALIDATE_XSD;
    return(0);
}
#endif /* LIBXML_SCHEMAS_ENABLED */

#ifdef LIBXML_RELAXNG_ENABLED
/**
 * Use RelaxNG to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * If both `rng` and `ctxt` are NULL, then RelaxNG validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param rng  the path to a RelaxNG schema or NULL
 * @param ctxt  the RelaxNG schema validation context or NULL
 * @param options  options (not yet used)
 * @returns 0 in case the RelaxNG validation could be (de)activated and
 *	   -1 in case of error.
 */
static int
xmlTextReaderRelaxNGValidateInternal(xmlTextReaderPtr reader,
				     const char *rng,
				     xmlRelaxNGValidCtxtPtr ctxt,
				     int options ATTRIBUTE_UNUSED)
{
    if (reader == NULL)
	return(-1);

    if ((rng != NULL) && (ctxt != NULL))
	return (-1);

    if (((rng != NULL) || (ctxt != NULL)) &&
	((reader->mode != XML_TEXTREADER_MODE_INITIAL) ||
	 (reader->ctxt == NULL)))
	return(-1);

    /* Cleanup previous validation stuff. */
    if (reader->rngValidCtxt != NULL) {
	if ( !reader->rngPreserveCtxt)
	    xmlRelaxNGFreeValidCtxt(reader->rngValidCtxt);
	reader->rngValidCtxt = NULL;
    }
    reader->rngPreserveCtxt = 0;
    if (reader->rngSchemas != NULL) {
	xmlRelaxNGFree(reader->rngSchemas);
	reader->rngSchemas = NULL;
    }

    if ((rng == NULL) && (ctxt == NULL)) {
	/* We just want to deactivate the validation, so get out. */
	return(0);
    }


    if (rng != NULL) {
	xmlRelaxNGParserCtxtPtr pctxt;
	/* Parse the schema and create validation environment. */

	pctxt = xmlRelaxNGNewParserCtxt(rng);
	if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
	    xmlRelaxNGSetParserStructuredErrors(pctxt,
                    xmlTextReaderStructuredRelay, reader);
	reader->rngSchemas = xmlRelaxNGParse(pctxt);
	xmlRelaxNGFreeParserCtxt(pctxt);
	if (reader->rngSchemas == NULL)
	    return(-1);

	reader->rngValidCtxt = xmlRelaxNGNewValidCtxt(reader->rngSchemas);
	if (reader->rngValidCtxt == NULL) {
	    xmlRelaxNGFree(reader->rngSchemas);
	    reader->rngSchemas = NULL;
	    return(-1);
	}
    } else {
	/* Use the given validation context. */
	reader->rngValidCtxt = ctxt;
	reader->rngPreserveCtxt = 1;
    }
    /*
    * Redirect the validation context's error channels to use
    * the reader channels.
    * TODO: In case the user provides the validation context we
    *	could make this redirection optional.
    */
    if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
        xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt,
                xmlTextReaderStructuredRelay, reader);
    reader->rngValidErrors = 0;
    reader->rngFullNode = NULL;
    reader->validate = XML_TEXTREADER_VALIDATE_RNG;
    return(0);
}
#endif /* LIBXML_RELAXNG_ENABLED */

#ifdef LIBXML_SCHEMAS_ENABLED
/**
 * Validate the document as it is processed using XML Schema.
 * Activation is only possible before the first Read().
 * If both `xsd` and `ctxt` are NULL then XML Schema validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param xsd  the path to a W3C XSD schema or NULL
 * @param ctxt  the XML Schema validation context or NULL
 * @param options  options (not used yet)
 * @returns 0 in case the schemas validation could be (de)activated and
 *         -1 in case of error.
 */
static int
xmlTextReaderSchemaValidateInternal(xmlTextReaderPtr reader,
				    const char *xsd,
				    xmlSchemaValidCtxtPtr ctxt,
				    int options ATTRIBUTE_UNUSED)
{
    if (reader == NULL)
        return(-1);

    if ((xsd != NULL) && (ctxt != NULL))
	return(-1);

    if (((xsd != NULL) || (ctxt != NULL)) &&
	((reader->mode != XML_TEXTREADER_MODE_INITIAL) ||
        (reader->ctxt == NULL)))
	return(-1);

    /* Cleanup previous validation stuff. */
    if (reader->xsdPlug != NULL) {
	xmlSchemaSAXUnplug(reader->xsdPlug);
	reader->xsdPlug = NULL;
    }
    if (reader->xsdValidCtxt != NULL) {
	if (! reader->xsdPreserveCtxt)
	    xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	reader->xsdValidCtxt = NULL;
    }
    reader->xsdPreserveCtxt = 0;
    if (reader->xsdSchemas != NULL) {
	xmlSchemaFree(reader->xsdSchemas);
	reader->xsdSchemas = NULL;
    }

    if ((xsd == NULL) && (ctxt == NULL)) {
	/* We just want to deactivate the validation, so get out. */
	return(0);
    }

    if (xsd != NULL) {
	xmlSchemaParserCtxtPtr pctxt;
	/* Parse the schema and create validation environment. */
	pctxt = xmlSchemaNewParserCtxt(xsd);
	if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
	    xmlSchemaSetParserStructuredErrors(pctxt,
                    xmlTextReaderStructuredRelay, reader);
	reader->xsdSchemas = xmlSchemaParse(pctxt);
	xmlSchemaFreeParserCtxt(pctxt);
	if (reader->xsdSchemas == NULL)
	    return(-1);
	reader->xsdValidCtxt = xmlSchemaNewValidCtxt(reader->xsdSchemas);
	if (reader->xsdValidCtxt == NULL) {
	    xmlSchemaFree(reader->xsdSchemas);
	    reader->xsdSchemas = NULL;
	    return(-1);
	}
	reader->xsdPlug = xmlSchemaSAXPlug(reader->xsdValidCtxt,
	    &(reader->ctxt->sax),
	    &(reader->ctxt->userData));
	if (reader->xsdPlug == NULL) {
	    xmlSchemaFree(reader->xsdSchemas);
	    reader->xsdSchemas = NULL;
	    xmlSchemaFreeValidCtxt(reader->xsdValidCtxt);
	    reader->xsdValidCtxt = NULL;
	    return(-1);
	}
    } else {
	/* Use the given validation context. */
	reader->xsdValidCtxt = ctxt;
	reader->xsdPreserveCtxt = 1;
	reader->xsdPlug = xmlSchemaSAXPlug(reader->xsdValidCtxt,
	    &(reader->ctxt->sax),
	    &(reader->ctxt->userData));
	if (reader->xsdPlug == NULL) {
	    reader->xsdValidCtxt = NULL;
	    reader->xsdPreserveCtxt = 0;
	    return(-1);
	}
    }
    xmlSchemaValidateSetLocator(reader->xsdValidCtxt,
                                xmlTextReaderLocator,
				(void *) reader);
    /*
    * Redirect the validation context's error channels to use
    * the reader channels.
    * TODO: In case the user provides the validation context we
    *   could make this redirection optional.
    */
    if ((reader->errorFunc != NULL) || (reader->sErrorFunc != NULL))
	xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt,
			xmlTextReaderStructuredRelay, reader);
    reader->xsdValidErrors = 0;
    reader->validate = XML_TEXTREADER_VALIDATE_XSD;
    return(0);
}

/**
 * Use W3C XSD schema context to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * If `ctxt` is NULL, then XML Schema validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param ctxt  the XML Schema validation context or NULL
 * @param options  options (not used yet)
 * @returns 0 in case the schemas validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderSchemaValidateCtxt(xmlTextReader *reader,
				    xmlSchemaValidCtxt *ctxt,
				    int options)
{
    return(xmlTextReaderSchemaValidateInternal(reader, NULL, ctxt, options));
}

/**
 * Use W3C XSD schema to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * If `xsd` is NULL, then XML Schema validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param xsd  the path to a W3C XSD schema or NULL
 * @returns 0 in case the schemas validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderSchemaValidate(xmlTextReader *reader, const char *xsd)
{
    return(xmlTextReaderSchemaValidateInternal(reader, xsd, NULL, 0));
}
#endif /* LIBXML_SCHEMAS_ENABLED */

#ifdef LIBXML_RELAXNG_ENABLED
/**
 * Use RelaxNG schema context to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * If `ctxt` is NULL, then RelaxNG schema validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param ctxt  the RelaxNG schema validation context or NULL
 * @param options  options (not used yet)
 * @returns 0 in case the schemas validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderRelaxNGValidateCtxt(xmlTextReader *reader,
				 xmlRelaxNGValidCtxt *ctxt,
				 int options)
{
    return(xmlTextReaderRelaxNGValidateInternal(reader, NULL, ctxt, options));
}

/**
 * Use RelaxNG schema to validate the document as it is processed.
 * Activation is only possible before the first Read().
 * If `rng` is NULL, then RelaxNG schema validation is deactivated.
 *
 * @param reader  the xmlTextReader used
 * @param rng  the path to a RelaxNG schema or NULL
 * @returns 0 in case the schemas validation could be (de)activated and
 *         -1 in case of error.
 */
int
xmlTextReaderRelaxNGValidate(xmlTextReader *reader, const char *rng)
{
    return(xmlTextReaderRelaxNGValidateInternal(reader, rng, NULL, 0));
}
#endif /* LIBXML_RELAXNG_ENABLED */

/**
 * Determine whether the current node is a namespace declaration
 * rather than a regular attribute.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the current node is a namespace declaration, 0 if it
 * is a regular attribute or other type of node, or -1 in case of
 * error.
 */
int
xmlTextReaderIsNamespaceDecl(xmlTextReader *reader) {
    xmlNodePtr node;
    if (reader == NULL)
	return(-1);
    if (reader->node == NULL)
	return(-1);
    if (reader->curnode != NULL)
	node = reader->curnode;
    else
	node = reader->node;

    if (XML_NAMESPACE_DECL == node->type)
	return(1);
    else
	return(0);
}

/**
 * Determine the XML version of the document being read.
 *
 * @param reader  the xmlTextReader used
 * @returns a string containing the XML version of the document or NULL
 * in case of error.  The string is deallocated with the reader.
 */
const xmlChar *
xmlTextReaderConstXmlVersion(xmlTextReader *reader) {
    xmlDocPtr doc = NULL;
    if (reader == NULL)
	return(NULL);
    if (reader->doc != NULL)
        doc = reader->doc;
    else if (reader->ctxt != NULL)
	doc = reader->ctxt->myDoc;
    if (doc == NULL)
	return(NULL);

    if (doc->version == NULL)
	return(NULL);
    else
      return(constString(reader, doc->version));
}

/**
 * Determine the standalone status of the document being read.
 *
 * @param reader  the xmlTextReader used
 * @returns 1 if the document was declared to be standalone, 0 if it
 * was declared to be not standalone, or -1 if the document did not
 * specify its standalone status or in case of error.
 */
int
xmlTextReaderStandalone(xmlTextReader *reader) {
    xmlDocPtr doc = NULL;
    if (reader == NULL)
	return(-1);
    if (reader->doc != NULL)
        doc = reader->doc;
    else if (reader->ctxt != NULL)
	doc = reader->ctxt->myDoc;
    if (doc == NULL)
	return(-1);

    return(doc->standalone);
}

/************************************************************************
 *									*
 *			Error Handling Extensions                       *
 *									*
 ************************************************************************/

/**
 * Obtain the line number for the given locator.
 *
 * @param locator  the void used
 * @returns the line number or -1 in case of error.
 */
int
xmlTextReaderLocatorLineNumber(xmlTextReaderLocatorPtr locator) {
    /* we know that locator is a xmlParserCtxtPtr */
    xmlParserCtxtPtr ctx = (xmlParserCtxtPtr)locator;
    int ret = -1;

    if (locator == NULL)
        return(-1);
    if (ctx->node != NULL) {
	ret = xmlGetLineNo(ctx->node);
    }
    else {
	/* inspired from error.c */
	xmlParserInputPtr input;
	input = ctx->input;
	if ((input->filename == NULL) && (ctx->inputNr > 1))
	    input = ctx->inputTab[ctx->inputNr - 2];
	if (input != NULL) {
	    ret = input->line;
	}
	else {
	    ret = -1;
	}
    }

    return ret;
}

/**
 * Obtain the base URI for the given locator.
 *
 * @param locator  the void used
 * @returns the base URI or NULL in case of error,
 *    if non NULL it need to be freed by the caller.
 */
xmlChar *
xmlTextReaderLocatorBaseURI(xmlTextReaderLocatorPtr locator) {
    /* we know that locator is a xmlParserCtxtPtr */
    xmlParserCtxtPtr ctx = (xmlParserCtxtPtr)locator;
    xmlChar *ret = NULL;

    if (locator == NULL)
        return(NULL);
    if (ctx->node != NULL) {
	ret = xmlNodeGetBase(NULL,ctx->node);
    }
    else {
	/* inspired from error.c */
	xmlParserInputPtr input;
	input = ctx->input;
	if ((input->filename == NULL) && (ctx->inputNr > 1))
	    input = ctx->inputTab[ctx->inputNr - 2];
	if (input != NULL) {
	    ret = xmlStrdup(BAD_CAST input->filename);
	}
	else {
	    ret = NULL;
	}
    }

    return ret;
}

/**
 * Register a callback function that will be called on error and warnings.
 *
 * @deprecated Use #xmlTextReaderSetStructuredErrorHandler.
 *
 * If `f` is NULL, the default error and warning handlers are restored.
 *
 * @param reader  the xmlTextReader used
 * @param f  	the callback function to call on error and warnings
 * @param arg  a user argument to pass to the callback function
 */
void
xmlTextReaderSetErrorHandler(xmlTextReader *reader,
                             xmlTextReaderErrorFunc f, void *arg)
{
    if (reader == NULL)
        return;

    if (f != NULL) {
        reader->errorFunc = f;
        reader->sErrorFunc = NULL;
        reader->errorFuncArg = arg;
        xmlCtxtSetErrorHandler(reader->ctxt,
                xmlTextReaderStructuredRelay, reader);
#ifdef LIBXML_RELAXNG_ENABLED
        if (reader->rngValidCtxt) {
            xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt,
                    xmlTextReaderStructuredRelay, reader);
        }
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
        if (reader->xsdValidCtxt) {
            xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt,
                    xmlTextReaderStructuredRelay, reader);
        }
#endif
    } else {
        /* restore defaults */
        reader->errorFunc = NULL;
        reader->sErrorFunc = NULL;
        reader->errorFuncArg = NULL;
        xmlCtxtSetErrorHandler(reader->ctxt, NULL, NULL);
#ifdef LIBXML_RELAXNG_ENABLED
        if (reader->rngValidCtxt) {
            xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt, NULL,
                                               NULL);
        }
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
        if (reader->xsdValidCtxt) {
            xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt, NULL,
                                              NULL);
        }
#endif
    }
}

/**
* xmlTextReaderSetStructuredErrorHandler:
 *
 * Register a callback function that will be called on error and warnings.
 *
 * If `f` is NULL, the default error and warning handlers are restored.
 *
 * @param reader  the xmlTextReader used
 * @param f  	the callback function to call on error and warnings
 * @param arg  a user argument to pass to the callback function
 */
void
xmlTextReaderSetStructuredErrorHandler(xmlTextReader *reader,
                                       xmlStructuredErrorFunc f, void *arg)
{
    if (reader == NULL)
        return;

    if (f != NULL) {
        reader->sErrorFunc = f;
        reader->errorFunc = NULL;
        reader->errorFuncArg = arg;
        xmlCtxtSetErrorHandler(reader->ctxt,
                xmlTextReaderStructuredRelay, reader);
#ifdef LIBXML_RELAXNG_ENABLED
        if (reader->rngValidCtxt) {
            xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt,
                    xmlTextReaderStructuredRelay, reader);
        }
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
        if (reader->xsdValidCtxt) {
            xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt,
                    xmlTextReaderStructuredRelay, reader);
        }
#endif
    } else {
        /* restore defaults */
        reader->errorFunc = NULL;
        reader->sErrorFunc = NULL;
        reader->errorFuncArg = NULL;
        xmlCtxtSetErrorHandler(reader->ctxt, NULL, NULL);
#ifdef LIBXML_RELAXNG_ENABLED
        if (reader->rngValidCtxt) {
            xmlRelaxNGSetValidStructuredErrors(reader->rngValidCtxt, NULL,
                                               NULL);
        }
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
        if (reader->xsdValidCtxt) {
            xmlSchemaSetValidStructuredErrors(reader->xsdValidCtxt, NULL,
                                              NULL);
        }
#endif
    }
}

/**
 * Retrieve the error callback function and user argument.
 *
 * @param reader  the xmlTextReader used
 * @param f  	the callback function or NULL is no callback has been registered
 * @param arg  a user argument
 */
void
xmlTextReaderGetErrorHandler(xmlTextReader *reader,
                             xmlTextReaderErrorFunc * f, void **arg)
{
    if (f != NULL)
        *f = reader->errorFunc;
    if (arg != NULL)
        *arg = reader->errorFuncArg;
}

/**
 * Register a callback function that will be called to load external
 * resources like entities.
 *
 * @since 2.14.0
 * @param reader  thr reader
 * @param loader  resource loader
 * @param data  user data which will be passed to the loader
 */
void
xmlTextReaderSetResourceLoader(xmlTextReader *reader,
                               xmlResourceLoader loader, void *data) {
    if ((reader == NULL) || (reader->ctxt == NULL))
        return;

    reader->resourceLoader = loader;
    reader->resourceCtxt = data;

    xmlCtxtSetResourceLoader(reader->ctxt, loader, data);
}

/**
 * Retrieve the validity status from the parser context
 *
 * @param reader  the xmlTextReader used
 * @returns the flag value 1 if valid, 0 if no, and -1 in case of error
 */
int
xmlTextReaderIsValid(xmlTextReader *reader)
{
    if (reader == NULL)
        return (-1);
#ifdef LIBXML_RELAXNG_ENABLED
    if (reader->validate == XML_TEXTREADER_VALIDATE_RNG)
        return (reader->rngValidErrors == 0);
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
    if (reader->validate == XML_TEXTREADER_VALIDATE_XSD)
        return (reader->xsdValidErrors == 0);
#endif
    if ((reader->ctxt != NULL) && (reader->ctxt->validate == 1))
        return (reader->ctxt->valid);
    return (0);
}

/************************************************************************
 *									*
 *	New set (2.6.0) of simpler and more flexible APIs		*
 *									*
 ************************************************************************/

/**
 * Setup an XML reader with new options
 *
 * @param reader  an XML reader
 * @param input  xmlParserInputBuffer used to feed the reader, will
 *         be destroyed with it.
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlTextReaderSetup(xmlTextReader *reader,
                   xmlParserInputBuffer *input, const char *URL,
                   const char *encoding, int options)
{
    if (reader == NULL) {
        if (input != NULL)
	    xmlFreeParserInputBuffer(input);
        return (-1);
    }

    /*
     * we force the generation of compact text nodes on the reader
     * since usr applications should never modify the tree
     */
    options |= XML_PARSE_COMPACT;

    reader->doc = NULL;
    reader->entNr = 0;
    reader->parserFlags = options;
    reader->validate = XML_TEXTREADER_NOT_VALIDATE;
    if ((input != NULL) && (reader->input != NULL) &&
        (reader->allocs & XML_TEXTREADER_INPUT)) {
	xmlFreeParserInputBuffer(reader->input);
	reader->input = NULL;
	reader->allocs -= XML_TEXTREADER_INPUT;
    }
    if (input != NULL) {
	reader->input = input;
	reader->allocs |= XML_TEXTREADER_INPUT;
    }
    if (reader->buffer == NULL)
        reader->buffer = xmlBufCreate(50);
    if (reader->buffer == NULL) {
        return (-1);
    }
    if (reader->sax == NULL)
	reader->sax = (xmlSAXHandler *) xmlMalloc(sizeof(xmlSAXHandler));
    if (reader->sax == NULL) {
        return (-1);
    }
    xmlSAXVersion(reader->sax, 2);
    reader->startElement = reader->sax->startElement;
    reader->sax->startElement = xmlTextReaderStartElement;
    reader->endElement = reader->sax->endElement;
    reader->sax->endElement = xmlTextReaderEndElement;
#ifdef LIBXML_SAX1_ENABLED
    if (reader->sax->initialized == XML_SAX2_MAGIC) {
#endif /* LIBXML_SAX1_ENABLED */
        reader->startElementNs = reader->sax->startElementNs;
        reader->sax->startElementNs = xmlTextReaderStartElementNs;
        reader->endElementNs = reader->sax->endElementNs;
        reader->sax->endElementNs = xmlTextReaderEndElementNs;
#ifdef LIBXML_SAX1_ENABLED
    } else {
        reader->startElementNs = NULL;
        reader->endElementNs = NULL;
    }
#endif /* LIBXML_SAX1_ENABLED */
    reader->characters = reader->sax->characters;
    reader->sax->characters = xmlTextReaderCharacters;
    reader->sax->ignorableWhitespace = xmlTextReaderCharacters;
    reader->cdataBlock = reader->sax->cdataBlock;
    reader->sax->cdataBlock = xmlTextReaderCDataBlock;

    reader->mode = XML_TEXTREADER_MODE_INITIAL;
    reader->node = NULL;
    reader->curnode = NULL;
    if (input != NULL) {
        if (xmlBufUse(reader->input->buffer) < 4) {
            xmlParserInputBufferRead(input, 4);
        }
        if (reader->ctxt == NULL) {
            if (xmlBufUse(reader->input->buffer) >= 4) {
                reader->ctxt = xmlCreatePushParserCtxt(reader->sax, NULL,
		       (const char *) xmlBufContent(reader->input->buffer),
                                      4, URL);
                reader->base = 0;
                reader->cur = 4;
            } else {
                reader->ctxt =
                    xmlCreatePushParserCtxt(reader->sax, NULL, NULL, 0, URL);
                reader->base = 0;
                reader->cur = 0;
            }
            if (reader->ctxt == NULL) {
                return (-1);
            }
        } else {
	    xmlParserInputPtr inputStream;
	    xmlParserInputBufferPtr buf;

	    xmlCtxtReset(reader->ctxt);
	    buf = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
	    if (buf == NULL) return(-1);
	    inputStream = xmlNewInputStream(reader->ctxt);
	    if (inputStream == NULL) {
		xmlFreeParserInputBuffer(buf);
		return(-1);
	    }

	    if (URL == NULL)
		inputStream->filename = NULL;
	    else
		inputStream->filename = (char *)
		    xmlCanonicPath((const xmlChar *) URL);
	    inputStream->buf = buf;
            xmlBufResetInput(buf->buffer, inputStream);

            if (xmlCtxtPushInput(reader->ctxt, inputStream) < 0) {
                xmlFreeInputStream(inputStream);
                return(-1);
            }
	    reader->cur = 0;
	}
    }
    if (reader->dict != NULL) {
        if (reader->ctxt->dict != NULL) {
	    if (reader->dict != reader->ctxt->dict) {
		xmlDictFree(reader->dict);
		reader->dict = reader->ctxt->dict;
	    }
	} else {
	    reader->ctxt->dict = reader->dict;
	}
    } else {
	if (reader->ctxt->dict == NULL)
	    reader->ctxt->dict = xmlDictCreate();
        reader->dict = reader->ctxt->dict;
    }
    reader->ctxt->_private = reader;
    reader->ctxt->dictNames = 1;
    /*
     * use the parser dictionary to allocate all elements and attributes names
     */
    reader->ctxt->parseMode = XML_PARSE_READER;

#ifdef LIBXML_XINCLUDE_ENABLED
    if (reader->xincctxt != NULL) {
	xmlXIncludeFreeContext(reader->xincctxt);
	reader->xincctxt = NULL;
    }
    if (options & XML_PARSE_XINCLUDE) {
        reader->xinclude = 1;
	options -= XML_PARSE_XINCLUDE;
    } else
        reader->xinclude = 0;
    reader->in_xinclude = 0;
#endif
#ifdef LIBXML_PATTERN_ENABLED
    if (reader->patternTab == NULL) {
        reader->patternNr = 0;
	reader->patternMax = 0;
    }
    while (reader->patternNr > 0) {
        reader->patternNr--;
	if (reader->patternTab[reader->patternNr] != NULL) {
	    xmlFreePattern(reader->patternTab[reader->patternNr]);
            reader->patternTab[reader->patternNr] = NULL;
	}
    }
#endif

    if (options & XML_PARSE_DTDVALID)
        reader->validate = XML_TEXTREADER_VALIDATE_DTD;

    xmlCtxtUseOptions(reader->ctxt, options);
    if (encoding != NULL)
        xmlSwitchEncodingName(reader->ctxt, encoding);
    if ((URL != NULL) && (reader->ctxt->input != NULL) &&
        (reader->ctxt->input->filename == NULL)) {
        reader->ctxt->input->filename = (char *)
            xmlStrdup((const xmlChar *) URL);
        if (reader->ctxt->input->filename == NULL)
            return(-1);
    }

    reader->doc = NULL;

    return (0);
}

/**
 * Set the maximum amplification factor. See #xmlCtxtSetMaxAmplification.
 *
 * @param reader  an XML reader
 * @param maxAmpl  maximum amplification factor
 */
void
xmlTextReaderSetMaxAmplification(xmlTextReader *reader, unsigned maxAmpl)
{
    if (reader == NULL)
        return;
    xmlCtxtSetMaxAmplification(reader->ctxt, maxAmpl);
}

/**
 * @since 2.13.0
 *
 * @param reader  an XML reader
 * @returns the last error.
 */
const xmlError *
xmlTextReaderGetLastError(xmlTextReader *reader)
{
    if ((reader == NULL) || (reader->ctxt == NULL))
        return(NULL);
    return(&reader->ctxt->lastError);
}

/**
 * This function provides the current index of the parser used
 * by the reader, relative to the start of the current entity.
 * This function actually just wraps a call to #xmlByteConsumed
 * for the parser context associated with the reader.
 * See #xmlByteConsumed for more information.
 *
 * @deprecated The returned value is mostly random and useless.
 * It reflects the parser reading ahead and is in no way related to
 * the current node.
 *
 * @param reader  an XML reader
 * @returns the index in bytes from the beginning of the entity or -1
 *         in case the index could not be computed.
 */
long
xmlTextReaderByteConsumed(xmlTextReader *reader) {
    xmlParserInputPtr in;

    if ((reader == NULL) || (reader->ctxt == NULL))
        return(-1);
    in = reader->ctxt->input;
    if (in == NULL)
        return(-1);
    return(in->consumed + (in->cur - in->base));
}


/**
 * Create an xmltextReader for a preparsed document.
 *
 * @param doc  a preparsed document
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderWalker(xmlDoc *doc)
{
    xmlTextReaderPtr ret;

    if (doc == NULL)
        return(NULL);

    ret = xmlMalloc(sizeof(xmlTextReader));
    if (ret == NULL) {
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlTextReader));
    ret->entNr = 0;
    ret->input = NULL;
    ret->mode = XML_TEXTREADER_MODE_INITIAL;
    ret->node = NULL;
    ret->curnode = NULL;
    ret->base = 0;
    ret->cur = 0;
    ret->allocs = XML_TEXTREADER_CTXT;
    ret->doc = doc;
    ret->state = XML_TEXTREADER_START;
    ret->dict = xmlDictCreate();
    return(ret);
}

/**
 * Create an xmltextReader for an XML in-memory document.
 * The parsing flags `options` are a combination of xmlParserOption.
 *
 * @param cur  a pointer to a zero terminated string
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderForDoc(const xmlChar * cur, const char *URL, const char *encoding,
                int options)
{
    int len;

    if (cur == NULL)
        return (NULL);
    len = xmlStrlen(cur);

    return (xmlReaderForMemory
            ((const char *) cur, len, URL, encoding, options));
}

/**
 * parse an XML file from the filesystem or the network.
 * The parsing flags `options` are a combination of xmlParserOption.
 *
 * @param filename  a file or URL
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderForFile(const char *filename, const char *encoding, int options)
{
    xmlTextReaderPtr reader;

    reader = xmlNewTextReaderFilename(filename);
    if (reader == NULL)
        return (NULL);
    if (xmlTextReaderSetup(reader, NULL, NULL, encoding, options) < 0) {
        xmlFreeTextReader(reader);
        return (NULL);
    }
    return (reader);
}

/**
 * Create an xmltextReader for an XML in-memory document.
 * The parsing flags `options` are a combination of xmlParserOption.
 *
 * @param buffer  a pointer to a char array
 * @param size  the size of the array
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderForMemory(const char *buffer, int size, const char *URL,
                   const char *encoding, int options)
{
    xmlTextReaderPtr reader;
    xmlParserInputBufferPtr buf;

    buf = xmlParserInputBufferCreateMem(buffer, size, XML_CHAR_ENCODING_NONE);
    if (buf == NULL) {
        return (NULL);
    }
    reader = xmlNewTextReader(buf, URL);
    if (reader == NULL) {
        xmlFreeParserInputBuffer(buf);
        return (NULL);
    }
    reader->allocs |= XML_TEXTREADER_INPUT;
    if (xmlTextReaderSetup(reader, NULL, URL, encoding, options) < 0) {
        xmlFreeTextReader(reader);
        return (NULL);
    }
    return (reader);
}

/**
 * Create an xmltextReader for an XML from a file descriptor.
 * The parsing flags `options` are a combination of xmlParserOption.
 * NOTE that the file descriptor will not be closed when the
 *      reader is closed or reset.
 *
 * @param fd  an open file descriptor
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderForFd(int fd, const char *URL, const char *encoding, int options)
{
    xmlTextReaderPtr reader;
    xmlParserInputBufferPtr input;
    xmlParserErrors code;

    if (fd < 0) {
        xmlTextReaderErr(XML_ERR_ARGUMENT, "invalid argument");
        return(NULL);
    }

    input = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
        xmlTextReaderErrMemory(NULL);
        return(NULL);
    }
    /*
     * TODO: Remove XML_INPUT_UNZIP
     */
    code = xmlInputFromFd(input, fd, XML_INPUT_UNZIP);
    if (code != XML_ERR_OK) {
        xmlTextReaderErr(code, "failed to open fd");
        return(NULL);
    }
    input->closecallback = NULL;

    reader = xmlNewTextReader(input, URL);
    if (reader == NULL) {
        xmlTextReaderErrMemory(NULL);
        xmlFreeParserInputBuffer(input);
        return(NULL);
    }
    reader->allocs |= XML_TEXTREADER_INPUT;
    if (xmlTextReaderSetup(reader, NULL, URL, encoding, options) < 0) {
        xmlTextReaderErrMemory(NULL);
        xmlFreeTextReader(reader);
        return(NULL);
    }
    return (reader);
}

/**
 * Create an xmltextReader for an XML document from I/O functions and source.
 * The parsing flags `options` are a combination of xmlParserOption.
 *
 * @param ioread  an I/O read function
 * @param ioclose  an I/O close function
 * @param ioctx  an I/O handler
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the new reader or NULL in case of error.
 */
xmlTextReader *
xmlReaderForIO(xmlInputReadCallback ioread, xmlInputCloseCallback ioclose,
               void *ioctx, const char *URL, const char *encoding,
               int options)
{
    xmlTextReaderPtr reader;
    xmlParserInputBufferPtr input;

    if (ioread == NULL)
        return (NULL);

    input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx,
                                         XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
        if (ioclose != NULL)
            ioclose(ioctx);
        return (NULL);
    }
    reader = xmlNewTextReader(input, URL);
    if (reader == NULL) {
        xmlFreeParserInputBuffer(input);
        return (NULL);
    }
    reader->allocs |= XML_TEXTREADER_INPUT;
    if (xmlTextReaderSetup(reader, NULL, URL, encoding, options) < 0) {
        xmlFreeTextReader(reader);
        return (NULL);
    }
    return (reader);
}

/**
 * Setup an xmltextReader to parse a preparsed XML document.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param doc  a preparsed document
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewWalker(xmlTextReader *reader, xmlDoc *doc)
{
    if (doc == NULL)
        return (-1);
    if (reader == NULL)
        return (-1);

    if (reader->input != NULL) {
        xmlFreeParserInputBuffer(reader->input);
    }
    if (reader->ctxt != NULL) {
	xmlCtxtReset(reader->ctxt);
    }

    reader->entNr = 0;
    reader->input = NULL;
    reader->mode = XML_TEXTREADER_MODE_INITIAL;
    reader->node = NULL;
    reader->curnode = NULL;
    reader->base = 0;
    reader->cur = 0;
    reader->allocs = XML_TEXTREADER_CTXT;
    reader->doc = doc;
    reader->state = XML_TEXTREADER_START;
    if (reader->dict == NULL) {
        if ((reader->ctxt != NULL) && (reader->ctxt->dict != NULL))
	    reader->dict = reader->ctxt->dict;
	else
	    reader->dict = xmlDictCreate();
    }
    return(0);
}

/**
 * Setup an xmltextReader to parse an XML in-memory document.
 * The parsing flags `options` are a combination of xmlParserOption.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param cur  a pointer to a zero terminated string
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewDoc(xmlTextReader *reader, const xmlChar * cur,
                const char *URL, const char *encoding, int options)
{

    int len;

    if (cur == NULL)
        return (-1);
    if (reader == NULL)
        return (-1);

    len = xmlStrlen(cur);
    return (xmlReaderNewMemory(reader, (const char *)cur, len,
                               URL, encoding, options));
}

/**
 * parse an XML file from the filesystem or the network.
 * The parsing flags `options` are a combination of xmlParserOption.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param filename  a file or URL
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewFile(xmlTextReader *reader, const char *filename,
                 const char *encoding, int options)
{
    xmlParserInputBufferPtr input;

    if ((filename == NULL) || (reader == NULL)) {
        xmlTextReaderErr(XML_ERR_ARGUMENT, "invalid argument");
        return(-1);
    }

    if (xmlParserInputBufferCreateFilenameValue != NULL) {
        input = xmlParserInputBufferCreateFilenameValue(filename,
                XML_CHAR_ENCODING_NONE);
        if (input == NULL) {
            xmlTextReaderErr(XML_IO_ENOENT, "failed to open %s", filename);
            return(-1);
        }
    } else {
        /*
         * TODO: Remove XML_INPUT_UNZIP
         */
        xmlParserInputFlags flags = XML_INPUT_UNZIP;
        xmlParserErrors code;

        if ((options & XML_PARSE_NONET) == 0)
            flags |= XML_INPUT_NETWORK;

        code = xmlParserInputBufferCreateUrl(filename, XML_CHAR_ENCODING_NONE,
                                             flags, &input);
        if (code != XML_ERR_OK) {
            xmlTextReaderErr(code, "failed to open %s", filename);
            return(-1);
        }
    }

    if (xmlTextReaderSetup(reader, input, filename, encoding, options) < 0) {
        xmlTextReaderErrMemory(NULL);
        return(-1);
    }

    return(0);
}

/**
 * Setup an xmltextReader to parse an XML in-memory document.
 * The parsing flags `options` are a combination of xmlParserOption.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param buffer  a pointer to a char array
 * @param size  the size of the array
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewMemory(xmlTextReader *reader, const char *buffer, int size,
                   const char *URL, const char *encoding, int options)
{
    xmlParserInputBufferPtr input;

    if (reader == NULL)
        return (-1);
    if (buffer == NULL)
        return (-1);

    input = xmlParserInputBufferCreateMem(buffer, size,
                                      XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
        return (-1);
    }
    return (xmlTextReaderSetup(reader, input, URL, encoding, options));
}

/**
 * Setup an xmltextReader to parse an XML from a file descriptor.
 * NOTE that the file descriptor will not be closed when the
 *      reader is closed or reset.
 * The parsing flags `options` are a combination of xmlParserOption.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param fd  an open file descriptor
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewFd(xmlTextReader *reader, int fd,
               const char *URL, const char *encoding, int options)
{
    xmlParserInputBufferPtr input;
    xmlParserErrors code;

    if ((fd < 0) || (reader == NULL)) {
        xmlTextReaderErr(XML_ERR_ARGUMENT, "invalid argument");
        return(-1);
    }

    input = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
        xmlTextReaderErrMemory(NULL);
        return(-1);
    }
    /*
     * TODO: Remove XML_INPUT_UNZIP
     */
    code = xmlInputFromFd(input, fd, XML_INPUT_UNZIP);
    if (code != XML_ERR_OK) {
        xmlTextReaderErr(code, "failed to open fd");
        return(-1);
    }
    input->closecallback = NULL;

    if (xmlTextReaderSetup(reader, input, URL, encoding, options) < 0) {
        xmlTextReaderErrMemory(NULL);
        return(-1);
    }

    return(0);
}

/**
 * Setup an xmltextReader to parse an XML document from I/O functions
 * and source.
 * The parsing flags `options` are a combination of xmlParserOption.
 * This reuses the existing `reader` xmlTextReader.
 *
 * @param reader  an XML reader
 * @param ioread  an I/O read function
 * @param ioclose  an I/O close function
 * @param ioctx  an I/O handler
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlReaderNewIO(xmlTextReader *reader, xmlInputReadCallback ioread,
               xmlInputCloseCallback ioclose, void *ioctx,
               const char *URL, const char *encoding, int options)
{
    xmlParserInputBufferPtr input;

    if (ioread == NULL)
        return (-1);
    if (reader == NULL)
        return (-1);

    input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx,
                                         XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
        if (ioclose != NULL)
            ioclose(ioctx);
        return (-1);
    }
    return (xmlTextReaderSetup(reader, input, URL, encoding, options));
}

#endif /* LIBXML_READER_ENABLED */
