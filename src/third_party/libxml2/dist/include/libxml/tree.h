/**
 * @file
 *
 * @brief Document tree API
 *
 * Data structures and functions to build, modify, query and
 * serialize XML and HTML document trees. Also contains the
 * buffer API.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef XML_TREE_INTERNALS

/*
 * Emulate circular dependency for backward compatibility
 */
#include <libxml/parser.h>

#else /* XML_TREE_INTERNALS */

#ifndef __XML_TREE_H__
/** @cond ignore */
#define __XML_TREE_H__
/** @endcond */

#include <stdio.h>
#include <limits.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlstring.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlregexp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backward compatibility
 */
/** @cond ignore */
#define xmlBufferAllocScheme XML_BUFFER_ALLOC_EXACT
#define xmlDefaultBufferSize 4096
#define XML_GET_CONTENT(n) \
    ((n)->type == XML_ELEMENT_NODE ? NULL : (n)->content)
#define XML_GET_LINE(n)	xmlGetLineNo(n)
/** @endcond */

/*
 * Some of the basic types pointer to structures:
 */
/* xmlIO.h */
/**
 * Parser input buffer
 *
 * This struct and all related functions should ultimately
 * be removed from the public interface.
 */
typedef struct _xmlParserInputBuffer xmlParserInputBuffer;
typedef xmlParserInputBuffer *xmlParserInputBufferPtr;

/** Output buffer */
typedef struct _xmlOutputBuffer xmlOutputBuffer;
typedef xmlOutputBuffer *xmlOutputBufferPtr;

/* parser.h */
/** Parser input */
typedef struct _xmlParserInput xmlParserInput;
typedef xmlParserInput *xmlParserInputPtr;

/** Parser context */
typedef struct _xmlParserCtxt xmlParserCtxt;
typedef xmlParserCtxt *xmlParserCtxtPtr;

/** SAX locator */
typedef struct _xmlSAXLocator xmlSAXLocator;
typedef xmlSAXLocator *xmlSAXLocatorPtr;

/** SAX handler */
typedef struct _xmlSAXHandler xmlSAXHandler;
typedef xmlSAXHandler *xmlSAXHandlerPtr;

/* entities.h */
/** Entity declaration */
typedef struct _xmlEntity xmlEntity;
typedef xmlEntity *xmlEntityPtr;

/**
 * Removed, buffers always use XML_BUFFER_ALLOC_IO now.
 */
typedef enum {
    XML_BUFFER_ALLOC_DOUBLEIT,	/* double each time one need to grow */
    XML_BUFFER_ALLOC_EXACT,	/* grow only to the minimal size */
    XML_BUFFER_ALLOC_IMMUTABLE, /* immutable buffer, deprecated */
    XML_BUFFER_ALLOC_IO,	/* special allocation scheme used for I/O */
    XML_BUFFER_ALLOC_HYBRID,	/* exact up to a threshold, and doubleit thereafter */
    XML_BUFFER_ALLOC_BOUNDED	/* limit the upper size of the buffer */
} xmlBufferAllocationScheme;

/** Buffer type */
typedef struct _xmlBuffer xmlBuffer;
typedef xmlBuffer *xmlBufferPtr;
/**
 * A buffer structure, this old construct is limited to 2GB and
 * is being deprecated, use API with xmlBuf instead.
 */
struct _xmlBuffer {
    /**
     * @deprecated Use #xmlBufferContent
     *
     * The buffer content UTF8
     */
    xmlChar *content XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use #xmlBufferLength
     *
     * The buffer size used
     */
    unsigned int use XML_DEPRECATED_MEMBER;
    /* The buffer size */
    unsigned int size XML_DEPRECATED_MEMBER;
    /* The realloc method */
    xmlBufferAllocationScheme alloc XML_DEPRECATED_MEMBER;
    /* in IO mode we may have a different base */
    xmlChar *contentIO XML_DEPRECATED_MEMBER;
};

/** Buffer with 64-bit support */
typedef struct _xmlBuf xmlBuf;
typedef xmlBuf *xmlBufPtr;

/**
 * Macro used to express that the API use the new buffers for
 * xmlParserInputBuffer and xmlOutputBuffer. The change was
 * introduced in 2.9.0.
 */
#define LIBXML2_NEW_BUFFER

/**
 * This is the namespace for the special xml: prefix predefined in the
 * XML Namespace specification.
 */
#define XML_XML_NAMESPACE \
    (const xmlChar *) "http://www.w3.org/XML/1998/namespace"

/**
 * This is the name for the special xml:id attribute
 */
#define XML_XML_ID (const xmlChar *) "xml:id"

/**
 * The different element types carried by an XML tree.
 *
 * NOTE: This is synchronized with DOM Level 1 values.
 *       See http://www.w3.org/TR/REC-DOM-Level-1/
 *
 * Actually this had diverged a bit, and XML_DTD_NODE is used instead
 * of XML_DOCUMENT_TYPE_NODE.
 */
typedef enum {
    /**
     * An element.
     *
     * Objects of this type are an xmlNode.
     */
    XML_ELEMENT_NODE=		1,
    /**
     * An attribute.
     *
     * Objects of this type are an xmlAttr.
     */
    XML_ATTRIBUTE_NODE=		2,
    /**
     * A text node.
     *
     * Objects of this type are an xmlNode.
     */
    XML_TEXT_NODE=		3,
    /**
     * A CDATA section.
     *
     * Objects of this type are an xmlNode.
     */
    XML_CDATA_SECTION_NODE=	4,
    /**
     * An entity reference.
     *
     * Objects of this type are an xmlNode. The `children` member
     * points to the entity declaration if available.
     */
    XML_ENTITY_REF_NODE=	5,
    /** unused */
    XML_ENTITY_NODE=		6,
    /**
     * A processing instruction.
     *
     * Objects of this type are an xmlNode.
     */
    XML_PI_NODE=		7,
    /**
     * A comment.
     *
     * Objects of this type are an xmlNode.
     */
    XML_COMMENT_NODE=		8,
    /**
     * A document.
     *
     * Objects of this type are an xmlDoc.
     */
    XML_DOCUMENT_NODE=		9,
    /** unused */
    XML_DOCUMENT_TYPE_NODE=	10,
    /**
     * A document fragment.
     *
     * Objects of this type are an xmlNode.
     */
    XML_DOCUMENT_FRAG_NODE=	11,
    /** A notation, unused */
    XML_NOTATION_NODE=		12,
    /**
     * An HTML document.
     *
     * Objects of this type are an xmlDoc.
     */
    XML_HTML_DOCUMENT_NODE=	13,
    /**
     * A document type definition.
     *
     * Objects of this type are an xmlDtd.
     */
    XML_DTD_NODE=		14,
    /**
     * An element declaration.
     *
     * Objects of this type are an xmlElement.
     */
    XML_ELEMENT_DECL=		15,
    /**
     * An attribute declaration.
     *
     * Objects of this type are an xmlAttribute.
     */
    XML_ATTRIBUTE_DECL=		16,
    /**
     * An entity declaration.
     *
     * Objects of this type are an xmlEntity.
     */
    XML_ENTITY_DECL=		17,
    /**
     * An XPath namespace node.
     *
     * Can only be returned by the XPath engine. Objects of this
     * type are an xmlNs which has a completely different layout
     * than xmlNode. The `next` member contains a pointer to the
     * xmlNode element to which the namespace applies.
     *
     * Nodes of this type must be handled with extreme care to
     * avoid type confusion bugs.
     */
    XML_NAMESPACE_DECL=		18,
    /**
     * An XInclude start marker.
     *
     * Objects of this type are an xmlNode. Inserted as preceding
     * sibling of XIncluded content.
     */
    XML_XINCLUDE_START=		19,
    /**
     * An XInclude end marker.
     *
     * Objects of this type are an xmlNode. Inserted as following
     * sibling of XIncluded content.
     */
    XML_XINCLUDE_END=		20
    /* XML_DOCB_DOCUMENT_NODE=	21 */ /* removed */
} xmlElementType;

/** @cond IGNORE */
/* For backward compatibility */
#define XML_DOCB_DOCUMENT_NODE 21
/** @endcond */

/** Notation declaration */
typedef struct _xmlNotation xmlNotation;
typedef xmlNotation *xmlNotationPtr;
/**
 * A DTD Notation definition.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlNotation {
    /** Notation name */
    const xmlChar               *name;
    /** Public identifier, if any */
    const xmlChar               *PublicID;
    /** System identifier, if any */
    const xmlChar               *SystemID;
};

/**
 * A DTD Attribute type definition.
 */
typedef enum {
    XML_ATTRIBUTE_CDATA = 1,
    XML_ATTRIBUTE_ID,
    XML_ATTRIBUTE_IDREF	,
    XML_ATTRIBUTE_IDREFS,
    XML_ATTRIBUTE_ENTITY,
    XML_ATTRIBUTE_ENTITIES,
    XML_ATTRIBUTE_NMTOKEN,
    XML_ATTRIBUTE_NMTOKENS,
    XML_ATTRIBUTE_ENUMERATION,
    XML_ATTRIBUTE_NOTATION
} xmlAttributeType;

/**
 * A DTD Attribute default definition.
 */
typedef enum {
    XML_ATTRIBUTE_NONE = 1,
    XML_ATTRIBUTE_REQUIRED,
    XML_ATTRIBUTE_IMPLIED,
    XML_ATTRIBUTE_FIXED
} xmlAttributeDefault;

/** Enumeration in a DTD */
typedef struct _xmlEnumeration xmlEnumeration;
typedef xmlEnumeration *xmlEnumerationPtr;
/**
 * List structure used when there is an enumeration in DTDs.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlEnumeration {
    /** next enumeration */
    struct _xmlEnumeration    *next XML_DEPRECATED_MEMBER;
    /** value */
    const xmlChar            *name XML_DEPRECATED_MEMBER;
};

/** Attribute declaration */
typedef struct _xmlAttribute xmlAttribute;
typedef xmlAttribute *xmlAttributePtr;
/**
 * An Attribute declaration in a DTD.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlAttribute {
    /** application data */
    void           *_private;
    /** XML_ATTRIBUTE_DECL */
    xmlElementType          type;
    /** attribute name */
    const xmlChar          *name;
    /** NULL */
    struct _xmlNode    *children;
    /** NULL */
    struct _xmlNode        *last;
    /** DTD */
    struct _xmlDtd       *parent;
    /** next sibling */
    struct _xmlNode        *next;
    /** previous sibling */
    struct _xmlNode        *prev;
    /** containing document */
    struct _xmlDoc          *doc;

    /** next in hash table */
    struct _xmlAttribute  *nexth;
    /** attribute type */
    xmlAttributeType       atype;
    /** attribute default */
    xmlAttributeDefault      def;
    /** default value */
    xmlChar *defaultValue;
    /** enumeration tree if any */
    xmlEnumeration         *tree XML_DEPRECATED_MEMBER;
    /** namespace prefix if any */
    const xmlChar        *prefix;
    /** element name */
    const xmlChar          *elem;
};

/**
 * Possible definitions of element content types.
 */
typedef enum {
    XML_ELEMENT_CONTENT_PCDATA = 1,
    XML_ELEMENT_CONTENT_ELEMENT,
    XML_ELEMENT_CONTENT_SEQ,
    XML_ELEMENT_CONTENT_OR
} xmlElementContentType;

/**
 * Possible definitions of element content occurrences.
 */
typedef enum {
    XML_ELEMENT_CONTENT_ONCE = 1,
    XML_ELEMENT_CONTENT_OPT,
    XML_ELEMENT_CONTENT_MULT,
    XML_ELEMENT_CONTENT_PLUS
} xmlElementContentOccur;

/** Element content in element declarations */
typedef struct _xmlElementContent xmlElementContent;
typedef xmlElementContent *xmlElementContentPtr;
/**
 * An XML Element content as stored after parsing an element definition
 * in a DTD.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlElementContent {
    /** PCDATA, ELEMENT, SEQ or OR */
    xmlElementContentType     type XML_DEPRECATED_MEMBER;
    /** ONCE, OPT, MULT or PLUS */
    xmlElementContentOccur    ocur XML_DEPRECATED_MEMBER;
    /** element name */
    const xmlChar             *name XML_DEPRECATED_MEMBER;
    /** first child */
    struct _xmlElementContent *c1 XML_DEPRECATED_MEMBER;
    /** second child */
    struct _xmlElementContent *c2 XML_DEPRECATED_MEMBER;
    /** parent */
    struct _xmlElementContent *parent XML_DEPRECATED_MEMBER;
    /** namespace prefix */
    const xmlChar             *prefix XML_DEPRECATED_MEMBER;
};

/**
 * The different possibilities for an element content type.
 */
typedef enum {
    XML_ELEMENT_TYPE_UNDEFINED = 0,
    XML_ELEMENT_TYPE_EMPTY = 1,
    XML_ELEMENT_TYPE_ANY,
    XML_ELEMENT_TYPE_MIXED,
    XML_ELEMENT_TYPE_ELEMENT
} xmlElementTypeVal;

/** Element declaration */
typedef struct _xmlElement xmlElement;
typedef xmlElement *xmlElementPtr;
/**
 * An XML Element declaration from a DTD.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlElement {
    /** application data */
    void           *_private;
    /** XML_ELEMENT_DECL */
    xmlElementType          type;
    /** element name */
    const xmlChar          *name;
    /** NULL */
    struct _xmlNode    *children;
    /** NULL */
    struct _xmlNode        *last;
    /** -> DTD */
    struct _xmlDtd       *parent;
    /** next sibling */
    struct _xmlNode        *next;
    /** previous sibling */
    struct _xmlNode        *prev;
    /** containing document */
    struct _xmlDoc          *doc;

    /** element type */
    xmlElementTypeVal      etype;
    /** allowed element content */
    xmlElementContent *content;
    /** list of declared attributes */
    xmlAttribute     *attributes;
    /** namespace prefix if any */
    const xmlChar        *prefix;
#ifdef LIBXML_REGEXP_ENABLED
    /** validating regexp */
    xmlRegexp         *contModel XML_DEPRECATED_MEMBER;
#else
    void	      *contModel XML_DEPRECATED_MEMBER;
#endif
};


/**
 * A namespace declaration node.
 */
#define XML_LOCAL_NAMESPACE XML_NAMESPACE_DECL
typedef xmlElementType xmlNsType;

/** Namespace declaration */
typedef struct _xmlNs xmlNs;
typedef xmlNs *xmlNsPtr;
/**
 * An XML namespace.
 * Note that prefix == NULL is valid, it defines the default namespace
 * within the subtree (until overridden).
 *
 * xmlNsType is unified with xmlElementType.
 *
 * Note that the XPath engine returns XPath namespace nodes as
 * xmlNs cast to xmlNode. This is a terrible design decision that
 * can easily cause type confusion errors. In this case, the `next`
 * member points to the xmlNode element to which the namespace
 * node belongs.
 */
struct _xmlNs {
    /** next namespace */
    struct _xmlNs  *next;
    /** XML_NAMESPACE_DECL */
    xmlNsType      type;
    /** namespace URI */
    const xmlChar *href;
    /** namespace prefix */
    const xmlChar *prefix;
    /** application data */
    void           *_private;
    /** normally an xmlDoc */
    struct _xmlDoc *context XML_DEPRECATED_MEMBER;
};

/** Document type definition (DTD) */
typedef struct _xmlDtd xmlDtd;
typedef xmlDtd *xmlDtdPtr;
/**
 * An XML DTD, as defined by <!DOCTYPE ... There is actually one for
 * the internal subset and for the external subset.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlDtd {
    /** application data */
    void           *_private;
    /** XML_DTD_NODE */
    xmlElementType  type;
    /** name of the DTD */
    const xmlChar *name;
    /** first child */
    struct _xmlNode *children;
    /** last child */
    struct _xmlNode *last;
    /** parent node */
    struct _xmlDoc  *parent;
    /** next sibling */
    struct _xmlNode *next;
    /** previous sibling */
    struct _xmlNode *prev;
    /** containing document */
    struct _xmlDoc  *doc;

    /* End of common part */

    /** hash table for notations if any */
    void          *notations;
    /** hash table for elements if any */
    void          *elements;
    /** hash table for attributes if any */
    void          *attributes;
    /** hash table for entities if any */
    void          *entities;
    /** public identifier */
    xmlChar *ExternalID;
    /** system identifier */
    xmlChar *SystemID;
    /** hash table for parameter entities if any */
    void          *pentities;
};

/** Attribute of an element */
typedef struct _xmlAttr xmlAttr;
typedef xmlAttr *xmlAttrPtr;
/**
 * An attribute of element.
 */
struct _xmlAttr {
    /** application data */
    void           *_private;
    /** XML_ATTRIBUTE_NODE */
    xmlElementType   type;
    /** local name */
    const xmlChar   *name;
    /** first child */
    struct _xmlNode *children;
    /** last child */
    struct _xmlNode *last;
    /** parent node */
    struct _xmlNode *parent;
    /** next sibling */
    struct _xmlAttr *next;
    /** previous sibling */
    struct _xmlAttr *prev;
    /** containing document */
    struct _xmlDoc  *doc;
    /** namespace if any */
    xmlNs           *ns;
    /** attribute type if validating */
    xmlAttributeType atype;
    /** for type/PSVI information */
    void            *psvi;
    /** ID struct if any */
    struct _xmlID   *id XML_DEPRECATED_MEMBER;
};

/** Extra data for ID attributes */
typedef struct _xmlID xmlID;
typedef xmlID *xmlIDPtr;
/**
 * An XML ID instance.
 *
 * Should be treated as opaque. Accessing members directly
 * is deprecated.
 */
struct _xmlID {
    /* next ID */
    struct _xmlID    *next XML_DEPRECATED_MEMBER;
    /* The ID name */
    xmlChar *value XML_DEPRECATED_MEMBER;
    /* The attribute holding it */
    xmlAttr          *attr XML_DEPRECATED_MEMBER;
    /* The attribute if attr is not available */
    const xmlChar    *name XML_DEPRECATED_MEMBER;
    /* The line number if attr is not available */
    int               lineno XML_DEPRECATED_MEMBER;
    /* The document holding the ID */
    struct _xmlDoc   *doc XML_DEPRECATED_MEMBER;
};

/** @cond ignore */
typedef struct _xmlRef xmlRef;
typedef xmlRef *xmlRefPtr;
/*
 * An XML IDREF instance.
 */
struct _xmlRef {
    /* next Ref */
    struct _xmlRef    *next XML_DEPRECATED_MEMBER;
    /* The Ref name */
    const xmlChar     *value XML_DEPRECATED_MEMBER;
    /* The attribute holding it */
    xmlAttr          *attr XML_DEPRECATED_MEMBER;
    /* The attribute if attr is not available */
    const xmlChar    *name XML_DEPRECATED_MEMBER;
    /* The line number if attr is not available */
    int               lineno XML_DEPRECATED_MEMBER;
};
/** @endcond */

/** Generic node type in an XML or HTML tree */
typedef struct _xmlNode xmlNode;
typedef xmlNode *xmlNodePtr;
/**
 * Generic node type in an XML or HTML tree.
 *
 * This is used for
 *
 * - XML_ELEMENT_NODE
 * - XML_TEXT_NODE
 * - XML_CDATA_SECTION_NODE
 * - XML_ENTITY_REF_NODE
 * - XML_PI_NODE
 * - XML_COMMENT_NODE
 * - XML_DOCUMENT_FRAG_NODE
 * - XML_XINCLUDE_START_NODE
 * - XML_XINCLUDE_END_NODE
 *
 * Other node types have a different struct layout than xmlNode,
 * see xmlElementType. Except for XML_NAMESPACE_DECL all nodes
 * share the following members at the same offset:
 *
 * - `_private`
 * - `type` (also for XML_NAMESPACE_DECL)
 * - `name`
 * - `children`
 * - `last`
 * - `parent`
 * - `next`
 * - `prev`
 * - `doc`
 *
 * xmlNode and xmlAttr also share the `ns` member.
 */
struct _xmlNode {
    /** Application data. Often used by language bindings. */
    void           *_private;
    /** Type enum, an xmlElementType value */
    xmlElementType   type;
    /**
     * Name of the node.
     *
     * - Local name of elements or attributes. As a corner case,
     *   this can also contain Names which are invalid QNames in
     *   non-namespace-wellformed documents.
     * - Name of entity references
     * - Target of processing instructions
     * - Fixed string for text and comments
     * - Unused otherwise
     */
    const xmlChar   *name;
    /** First child. Entity declaration of entity references. */
    struct _xmlNode *children;
    /** Last child */
    struct _xmlNode *last;
    /** Parent node. NULL for documents or unlinked root nodes. */
    struct _xmlNode *parent;
    /** Next sibling */
    struct _xmlNode *next;
    /** Previous sibling */
    struct _xmlNode *prev;
    /**
     * Associated document.
     *
     * Used to access DTDs, entities, ID tables, dictionary or
     * other document properties. All children of a node share the
     * same document.
     */
    struct _xmlDoc  *doc;

    /* End of common part */

    /** Namespace of element if any */
    xmlNs           *ns;
    /**
     * Content of text, comment, PI nodes.
     *
     * Sort index for elements after calling #xmlXPathOrderDocElems.
     * Content of internal entities for entity references.
     */
    xmlChar         *content;
    /**
     * First attribute of element.
     *
     * Also used to store small strings with XML_PARSE_COMPACT.
     */
    struct _xmlAttr *properties;
    /** First namespace definition of element */
    xmlNs           *nsDef;
    /** For type/PSVI information */
    void            *psvi;
    /** Line number */
    unsigned short   line;
    /** Extra data for XPath/XSLT */
    unsigned short   extra;
};

/**
 * Set of properties of the document as found by the parser.
 * Some of them are linked to similarly named xmlParserOption.
 */
typedef enum {
    /** document is XML well formed */
    XML_DOC_WELLFORMED		= 1<<0,
    /** document is Namespace valid */
    XML_DOC_NSVALID		= 1<<1,
    /** parsed with old XML-1.0 parser */
    XML_DOC_OLD10		= 1<<2,
    /** DTD validation was successful */
    XML_DOC_DTDVALID		= 1<<3,
    /** XInclude substitution was done */
    XML_DOC_XINCLUDE		= 1<<4,
    /** Document was built using the API and not by parsing an instance */
    XML_DOC_USERBUILT		= 1<<5,
    /** built for internal processing */
    XML_DOC_INTERNAL		= 1<<6,
    /** parsed or built HTML document */
    XML_DOC_HTML		= 1<<7
} xmlDocProperties;

/** XML or HTML document */
typedef struct _xmlDoc xmlDoc;
typedef xmlDoc *xmlDocPtr;
/**
 * An XML or HTML document.
 */
struct _xmlDoc {
    /** application data */
    void           *_private;
    /** XML_DOCUMENT_NODE or XML_HTML_DOCUMENT_NODE */
    xmlElementType  type;
    /** NULL */
    char           *name;
    /** first child */
    struct _xmlNode *children;
    /** last child */
    struct _xmlNode *last;
    /** parent node */
    struct _xmlNode *parent;
    /** next sibling */
    struct _xmlNode *next;
    /** previous sibling */
    struct _xmlNode *prev;
    /** reference to itself */
    struct _xmlDoc  *doc;

    /* End of common part */

    /** level of zlib compression */
    int             compression;
    /**
     * standalone document (no external refs)
     *
     * - 1 if standalone="yes",
     * - 0 if standalone="no",
     * - -1 if there is no XML declaration,
     * - -2 if there is an XML declaration, but no
     *   standalone attribute was specified
     */
    int             standalone;
    /** internal subset */
    struct _xmlDtd  *intSubset;
    /** external subset */
    struct _xmlDtd  *extSubset;
    /** used to hold the XML namespace if needed */
    struct _xmlNs   *oldNs;
    /** version string from XML declaration */
    xmlChar  *version;
    /** actual encoding if any */
    xmlChar  *encoding;
    /** hash table for ID attributes if any */
    void           *ids;
    /** hash table for IDREFs attributes if any */
    void           *refs XML_DEPRECATED_MEMBER;
    /** URI of the document */
    xmlChar  *URL;
    /** unused */
    int             charset;
    /** dict used to allocate names if any */
    struct _xmlDict *dict;
    /** for type/PSVI information */
    void           *psvi;
    /** xmlParserOption enum used to parse the document */
    int             parseFlags;
    /** xmlDocProperties of the document */
    int             properties;
};


/** Context for DOM wrapper operations */
typedef struct _xmlDOMWrapCtxt xmlDOMWrapCtxt;
typedef xmlDOMWrapCtxt *xmlDOMWrapCtxtPtr;

/**
 * A function called to acquire namespaces (xmlNs) from the wrapper.
 *
 * @param ctxt  a DOM wrapper context
 * @param node  the context node (element or attribute)
 * @param nsName  the requested namespace name
 * @param nsPrefix  the requested namespace prefix
 * @returns an xmlNs or NULL in case of an error.
 */
typedef xmlNs *(*xmlDOMWrapAcquireNsFunction) (xmlDOMWrapCtxt *ctxt,
						 xmlNode *node,
						 const xmlChar *nsName,
						 const xmlChar *nsPrefix);

/**
 * Context for DOM wrapper-operations.
 */
struct _xmlDOMWrapCtxt {
    void * _private;
    /*
    * The type of this context, just in case we need specialized
    * contexts in the future.
    */
    int type;
    /*
    * Internal namespace map used for various operations.
    */
    void * namespaceMap;
    /*
    * Use this one to acquire an xmlNs intended for node->ns.
    * (Note that this is not intended for elem->nsDef).
    */
    xmlDOMWrapAcquireNsFunction getNsForNodeFunc;
};

/**
 * Signature for the registration callback of a created node
 *
 * @param node  the current node
 */
typedef void (*xmlRegisterNodeFunc) (xmlNode *node);

/**
 * Signature for the deregistration callback of a discarded node
 *
 * @param node  the current node
 */
typedef void (*xmlDeregisterNodeFunc) (xmlNode *node);

/**
 * Macro for compatibility naming layer with libxml1. Maps
 * to "children."
 */
#ifndef xmlChildrenNode
#define xmlChildrenNode children
#endif

/**
 * Macro for compatibility naming layer with libxml1. Maps
 * to "children".
 */
#ifndef xmlRootNode
#define xmlRootNode children
#endif

/*
 * Variables.
 */

/** @cond ignore */

XML_DEPRECATED
XMLPUBFUN xmlRegisterNodeFunc *__xmlRegisterNodeDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN xmlDeregisterNodeFunc *__xmlDeregisterNodeDefaultValue(void);

#ifndef XML_GLOBALS_NO_REDEFINITION
  #define xmlRegisterNodeDefaultValue \
    (*__xmlRegisterNodeDefaultValue())
  #define xmlDeregisterNodeDefaultValue \
    (*__xmlDeregisterNodeDefaultValue())
#endif

/** @endcond */

/*
 * Some helper functions
 */
XMLPUBFUN int
		xmlValidateNCName	(const xmlChar *value,
					 int space);

XMLPUBFUN int
		xmlValidateQName	(const xmlChar *value,
					 int space);
XMLPUBFUN int
		xmlValidateName		(const xmlChar *value,
					 int space);
XMLPUBFUN int
		xmlValidateNMToken	(const xmlChar *value,
					 int space);

XMLPUBFUN xmlChar *
		xmlBuildQName		(const xmlChar *ncname,
					 const xmlChar *prefix,
					 xmlChar *memory,
					 int len);
XMLPUBFUN xmlChar *
		xmlSplitQName2		(const xmlChar *name,
					 xmlChar **prefix);
XMLPUBFUN const xmlChar *
		xmlSplitQName3		(const xmlChar *name,
					 int *len);

/*
 * Creating/freeing new structures.
 */
XMLPUBFUN xmlDtd *
		xmlCreateIntSubset	(xmlDoc *doc,
					 const xmlChar *name,
					 const xmlChar *publicId,
					 const xmlChar *systemId);
XMLPUBFUN xmlDtd *
		xmlNewDtd		(xmlDoc *doc,
					 const xmlChar *name,
					 const xmlChar *publicId,
					 const xmlChar *systemId);
XMLPUBFUN xmlDtd *
		xmlGetIntSubset		(const xmlDoc *doc);
XMLPUBFUN void
		xmlFreeDtd		(xmlDtd *cur);
XMLPUBFUN xmlNs *
		xmlNewNs		(xmlNode *node,
					 const xmlChar *href,
					 const xmlChar *prefix);
XMLPUBFUN void
		xmlFreeNs		(xmlNs *cur);
XMLPUBFUN void
		xmlFreeNsList		(xmlNs *cur);
XMLPUBFUN xmlDoc *
		xmlNewDoc		(const xmlChar *version);
XMLPUBFUN void
		xmlFreeDoc		(xmlDoc *cur);
XMLPUBFUN xmlAttr *
		xmlNewDocProp		(xmlDoc *doc,
					 const xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN xmlAttr *
		xmlNewProp		(xmlNode *node,
					 const xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN xmlAttr *
		xmlNewNsProp		(xmlNode *node,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN xmlAttr *
		xmlNewNsPropEatName	(xmlNode *node,
					 xmlNs *ns,
					 xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN void
		xmlFreePropList		(xmlAttr *cur);
XMLPUBFUN void
		xmlFreeProp		(xmlAttr *cur);
XMLPUBFUN xmlAttr *
		xmlCopyProp		(xmlNode *target,
					 xmlAttr *cur);
XMLPUBFUN xmlAttr *
		xmlCopyPropList		(xmlNode *target,
					 xmlAttr *cur);
XMLPUBFUN xmlDtd *
		xmlCopyDtd		(xmlDtd *dtd);
XMLPUBFUN xmlDoc *
		xmlCopyDoc		(xmlDoc *doc,
					 int recursive);
/*
 * Creating new nodes.
 */
XMLPUBFUN xmlNode *
		xmlNewDocNode		(xmlDoc *doc,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocNodeEatName	(xmlDoc *doc,
					 xmlNs *ns,
					 xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewNode		(xmlNs *ns,
					 const xmlChar *name);
XMLPUBFUN xmlNode *
		xmlNewNodeEatName	(xmlNs *ns,
					 xmlChar *name);
XMLPUBFUN xmlNode *
		xmlNewChild		(xmlNode *parent,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocText		(const xmlDoc *doc,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewText		(const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocPI		(xmlDoc *doc,
					 const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewPI		(const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocTextLen	(xmlDoc *doc,
					 const xmlChar *content,
					 int len);
XMLPUBFUN xmlNode *
		xmlNewTextLen		(const xmlChar *content,
					 int len);
XMLPUBFUN xmlNode *
		xmlNewDocComment	(xmlDoc *doc,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewComment		(const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewCDataBlock	(xmlDoc *doc,
					 const xmlChar *content,
					 int len);
XMLPUBFUN xmlNode *
		xmlNewCharRef		(xmlDoc *doc,
					 const xmlChar *name);
XMLPUBFUN xmlNode *
		xmlNewReference		(const xmlDoc *doc,
					 const xmlChar *name);
XMLPUBFUN xmlNode *
		xmlCopyNode		(xmlNode *node,
					 int recursive);
XMLPUBFUN xmlNode *
		xmlDocCopyNode		(xmlNode *node,
					 xmlDoc *doc,
					 int recursive);
XMLPUBFUN xmlNode *
		xmlDocCopyNodeList	(xmlDoc *doc,
					 xmlNode *node);
XMLPUBFUN xmlNode *
		xmlCopyNodeList		(xmlNode *node);
XMLPUBFUN xmlNode *
		xmlNewTextChild		(xmlNode *parent,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocRawNode	(xmlDoc *doc,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *content);
XMLPUBFUN xmlNode *
		xmlNewDocFragment	(xmlDoc *doc);

/*
 * Navigating.
 */
XMLPUBFUN long
		xmlGetLineNo		(const xmlNode *node);
XMLPUBFUN xmlChar *
		xmlGetNodePath		(const xmlNode *node);
XMLPUBFUN xmlNode *
		xmlDocGetRootElement	(const xmlDoc *doc);
XMLPUBFUN xmlNode *
		xmlGetLastChild		(const xmlNode *parent);
XMLPUBFUN int
		xmlNodeIsText		(const xmlNode *node);
XMLPUBFUN int
		xmlIsBlankNode		(const xmlNode *node);

/*
 * Changing the structure.
 */
XMLPUBFUN xmlNode *
		xmlDocSetRootElement	(xmlDoc *doc,
					 xmlNode *root);
XMLPUBFUN void
		xmlNodeSetName		(xmlNode *cur,
					 const xmlChar *name);
XMLPUBFUN xmlNode *
		xmlAddChild		(xmlNode *parent,
					 xmlNode *cur);
XMLPUBFUN xmlNode *
		xmlAddChildList		(xmlNode *parent,
					 xmlNode *cur);
XMLPUBFUN xmlNode *
		xmlReplaceNode		(xmlNode *old,
					 xmlNode *cur);
XMLPUBFUN xmlNode *
		xmlAddPrevSibling	(xmlNode *cur,
					 xmlNode *elem);
XMLPUBFUN xmlNode *
		xmlAddSibling		(xmlNode *cur,
					 xmlNode *elem);
XMLPUBFUN xmlNode *
		xmlAddNextSibling	(xmlNode *cur,
					 xmlNode *elem);
XMLPUBFUN void
		xmlUnlinkNode		(xmlNode *cur);
XMLPUBFUN xmlNode *
		xmlTextMerge		(xmlNode *first,
					 xmlNode *second);
XMLPUBFUN int
		xmlTextConcat		(xmlNode *node,
					 const xmlChar *content,
					 int len);
XMLPUBFUN void
		xmlFreeNodeList		(xmlNode *cur);
XMLPUBFUN void
		xmlFreeNode		(xmlNode *cur);
XMLPUBFUN int
		xmlSetTreeDoc		(xmlNode *tree,
					 xmlDoc *doc);
XMLPUBFUN int
		xmlSetListDoc		(xmlNode *list,
					 xmlDoc *doc);
/*
 * Namespaces.
 */
XMLPUBFUN xmlNs *
		xmlSearchNs		(xmlDoc *doc,
					 xmlNode *node,
					 const xmlChar *nameSpace);
XMLPUBFUN xmlNs *
		xmlSearchNsByHref	(xmlDoc *doc,
					 xmlNode *node,
					 const xmlChar *href);
XMLPUBFUN int
		xmlGetNsListSafe	(const xmlDoc *doc,
					 const xmlNode *node,
					 xmlNs ***out);
XMLPUBFUN xmlNs **
		xmlGetNsList		(const xmlDoc *doc,
					 const xmlNode *node);

XMLPUBFUN void
		xmlSetNs		(xmlNode *node,
					 xmlNs *ns);
XMLPUBFUN xmlNs *
		xmlCopyNamespace	(xmlNs *cur);
XMLPUBFUN xmlNs *
		xmlCopyNamespaceList	(xmlNs *cur);

/*
 * Changing the content.
 */
XMLPUBFUN xmlAttr *
		xmlSetProp		(xmlNode *node,
					 const xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN xmlAttr *
		xmlSetNsProp		(xmlNode *node,
					 xmlNs *ns,
					 const xmlChar *name,
					 const xmlChar *value);
XMLPUBFUN int
		xmlNodeGetAttrValue	(const xmlNode *node,
					 const xmlChar *name,
					 const xmlChar *nsUri,
					 xmlChar **out);
XMLPUBFUN xmlChar *
		xmlGetNoNsProp		(const xmlNode *node,
					 const xmlChar *name);
XMLPUBFUN xmlChar *
		xmlGetProp		(const xmlNode *node,
					 const xmlChar *name);
XMLPUBFUN xmlAttr *
		xmlHasProp		(const xmlNode *node,
					 const xmlChar *name);
XMLPUBFUN xmlAttr *
		xmlHasNsProp		(const xmlNode *node,
					 const xmlChar *name,
					 const xmlChar *nameSpace);
XMLPUBFUN xmlChar *
		xmlGetNsProp		(const xmlNode *node,
					 const xmlChar *name,
					 const xmlChar *nameSpace);
XMLPUBFUN xmlNode *
		xmlStringGetNodeList	(const xmlDoc *doc,
					 const xmlChar *value);
XMLPUBFUN xmlNode *
		xmlStringLenGetNodeList	(const xmlDoc *doc,
					 const xmlChar *value,
					 int len);
XMLPUBFUN xmlChar *
		xmlNodeListGetString	(xmlDoc *doc,
					 const xmlNode *list,
					 int inLine);
XMLPUBFUN xmlChar *
		xmlNodeListGetRawString	(const xmlDoc *doc,
					 const xmlNode *list,
					 int inLine);
XMLPUBFUN int
		xmlNodeSetContent	(xmlNode *cur,
					 const xmlChar *content);
XMLPUBFUN int
		xmlNodeSetContentLen	(xmlNode *cur,
					 const xmlChar *content,
					 int len);
XMLPUBFUN int
		xmlNodeAddContent	(xmlNode *cur,
					 const xmlChar *content);
XMLPUBFUN int
		xmlNodeAddContentLen	(xmlNode *cur,
					 const xmlChar *content,
					 int len);
XMLPUBFUN xmlChar *
		xmlNodeGetContent	(const xmlNode *cur);

XMLPUBFUN int
		xmlNodeBufGetContent	(xmlBuffer *buffer,
					 const xmlNode *cur);
XMLPUBFUN int
		xmlBufGetNodeContent	(xmlBuf *buf,
					 const xmlNode *cur);

XMLPUBFUN xmlChar *
		xmlNodeGetLang		(const xmlNode *cur);
XMLPUBFUN int
		xmlNodeGetSpacePreserve	(const xmlNode *cur);
XMLPUBFUN int
		xmlNodeSetLang		(xmlNode *cur,
					 const xmlChar *lang);
XMLPUBFUN int
		xmlNodeSetSpacePreserve (xmlNode *cur,
					 int val);
XMLPUBFUN int
		xmlNodeGetBaseSafe	(const xmlDoc *doc,
					 const xmlNode *cur,
					 xmlChar **baseOut);
XMLPUBFUN xmlChar *
		xmlNodeGetBase		(const xmlDoc *doc,
					 const xmlNode *cur);
XMLPUBFUN int
		xmlNodeSetBase		(xmlNode *cur,
					 const xmlChar *uri);

/*
 * Removing content.
 */
XMLPUBFUN int
		xmlRemoveProp		(xmlAttr *cur);
XMLPUBFUN int
		xmlUnsetNsProp		(xmlNode *node,
					 xmlNs *ns,
					 const xmlChar *name);
XMLPUBFUN int
		xmlUnsetProp		(xmlNode *node,
					 const xmlChar *name);

#ifdef LIBXML_OUTPUT_ENABLED
XMLPUBFUN void xmlAttrSerializeTxtContent(xmlBuffer *buf,
					 xmlDoc *doc,
					 xmlAttr *attr,
					 const xmlChar *string);
#endif /* LIBXML_OUTPUT_ENABLED */

/*
 * Namespace handling.
 */
XMLPUBFUN int
		xmlReconciliateNs	(xmlDoc *doc,
					 xmlNode *tree);

#ifdef LIBXML_OUTPUT_ENABLED
/*
 * Saving.
 */
XMLPUBFUN void
		xmlDocDumpFormatMemory	(xmlDoc *cur,
					 xmlChar **mem,
					 int *size,
					 int format);
XMLPUBFUN void
		xmlDocDumpMemory	(xmlDoc *cur,
					 xmlChar **mem,
					 int *size);
XMLPUBFUN void
		xmlDocDumpMemoryEnc	(xmlDoc *out_doc,
					 xmlChar **doc_txt_ptr,
					 int * doc_txt_len,
					 const char *txt_encoding);
XMLPUBFUN void
		xmlDocDumpFormatMemoryEnc(xmlDoc *out_doc,
					 xmlChar **doc_txt_ptr,
					 int * doc_txt_len,
					 const char *txt_encoding,
					 int format);
XMLPUBFUN int
		xmlDocFormatDump	(FILE *f,
					 xmlDoc *cur,
					 int format);
XMLPUBFUN int
		xmlDocDump		(FILE *f,
					 xmlDoc *cur);
XMLPUBFUN void
		xmlElemDump		(FILE *f,
					 xmlDoc *doc,
					 xmlNode *cur);
XMLPUBFUN int
		xmlSaveFile		(const char *filename,
					 xmlDoc *cur);
XMLPUBFUN int
		xmlSaveFormatFile	(const char *filename,
					 xmlDoc *cur,
					 int format);
XMLPUBFUN size_t
		xmlBufNodeDump		(xmlBuf *buf,
					 xmlDoc *doc,
					 xmlNode *cur,
					 int level,
					 int format);
XMLPUBFUN int
		xmlNodeDump		(xmlBuffer *buf,
					 xmlDoc *doc,
					 xmlNode *cur,
					 int level,
					 int format);

XMLPUBFUN int
		xmlSaveFileTo		(xmlOutputBuffer *buf,
					 xmlDoc *cur,
					 const char *encoding);
XMLPUBFUN int
		xmlSaveFormatFileTo     (xmlOutputBuffer *buf,
					 xmlDoc *cur,
				         const char *encoding,
				         int format);
XMLPUBFUN void
		xmlNodeDumpOutput	(xmlOutputBuffer *buf,
					 xmlDoc *doc,
					 xmlNode *cur,
					 int level,
					 int format,
					 const char *encoding);

XMLPUBFUN int
		xmlSaveFormatFileEnc    (const char *filename,
					 xmlDoc *cur,
					 const char *encoding,
					 int format);

XMLPUBFUN int
		xmlSaveFileEnc		(const char *filename,
					 xmlDoc *cur,
					 const char *encoding);

#endif /* LIBXML_OUTPUT_ENABLED */
/*
 * XHTML
 */
XMLPUBFUN int
		xmlIsXHTML		(const xmlChar *systemID,
					 const xmlChar *publicID);

/*
 * Compression.
 */
XMLPUBFUN int
		xmlGetDocCompressMode	(const xmlDoc *doc);
XMLPUBFUN void
		xmlSetDocCompressMode	(xmlDoc *doc,
					 int mode);
XML_DEPRECATED
XMLPUBFUN int
		xmlGetCompressMode	(void);
XML_DEPRECATED
XMLPUBFUN void
		xmlSetCompressMode	(int mode);

/*
* DOM-wrapper helper functions.
*/
XMLPUBFUN xmlDOMWrapCtxt *
		xmlDOMWrapNewCtxt	(void);
XMLPUBFUN void
		xmlDOMWrapFreeCtxt	(xmlDOMWrapCtxt *ctxt);
XMLPUBFUN int
	    xmlDOMWrapReconcileNamespaces(xmlDOMWrapCtxt *ctxt,
					 xmlNode *elem,
					 int options);
XMLPUBFUN int
	    xmlDOMWrapAdoptNode		(xmlDOMWrapCtxt *ctxt,
					 xmlDoc *sourceDoc,
					 xmlNode *node,
					 xmlDoc *destDoc,
					 xmlNode *destParent,
					 int options);
XMLPUBFUN int
	    xmlDOMWrapRemoveNode	(xmlDOMWrapCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *node,
					 int options);
XMLPUBFUN int
	    xmlDOMWrapCloneNode		(xmlDOMWrapCtxt *ctxt,
					 xmlDoc *sourceDoc,
					 xmlNode *node,
					 xmlNode **clonedNode,
					 xmlDoc *destDoc,
					 xmlNode *destParent,
					 int deep,
					 int options);

/*
 * 5 interfaces from DOM ElementTraversal, but different in entities
 * traversal.
 */
XMLPUBFUN unsigned long
            xmlChildElementCount        (xmlNode *parent);
XMLPUBFUN xmlNode *
            xmlNextElementSibling       (xmlNode *node);
XMLPUBFUN xmlNode *
            xmlFirstElementChild        (xmlNode *parent);
XMLPUBFUN xmlNode *
            xmlLastElementChild         (xmlNode *parent);
XMLPUBFUN xmlNode *
            xmlPreviousElementSibling   (xmlNode *node);

XML_DEPRECATED
XMLPUBFUN xmlRegisterNodeFunc
	    xmlRegisterNodeDefault	(xmlRegisterNodeFunc func);
XML_DEPRECATED
XMLPUBFUN xmlDeregisterNodeFunc
	    xmlDeregisterNodeDefault	(xmlDeregisterNodeFunc func);
XML_DEPRECATED
XMLPUBFUN xmlRegisterNodeFunc
            xmlThrDefRegisterNodeDefault(xmlRegisterNodeFunc func);
XML_DEPRECATED
XMLPUBFUN xmlDeregisterNodeFunc
            xmlThrDefDeregisterNodeDefault(xmlDeregisterNodeFunc func);

/*
 * Handling Buffers, the old ones see `xmlBuf` for the new ones.
 */

XML_DEPRECATED
XMLPUBFUN void
		xmlSetBufferAllocationScheme(xmlBufferAllocationScheme scheme);
XML_DEPRECATED
XMLPUBFUN xmlBufferAllocationScheme
		xmlGetBufferAllocationScheme(void);

XMLPUBFUN xmlBuffer *
		xmlBufferCreate		(void);
XMLPUBFUN xmlBuffer *
		xmlBufferCreateSize	(size_t size);
XMLPUBFUN xmlBuffer *
		xmlBufferCreateStatic	(void *mem,
					 size_t size);
XML_DEPRECATED
XMLPUBFUN int
		xmlBufferResize		(xmlBuffer *buf,
					 unsigned int size);
XMLPUBFUN void
		xmlBufferFree		(xmlBuffer *buf);
XMLPUBFUN int
		xmlBufferDump		(FILE *file,
					 xmlBuffer *buf);
XMLPUBFUN int
		xmlBufferAdd		(xmlBuffer *buf,
					 const xmlChar *str,
					 int len);
XMLPUBFUN int
		xmlBufferAddHead	(xmlBuffer *buf,
					 const xmlChar *str,
					 int len);
XMLPUBFUN int
		xmlBufferCat		(xmlBuffer *buf,
					 const xmlChar *str);
XMLPUBFUN int
		xmlBufferCCat		(xmlBuffer *buf,
					 const char *str);
XML_DEPRECATED
XMLPUBFUN int
		xmlBufferShrink		(xmlBuffer *buf,
					 unsigned int len);
XML_DEPRECATED
XMLPUBFUN int
		xmlBufferGrow		(xmlBuffer *buf,
					 unsigned int len);
XMLPUBFUN void
		xmlBufferEmpty		(xmlBuffer *buf);
XMLPUBFUN const xmlChar*
		xmlBufferContent	(const xmlBuffer *buf);
XMLPUBFUN xmlChar*
		xmlBufferDetach         (xmlBuffer *buf);
XMLPUBFUN void
		xmlBufferSetAllocationScheme(xmlBuffer *buf,
					 xmlBufferAllocationScheme scheme);
XMLPUBFUN int
		xmlBufferLength		(const xmlBuffer *buf);
XMLPUBFUN void
		xmlBufferWriteCHAR	(xmlBuffer *buf,
					 const xmlChar *string);
XMLPUBFUN void
		xmlBufferWriteChar	(xmlBuffer *buf,
					 const char *string);
XMLPUBFUN void
		xmlBufferWriteQuotedString(xmlBuffer *buf,
					 const xmlChar *string);

/*
 * A few public routines for xmlBuf. As those are expected to be used
 * mostly internally the bulk of the routines are internal in buf.h
 */
XMLPUBFUN xmlChar*       xmlBufContent	(const xmlBuf* buf);
XMLPUBFUN xmlChar*       xmlBufEnd      (xmlBuf *buf);
XMLPUBFUN size_t         xmlBufUse      (xmlBuf *buf);
XMLPUBFUN size_t         xmlBufShrink	(xmlBuf *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __XML_TREE_H__ */

#endif /* XML_TREE_INTERNALS */

