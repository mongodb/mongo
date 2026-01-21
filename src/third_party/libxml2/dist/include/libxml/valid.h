/**
 * @file
 * 
 * @brief DTD validator
 * 
 * API to handle XML Document Type Definitions and validate
 * documents.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */


#ifndef __XML_VALID_H__
#define __XML_VALID_H__

#include <libxml/xmlversion.h>
#include <libxml/xmlerror.h>
#define XML_TREE_INTERNALS
#include <libxml/tree.h>
#undef XML_TREE_INTERNALS
#include <libxml/list.h>
#include <libxml/xmlautomata.h>
#include <libxml/xmlregexp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Validation state added for non-deterministic content model.
 */
typedef struct _xmlValidState xmlValidState;
typedef xmlValidState *xmlValidStatePtr;

/**
 * Report a validity error.
 *
 * @param ctx  user data (usually an xmlValidCtxt)
 * @param msg  printf-like format string
 * @param ...  arguments to format
 */
typedef void (*xmlValidityErrorFunc) (void *ctx,
			     const char *msg,
			     ...) LIBXML_ATTR_FORMAT(2,3);

/**
 * Report a validity warning.
 *
 * @param ctx  user data (usually an xmlValidCtxt)
 * @param msg  printf-like format string
 * @param ...  arguments to format
 */
typedef void (*xmlValidityWarningFunc) (void *ctx,
			       const char *msg,
			       ...) LIBXML_ATTR_FORMAT(2,3);

typedef struct _xmlValidCtxt xmlValidCtxt;
typedef xmlValidCtxt *xmlValidCtxtPtr;
/**
 * An xmlValidCtxt is used for error reporting when validating.
 */
struct _xmlValidCtxt {
    void *userData;			/* user specific data block */
    xmlValidityErrorFunc error;		/* the callback in case of errors */
    xmlValidityWarningFunc warning;	/* the callback in case of warning */

    /* Node analysis stack used when validating within entities */
    xmlNode           *node;          /* Current parsed Node */
    int                nodeNr;        /* Depth of the parsing stack */
    int                nodeMax;       /* Max depth of the parsing stack */
    xmlNode          **nodeTab;       /* array of nodes */

    unsigned int         flags;       /* internal flags */
    xmlDoc                *doc;       /* the document */
    int                  valid;       /* temporary validity check result */

    /* state state used for non-determinist content validation */
    xmlValidState     *vstate;        /* current state */
    int                vstateNr;      /* Depth of the validation stack */
    int                vstateMax;     /* Max depth of the validation stack */
    xmlValidState     *vstateTab;     /* array of validation states */

#ifdef LIBXML_REGEXP_ENABLED
    xmlAutomata              *am;     /* the automata */
    xmlAutomataState      *state;     /* used to build the automata */
#else
    void                     *am;
    void                  *state;
#endif
};

typedef struct _xmlHashTable xmlNotationTable;
typedef xmlNotationTable *xmlNotationTablePtr;

typedef struct _xmlHashTable xmlElementTable;
typedef xmlElementTable *xmlElementTablePtr;

typedef struct _xmlHashTable xmlAttributeTable;
typedef xmlAttributeTable *xmlAttributeTablePtr;

typedef struct _xmlHashTable xmlIDTable;
typedef xmlIDTable *xmlIDTablePtr;

typedef struct _xmlHashTable xmlRefTable;
typedef xmlRefTable *xmlRefTablePtr;

/* Notation */
XMLPUBFUN xmlNotation *
		xmlAddNotationDecl	(xmlValidCtxt *ctxt,
					 xmlDtd *dtd,
					 const xmlChar *name,
					 const xmlChar *publicId,
					 const xmlChar *systemId);
XML_DEPRECATED
XMLPUBFUN xmlNotationTable *
		xmlCopyNotationTable	(xmlNotationTable *table);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeNotationTable	(xmlNotationTable *table);
#ifdef LIBXML_OUTPUT_ENABLED
XML_DEPRECATED
XMLPUBFUN void
		xmlDumpNotationDecl	(xmlBuffer *buf,
					 xmlNotation *nota);
/* XML_DEPRECATED, still used in lxml */
XMLPUBFUN void
		xmlDumpNotationTable	(xmlBuffer *buf,
					 xmlNotationTable *table);
#endif /* LIBXML_OUTPUT_ENABLED */

/* Element Content */
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
		xmlNewElementContent	(const xmlChar *name,
					 xmlElementContentType type);
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
		xmlCopyElementContent	(xmlElementContent *content);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeElementContent	(xmlElementContent *cur);
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
		xmlNewDocElementContent	(xmlDoc *doc,
					 const xmlChar *name,
					 xmlElementContentType type);
XML_DEPRECATED
XMLPUBFUN xmlElementContent *
		xmlCopyDocElementContent(xmlDoc *doc,
					 xmlElementContent *content);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeDocElementContent(xmlDoc *doc,
					 xmlElementContent *cur);
XML_DEPRECATED
XMLPUBFUN void
		xmlSnprintfElementContent(char *buf,
					 int size,
	                                 xmlElementContent *content,
					 int englob);
#ifdef LIBXML_OUTPUT_ENABLED
XML_DEPRECATED
XMLPUBFUN void
		xmlSprintfElementContent(char *buf,
	                                 xmlElementContent *content,
					 int englob);
#endif /* LIBXML_OUTPUT_ENABLED */

/* Element */
XMLPUBFUN xmlElement *
		xmlAddElementDecl	(xmlValidCtxt *ctxt,
					 xmlDtd *dtd,
					 const xmlChar *name,
					 xmlElementTypeVal type,
					 xmlElementContent *content);
XML_DEPRECATED
XMLPUBFUN xmlElementTable *
		xmlCopyElementTable	(xmlElementTable *table);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeElementTable	(xmlElementTable *table);
#ifdef LIBXML_OUTPUT_ENABLED
XML_DEPRECATED
XMLPUBFUN void
		xmlDumpElementTable	(xmlBuffer *buf,
					 xmlElementTable *table);
XML_DEPRECATED
XMLPUBFUN void
		xmlDumpElementDecl	(xmlBuffer *buf,
					 xmlElement *elem);
#endif /* LIBXML_OUTPUT_ENABLED */

/* Enumeration */
XML_DEPRECATED
XMLPUBFUN xmlEnumeration *
		xmlCreateEnumeration	(const xmlChar *name);
/* XML_DEPRECATED, needed for custom attributeDecl SAX handler */
XMLPUBFUN void
		xmlFreeEnumeration	(xmlEnumeration *cur);
XML_DEPRECATED
XMLPUBFUN xmlEnumeration *
		xmlCopyEnumeration	(xmlEnumeration *cur);

/* Attribute */
XMLPUBFUN xmlAttribute *
		xmlAddAttributeDecl	(xmlValidCtxt *ctxt,
					 xmlDtd *dtd,
					 const xmlChar *elem,
					 const xmlChar *name,
					 const xmlChar *ns,
					 xmlAttributeType type,
					 xmlAttributeDefault def,
					 const xmlChar *defaultValue,
					 xmlEnumeration *tree);
XML_DEPRECATED
XMLPUBFUN xmlAttributeTable *
		xmlCopyAttributeTable  (xmlAttributeTable *table);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeAttributeTable  (xmlAttributeTable *table);
#ifdef LIBXML_OUTPUT_ENABLED
XML_DEPRECATED
XMLPUBFUN void
		xmlDumpAttributeTable  (xmlBuffer *buf,
					xmlAttributeTable *table);
XML_DEPRECATED
XMLPUBFUN void
		xmlDumpAttributeDecl   (xmlBuffer *buf,
					xmlAttribute *attr);
#endif /* LIBXML_OUTPUT_ENABLED */

/* IDs */
XMLPUBFUN int
		xmlAddIDSafe	       (xmlAttr *attr,
					const xmlChar *value);
XMLPUBFUN xmlID *
		xmlAddID	       (xmlValidCtxt *ctxt,
					xmlDoc *doc,
					const xmlChar *value,
					xmlAttr *attr);
XMLPUBFUN void
		xmlFreeIDTable	       (xmlIDTable *table);
XMLPUBFUN xmlAttr *
		xmlGetID	       (xmlDoc *doc,
					const xmlChar *ID);
XMLPUBFUN int
		xmlIsID		       (xmlDoc *doc,
					xmlNode *elem,
					xmlAttr *attr);
XMLPUBFUN int
		xmlRemoveID	       (xmlDoc *doc,
					xmlAttr *attr);

/* IDREFs */
XML_DEPRECATED
XMLPUBFUN xmlRef *
		xmlAddRef	       (xmlValidCtxt *ctxt,
					xmlDoc *doc,
					const xmlChar *value,
					xmlAttr *attr);
XML_DEPRECATED
XMLPUBFUN void
		xmlFreeRefTable	       (xmlRefTable *table);
XML_DEPRECATED
XMLPUBFUN int
		xmlIsRef	       (xmlDoc *doc,
					xmlNode *elem,
					xmlAttr *attr);
XML_DEPRECATED
XMLPUBFUN int
		xmlRemoveRef	       (xmlDoc *doc,
					xmlAttr *attr);
XML_DEPRECATED
XMLPUBFUN xmlList *
		xmlGetRefs	       (xmlDoc *doc,
					const xmlChar *ID);

/**
 * The public function calls related to validity checking.
 */
#ifdef LIBXML_VALID_ENABLED
/* Allocate/Release Validation Contexts */
XMLPUBFUN xmlValidCtxt *
		xmlNewValidCtxt(void);
XMLPUBFUN void
		xmlFreeValidCtxt(xmlValidCtxt *);

XML_DEPRECATED
XMLPUBFUN int
		xmlValidateRoot		(xmlValidCtxt *ctxt,
					 xmlDoc *doc);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateElementDecl	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
		                         xmlElement *elem);
XML_DEPRECATED
XMLPUBFUN xmlChar *
		xmlValidNormalizeAttributeValue(xmlDoc *doc,
					 xmlNode *elem,
					 const xmlChar *name,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN xmlChar *
		xmlValidCtxtNormalizeAttributeValue(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem,
					 const xmlChar *name,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateAttributeDecl(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
		                         xmlAttribute *attr);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateAttributeValue(xmlAttributeType type,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateNotationDecl	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
		                         xmlNotation *nota);
XMLPUBFUN int
		xmlValidateDtd		(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlDtd *dtd);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateDtdFinal	(xmlValidCtxt *ctxt,
					 xmlDoc *doc);
XMLPUBFUN int
		xmlValidateDocument	(xmlValidCtxt *ctxt,
					 xmlDoc *doc);
XMLPUBFUN int
		xmlValidateElement	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateOneElement	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
		                         xmlNode *elem);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateOneAttribute	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode 	*elem,
					 xmlAttr *attr,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateOneNamespace	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem,
					 const xmlChar *prefix,
					 xmlNs *ns,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateDocumentFinal(xmlValidCtxt *ctxt,
					 xmlDoc *doc);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidateNotationUse	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 const xmlChar *notationName);
#endif /* LIBXML_VALID_ENABLED */

XML_DEPRECATED
XMLPUBFUN int
		xmlIsMixedElement	(xmlDoc *doc,
					 const xmlChar *name);
XMLPUBFUN xmlAttribute *
		xmlGetDtdAttrDesc	(xmlDtd *dtd,
					 const xmlChar *elem,
					 const xmlChar *name);
XMLPUBFUN xmlAttribute *
		xmlGetDtdQAttrDesc	(xmlDtd *dtd,
					 const xmlChar *elem,
					 const xmlChar *name,
					 const xmlChar *prefix);
XMLPUBFUN xmlNotation *
		xmlGetDtdNotationDesc	(xmlDtd *dtd,
					 const xmlChar *name);
XMLPUBFUN xmlElement *
		xmlGetDtdQElementDesc	(xmlDtd *dtd,
					 const xmlChar *name,
					 const xmlChar *prefix);
XMLPUBFUN xmlElement *
		xmlGetDtdElementDesc	(xmlDtd *dtd,
					 const xmlChar *name);

#ifdef LIBXML_VALID_ENABLED

XMLPUBFUN int
		xmlValidGetPotentialChildren(xmlElementContent *ctree,
					 const xmlChar **names,
					 int *len,
					 int max);

/* only needed for `xmllint --insert` */
XMLPUBFUN int
		xmlValidGetValidElements(xmlNode *prev,
					 xmlNode *next,
					 const xmlChar **names,
					 int max);
XMLPUBFUN int
		xmlValidateNameValue	(const xmlChar *value);
XMLPUBFUN int
		xmlValidateNamesValue	(const xmlChar *value);
XMLPUBFUN int
		xmlValidateNmtokenValue	(const xmlChar *value);
XMLPUBFUN int
		xmlValidateNmtokensValue(const xmlChar *value);

#ifdef LIBXML_REGEXP_ENABLED
/*
 * Validation based on the regexp support
 */
XML_DEPRECATED
XMLPUBFUN int
		xmlValidBuildContentModel(xmlValidCtxt *ctxt,
					 xmlElement *elem);

XML_DEPRECATED
XMLPUBFUN int
		xmlValidatePushElement	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem,
					 const xmlChar *qname);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidatePushCData	(xmlValidCtxt *ctxt,
					 const xmlChar *data,
					 int len);
XML_DEPRECATED
XMLPUBFUN int
		xmlValidatePopElement	(xmlValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem,
					 const xmlChar *qname);
#endif /* LIBXML_REGEXP_ENABLED */

#endif /* LIBXML_VALID_ENABLED */

#ifdef __cplusplus
}
#endif
#endif /* __XML_VALID_H__ */
