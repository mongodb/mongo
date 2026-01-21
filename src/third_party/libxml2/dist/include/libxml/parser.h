/**
 * @file
 * 
 * @brief Validating XML 1.0 parser
 * 
 * Interfaces, constants and types related to the XML parser.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_PARSER_H__
#define __XML_PARSER_H__

#include <libxml/xmlversion.h>
/** @cond ignore */
#define XML_TREE_INTERNALS
/** @endcond */
#include <libxml/tree.h>
#undef XML_TREE_INTERNALS
#include <libxml/dict.h>
#include <libxml/hash.h>
#include <libxml/valid.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlstring.h>
#include <libxml/xmlmemory.h>
#include <libxml/encoding.h>
#include <libxml/xmlIO.h>
/* for compatibility */
#include <libxml/SAX2.h>
#include <libxml/threads.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The default version of XML used: 1.0
 */
#define XML_DEFAULT_VERSION	"1.0"

/**
 * Status after parsing a document
 */
typedef enum {
    /** not well-formed */
    XML_STATUS_NOT_WELL_FORMED          = (1 << 0),
    /** not namespace-well-formed */
    XML_STATUS_NOT_NS_WELL_FORMED       = (1 << 1),
    /** DTD validation failed */
    XML_STATUS_DTD_VALIDATION_FAILED    = (1 << 2),
    /** catastrophic failure like OOM or I/O error */
    XML_STATUS_CATASTROPHIC_ERROR       = (1 << 3)
} xmlParserStatus;

/**
 * Resource type for resource loaders
 */
typedef enum {
    /** unknown */
    XML_RESOURCE_UNKNOWN = 0,
    /** main document */
    XML_RESOURCE_MAIN_DOCUMENT,
    /** external DTD */
    XML_RESOURCE_DTD,
    /** external general entity */
    XML_RESOURCE_GENERAL_ENTITY,
    /** external parameter entity */
    XML_RESOURCE_PARAMETER_ENTITY,
    /** XIncluded document */
    XML_RESOURCE_XINCLUDE,
    /** XIncluded text */
    XML_RESOURCE_XINCLUDE_TEXT
} xmlResourceType;

/**
 * Flags for parser input
 */
typedef enum {
    /** The input buffer won't be changed during parsing. */
    XML_INPUT_BUF_STATIC            = (1 << 1),
    /** The input buffer is zero-terminated. (Note that the zero
        byte shouldn't be included in buffer size.) */
    XML_INPUT_BUF_ZERO_TERMINATED   = (1 << 2),
    /** Uncompress gzipped file input */
    XML_INPUT_UNZIP                 = (1 << 3),
    /** Allow network access. Unused internally. */
    XML_INPUT_NETWORK               = (1 << 4),
    /** Allow system catalog to resolve URIs. */
    XML_INPUT_USE_SYS_CATALOG       = (1 << 5)
} xmlParserInputFlags;

/* Deprecated */
typedef void (* xmlParserInputDeallocate)(xmlChar *str);

/**
 * Parser input
 */
struct _xmlParserInput {
    /* Input buffer */
    xmlParserInputBuffer *buf XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use #xmlCtxtGetInputPosition
     *
     * The filename or URI, if any
     */
    const char *filename;
    /* unused */
    const char *directory XML_DEPRECATED_MEMBER;
    /* Base of the array to parse */
    const xmlChar *base;
    /**
     * @deprecated Use #xmlCtxtGetInputWindow
     *
     * Current char being parsed
     */
    const xmlChar *cur;
    /* end of the array to parse */
    const xmlChar *end;
    /* unused */
    int length XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use #xmlCtxtGetInputPosition
     *
     * Current line
     */
    int line;
    /**
     * @deprecated Use #xmlCtxtGetInputPosition
     *
     * Current column
     */
    int col;
    /**
     * @deprecated Use #xmlCtxtGetInputPosition
     *
     * How many xmlChars already consumed
     */
    unsigned long consumed;
    /* function to deallocate the base */
    xmlParserInputDeallocate free XML_DEPRECATED_MEMBER;
    /* unused */
    const xmlChar *encoding XML_DEPRECATED_MEMBER;
    /* the version string for entity */
    const xmlChar *version XML_DEPRECATED_MEMBER;
    /* Flags */
    int flags XML_DEPRECATED_MEMBER;
    /* an unique identifier for the entity, unused internally */
    int id XML_DEPRECATED_MEMBER;
    /* unused */
    unsigned long parentConsumed XML_DEPRECATED_MEMBER;
    /* entity, if any */
    xmlEntity *entity XML_DEPRECATED_MEMBER;
};

/** @cond ignore */

typedef struct _xmlParserNodeInfo xmlParserNodeInfo;
typedef xmlParserNodeInfo *xmlParserNodeInfoPtr;

struct _xmlParserNodeInfo {
  const struct _xmlNode* node;
  /* Position & line # that text that created the node begins & ends on */
  unsigned long begin_pos;
  unsigned long begin_line;
  unsigned long end_pos;
  unsigned long end_line;
};

typedef struct _xmlParserNodeInfoSeq xmlParserNodeInfoSeq;
typedef xmlParserNodeInfoSeq *xmlParserNodeInfoSeqPtr;
struct _xmlParserNodeInfoSeq {
  unsigned long maximum;
  unsigned long length;
  xmlParserNodeInfo* buffer;
};

/*
 * Internal type
 */
typedef enum {
    XML_PARSER_EOF = -1,	/* nothing is to be parsed */
    XML_PARSER_START = 0,	/* nothing has been parsed */
    XML_PARSER_MISC,		/* Misc* before int subset */
    XML_PARSER_PI,		/* Within a processing instruction */
    XML_PARSER_DTD,		/* within some DTD content */
    XML_PARSER_PROLOG,		/* Misc* after internal subset */
    XML_PARSER_COMMENT,		/* within a comment */
    XML_PARSER_START_TAG,	/* within a start tag */
    XML_PARSER_CONTENT,		/* within the content */
    XML_PARSER_CDATA_SECTION,	/* within a CDATA section */
    XML_PARSER_END_TAG,		/* within a closing tag */
    XML_PARSER_ENTITY_DECL,	/* within an entity declaration */
    XML_PARSER_ENTITY_VALUE,	/* within an entity value in a decl */
    XML_PARSER_ATTRIBUTE_VALUE,	/* within an attribute value */
    XML_PARSER_SYSTEM_LITERAL,	/* within a SYSTEM value */
    XML_PARSER_EPILOG,		/* the Misc* after the last end tag */
    XML_PARSER_IGNORE,		/* within an IGNORED section */
    XML_PARSER_PUBLIC_LITERAL,	/* within a PUBLIC value */
    XML_PARSER_XML_DECL         /* before XML decl (but after BOM) */
} xmlParserInputState;

/*
 * Internal bits in the 'loadsubset' context member
 */
#define XML_DETECT_IDS		2
#define XML_COMPLETE_ATTRS	4
#define XML_SKIP_IDS		8

/*
 * Internal type. Only XML_PARSE_READER is used.
 */
typedef enum {
    XML_PARSE_UNKNOWN = 0,
    XML_PARSE_DOM = 1,
    XML_PARSE_SAX = 2,
    XML_PARSE_PUSH_DOM = 3,
    XML_PARSE_PUSH_SAX = 4,
    XML_PARSE_READER = 5
} xmlParserMode;

typedef struct _xmlStartTag xmlStartTag;
typedef struct _xmlParserNsData xmlParserNsData;
typedef struct _xmlAttrHashBucket xmlAttrHashBucket;

/** @endcond */

/**
 * Callback for custom resource loaders.
 *
 * `flags` can contain XML_INPUT_UNZIP and XML_INPUT_NETWORK.
 *
 * The URL is resolved using XML catalogs before being passed to
 * the callback.
 *
 * On success, `out` should be set to a new parser input object and
 * XML_ERR_OK should be returned.
 *
 * @param ctxt  parser context
 * @param url  URL or system ID to load
 * @param publicId  publid ID from DTD (optional)
 * @param type  resource type
 * @param flags  flags
 * @param out  result pointer
 * @returns an xmlParserErrors code.
 */
typedef xmlParserErrors
(*xmlResourceLoader)(void *ctxt, const char *url, const char *publicId,
                     xmlResourceType type, xmlParserInputFlags flags,
                     xmlParserInput **out);

/**
 * Parser context
 */
struct _xmlParserCtxt {
    /**
     * @deprecated Use xmlCtxtGetSaxHandler() and
     * xmlCtxtSetSaxHandler().
     *
     * the SAX handler
     */
    struct _xmlSAXHandler *sax;
    /**
     * @deprecated Use #xmlCtxtGetUserData
     *
     * user data for SAX interface, defaults to the context itself
     */
    void *userData;
    /**
     * @deprecated Use xmlCtxtGetDocument()
     *
     * the document being built
     */
    xmlDoc *myDoc;
    /**
     * @deprecated Use xmlCtxtGetStatus()
     *
     * is the document well formed?
     */
    int wellFormed;
    /**
     * @deprecated Use xmlParserOption XML_PARSE_NOENT
     *
     * shall we replace entities?
     */
    int replaceEntities XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtGetVersion()
     *
     * the XML version string
     */
    xmlChar *version;
    /**
     * @deprecated Use xmlCtxtGetDeclaredEncoding()
     *
     * the declared encoding, if any
     */
    xmlChar *encoding;
    /**
     * @deprecated Use xmlCtxtGetStandalone()
     *
     * standalone document
     */
    int standalone;

    /**
     * @deprecated Use xmlCtxtIsHtml()
     *
     * non-zero for HTML documents, actually an htmlInsertMode
     */
    int html;

    /* Input stream stack */

    /**
     * Current input stream
     */
    /* TODO: Add accessors, see issue #762 */
    xmlParserInput *input;
    /* Number of current input streams */
    int inputNr;
    /* Max number of input streams */
    int inputMax XML_DEPRECATED_MEMBER;
    /* stack of inputs */
    xmlParserInput **inputTab;

    /* Node analysis stack only used for DOM building */

    /**
     * @deprecated Use #xmlCtxtGetNode
     *
     * The current element.
     */
    xmlNode *node;
    /* Depth of the parsing stack */
    int nodeNr XML_DEPRECATED_MEMBER;
    /* Max depth of the parsing stack */
    int nodeMax XML_DEPRECATED_MEMBER;
    /* array of nodes */
    xmlNode **nodeTab XML_DEPRECATED_MEMBER;

    /* Whether node info should be kept */
    int record_info;
    /* info about each node parsed */
    xmlParserNodeInfoSeq node_seq XML_DEPRECATED_MEMBER;

    /**
     * @deprecated Use xmlCtxtGetLastError()
     *
     * error code
     */
    int errNo;

    /* reference and external subset */
    int hasExternalSubset XML_DEPRECATED_MEMBER;
    /* the internal subset has PE refs */
    int hasPErefs XML_DEPRECATED_MEMBER;
    /* unused */
    int external XML_DEPRECATED_MEMBER;

    /**
     * @deprecated Use xmlCtxtGetStatus()
     *
     * is the document valid
     */
    int valid;
    /**
     * @deprecated Use xmlParserOption XML_PARSE_DTDVALID
     *
     * shall we try to validate?
     */
    int validate XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtGetValidCtxt()
     *
     * The validity context
     */
    xmlValidCtxt vctxt;

    /* push parser state */
    xmlParserInputState instate XML_DEPRECATED_MEMBER;
    /* unused */
    int token XML_DEPRECATED_MEMBER;

    /**
     * @deprecated Don't use
     *
     * The main document URI, if available, with its last
     * component stripped.
     */
    char *directory;

    /* Node name stack */

    /* Current parsed Node */
    const xmlChar *name XML_DEPRECATED_MEMBER;
    /* Depth of the parsing stack */
    int nameNr XML_DEPRECATED_MEMBER;
    /* Max depth of the parsing stack */
    int nameMax XML_DEPRECATED_MEMBER;
    /* array of nodes */
    const xmlChar **nameTab XML_DEPRECATED_MEMBER;

    /* unused */
    long nbChars XML_DEPRECATED_MEMBER;
    /* used by progressive parsing lookup */
    long checkIndex XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use inverted xmlParserOption XML_PARSE_NOBLANKS
     *
     * ugly but ...
     */
    int keepBlanks XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtIsStopped()
     *
     * SAX callbacks are disabled
     */
    int disableSAX XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtIsInSubset
     *
     * Set if DTD content is parsed.
     *
     * - 0: not in DTD
     * - 1: in internal DTD subset
     * - 2: in external DTD subset
     */
    int inSubset;
    /**
     * @deprecated Use the `name` argument of the
     * `internalSubset` SAX callback or #xmlCtxtGetDocTypeDecl
     *
     * Name of the internal subset (root element type).
     */
    const xmlChar *intSubName;
    /**
     * @deprecated Use the `systemId` argument of the
     * `internalSubset` SAX callback or #xmlCtxtGetDocTypeDecl
     *
     * System identifier (URI) of external the subset.
     */
    xmlChar *extSubURI;
    /**
     * @deprecated Use the `publicId` argument of the
     * `internalSubset` SAX callback or #xmlCtxtGetDocTypeDecl
     *
     * This member is MISNAMED. It contains the *public* identifier
     * of the external subset.
     */
    xmlChar *extSubSystem;

    /* xml:space values */

    /* Should the parser preserve spaces */
    int *space XML_DEPRECATED_MEMBER;
    /* Depth of the parsing stack */
    int spaceNr XML_DEPRECATED_MEMBER;
    /* Max depth of the parsing stack */
    int spaceMax XML_DEPRECATED_MEMBER;
    /* array of space infos */
    int *spaceTab XML_DEPRECATED_MEMBER;

    /* to prevent entity substitution loops */
    int depth XML_DEPRECATED_MEMBER;
    /* unused */
    xmlParserInput *entity XML_DEPRECATED_MEMBER;
    /* unused */
    int charset XML_DEPRECATED_MEMBER;
    /* Those two fields are there to speed up large node parsing */
    int nodelen XML_DEPRECATED_MEMBER;
    int nodemem XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlParserOption XML_PARSE_PEDANTIC
     *
     * signal pedantic warnings
     */
    int pedantic XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtGetPrivate() and xmlCtxtSetPrivate()
     *
     * For user data, libxml won't touch it
     */
    void *_private;
    /**
     * @deprecated Use xmlParserOption XML_PARSE_DTDLOAD,
     * XML_PARSE_DTDATTR or XML_PARSE_SKIP_IDS.
     *
     * Control loading of the external subset and handling of IDs.
     * Other options like `validate` can override this value.
     *
     * - 0: The default behavior is to process IDs and to ignore
     *   the external subset.
     * - XML_DETECT_IDS: Load external subset. This flag is
     *   misnamed. ID handling is only controlled by XML_SKIP_IDS.
     * - XML_COMPLETE_ATTRS: Load external subset and process
     *   default attributes.
     * - XML_SKIP_IDS: Ignore IDs.
     */
    int loadsubset XML_DEPRECATED_MEMBER;
    /* unused */
    int linenumbers XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtGetCatalogs() and xmlCtxtSetCatalogs()
     *
     * document's own catalog
     */
    void *catalogs XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlParserOption XML_PARSE_RECOVER
     * run in recovery mode
     */
    int recovery XML_DEPRECATED_MEMBER;
    /* unused */
    int progressive XML_DEPRECATED_MEMBER;
    /**
     * @deprecated Use xmlCtxtGetDict() and xmlCtxtSetDict()
     *
     * dictionary for the parser
     */
    xmlDict *dict;
    /* array for the attributes callbacks */
    const xmlChar **atts XML_DEPRECATED_MEMBER;
    /* the size of the array */
    int maxatts XML_DEPRECATED_MEMBER;
    /* unused */
    int docdict XML_DEPRECATED_MEMBER;

    /*
     * pre-interned strings
     */
    const xmlChar *str_xml XML_DEPRECATED_MEMBER;
    const xmlChar *str_xmlns XML_DEPRECATED_MEMBER;
    const xmlChar *str_xml_ns XML_DEPRECATED_MEMBER;

    /*
     * Everything below is used only by the new SAX mode
     */

    /* operating in the new SAX mode */
    int sax2 XML_DEPRECATED_MEMBER;
    /* the number of inherited namespaces */
    int nsNr XML_DEPRECATED_MEMBER;
    /* the size of the arrays */
    int nsMax XML_DEPRECATED_MEMBER;
    /* the array of prefix/namespace name */
    const xmlChar **nsTab XML_DEPRECATED_MEMBER;
    /* which attribute were allocated */
    unsigned *attallocs XML_DEPRECATED_MEMBER;
    /* array of data for push */
    xmlStartTag *pushTab XML_DEPRECATED_MEMBER;
    /* defaulted attributes if any */
    xmlHashTable *attsDefault XML_DEPRECATED_MEMBER;
    /* non-CDATA attributes if any */
    xmlHashTable *attsSpecial XML_DEPRECATED_MEMBER;

    /**
     * @deprecated Use xmlCtxtGetStatus()
     *
     * is the document XML Namespace okay
     */
    int nsWellFormed;
    /**
     * @deprecated Use xmlCtxtGetOptions()
     *
     * Extra options
     */
    int options;

    /**
     * @deprecated Use inverted xmlParserOption XML_PARSE_NODICT
     *
     * Use dictionary names for the tree
     */
    int dictNames XML_DEPRECATED_MEMBER;

    /*
     * Those fields are needed only for streaming parsing so far
     */

    /* number of freed element nodes */
    int freeElemsNr XML_DEPRECATED_MEMBER;
    /* List of freed element nodes */
    xmlNode *freeElems XML_DEPRECATED_MEMBER;
    /* number of freed attributes nodes */
    int freeAttrsNr XML_DEPRECATED_MEMBER;
    /* List of freed attributes nodes */
    xmlAttr *freeAttrs XML_DEPRECATED_MEMBER;

    /**
     * @deprecated Use xmlCtxtGetLastError()
     *
     * the complete error information for the last error.
     */
    xmlError lastError XML_DEPRECATED_MEMBER;
    /* the parser mode */
    xmlParserMode parseMode XML_DEPRECATED_MEMBER;
    /* unused */
    unsigned long nbentities XML_DEPRECATED_MEMBER;
    /* size of external entities */
    unsigned long sizeentities XML_DEPRECATED_MEMBER;

    /* for use by HTML non-recursive parser */
    /* Current NodeInfo */
    xmlParserNodeInfo *nodeInfo XML_DEPRECATED_MEMBER;
    /* Depth of the parsing stack */
    int nodeInfoNr XML_DEPRECATED_MEMBER;
    /* Max depth of the parsing stack */
    int nodeInfoMax XML_DEPRECATED_MEMBER;
    /* array of nodeInfos */
    xmlParserNodeInfo *nodeInfoTab XML_DEPRECATED_MEMBER;

    /* we need to label inputs */
    int input_id XML_DEPRECATED_MEMBER;
    /* volume of entity copy */
    unsigned long sizeentcopy XML_DEPRECATED_MEMBER;

    /* quote state for push parser */
    int endCheckState XML_DEPRECATED_MEMBER;
    /* number of errors */
    unsigned short nbErrors XML_DEPRECATED_MEMBER;
    /* number of warnings */
    unsigned short nbWarnings XML_DEPRECATED_MEMBER;
    /* maximum amplification factor */
    unsigned maxAmpl XML_DEPRECATED_MEMBER;

    /* namespace database */
    xmlParserNsData *nsdb XML_DEPRECATED_MEMBER;
    /* allocated size */
    unsigned attrHashMax XML_DEPRECATED_MEMBER;
    /* atttribute hash table */
    xmlAttrHashBucket *attrHash XML_DEPRECATED_MEMBER;

    xmlStructuredErrorFunc errorHandler XML_DEPRECATED_MEMBER;
    void *errorCtxt XML_DEPRECATED_MEMBER;

    xmlResourceLoader resourceLoader XML_DEPRECATED_MEMBER;
    void *resourceCtxt XML_DEPRECATED_MEMBER;

    xmlCharEncConvImpl convImpl XML_DEPRECATED_MEMBER;
    void *convCtxt XML_DEPRECATED_MEMBER;
};

/**
 * A SAX Locator.
 */
struct _xmlSAXLocator {
    const xmlChar *(*getPublicId)(void *ctx);
    const xmlChar *(*getSystemId)(void *ctx);
    int (*getLineNumber)(void *ctx);
    int (*getColumnNumber)(void *ctx);
};

/**
 * SAX callback to resolve external entities.
 *
 * This is only used to load DTDs. The preferred way to install
 * custom resolvers is #xmlCtxtSetResourceLoader.
 *
 * @param ctx  the user data (XML parser context)
 * @param publicId  The public identifier of the entity
 * @param systemId  The system identifier of the entity (URL)
 * @returns the xmlParserInput if inlined or NULL for DOM behaviour.
 */
typedef xmlParserInput *(*resolveEntitySAXFunc) (void *ctx,
				const xmlChar *publicId,
				const xmlChar *systemId);
/**
 * SAX callback for the internal subset.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the root element name
 * @param publicId  the public identifier
 * @param systemId  the system identifier (e.g. filename or URL)
 */
typedef void (*internalSubsetSAXFunc) (void *ctx,
				const xmlChar *name,
				const xmlChar *publicId,
				const xmlChar *systemId);
/**
 * SAX callback for the external subset.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the root element name
 * @param publicId  the public identifier
 * @param systemId  the system identifier (e.g. filename or URL)
 */
typedef void (*externalSubsetSAXFunc) (void *ctx,
				const xmlChar *name,
				const xmlChar *publicId,
				const xmlChar *systemId);
/**
 * SAX callback to look up a general entity by name.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 * @returns the xmlEntity if found.
 */
typedef xmlEntity *(*getEntitySAXFunc) (void *ctx,
				const xmlChar *name);
/**
 * SAX callback to look up a parameter entity by name.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 * @returns the xmlEntity if found.
 */
typedef xmlEntity *(*getParameterEntitySAXFunc) (void *ctx,
				const xmlChar *name);
/**
 * SAX callback for entity declarations.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the entity name
 * @param type  the entity type
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID of the entity
 * @param content  the entity value (without processing).
 */
typedef void (*entityDeclSAXFunc) (void *ctx,
				const xmlChar *name,
				int type,
				const xmlChar *publicId,
				const xmlChar *systemId,
				xmlChar *content);
/**
 * SAX callback for notation declarations.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The name of the notation
 * @param publicId  The public ID of the notation
 * @param systemId  The system ID of the notation
 */
typedef void (*notationDeclSAXFunc)(void *ctx,
				const xmlChar *name,
				const xmlChar *publicId,
				const xmlChar *systemId);
/**
 * SAX callback for attribute declarations.
 *
 * @param ctx  the user data (XML parser context)
 * @param elem  the name of the element
 * @param fullname  the attribute name
 * @param type  the attribute type
 * @param def  the type of default value
 * @param defaultValue  the attribute default value
 * @param tree  the tree of enumerated value set
 */
typedef void (*attributeDeclSAXFunc)(void *ctx,
				const xmlChar *elem,
				const xmlChar *fullname,
				int type,
				int def,
				const xmlChar *defaultValue,
				xmlEnumeration *tree);
/**
 * SAX callback for element declarations.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the element name
 * @param type  the element type
 * @param content  the element value tree
 */
typedef void (*elementDeclSAXFunc)(void *ctx,
				const xmlChar *name,
				int type,
				xmlElementContent *content);
/**
 * SAX callback for unparsed entity declarations.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The name of the entity
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID of the entity
 * @param notationName  the name of the notation
 */
typedef void (*unparsedEntityDeclSAXFunc)(void *ctx,
				const xmlChar *name,
				const xmlChar *publicId,
				const xmlChar *systemId,
				const xmlChar *notationName);
/**
 * This callback receives the "document locator" at startup,
 * which is always the global xmlDefaultSAXLocator.
 *
 * Everything is available on the context, so this is useless in
 * our case.
 *
 * @param ctx  the user data (XML parser context)
 * @param loc  A SAX Locator
 */
typedef void (*setDocumentLocatorSAXFunc) (void *ctx,
				xmlSAXLocator *loc);
/**
 * SAX callback for start of document.
 *
 * @param ctx  the user data (XML parser context)
 */
typedef void (*startDocumentSAXFunc) (void *ctx);
/**
 * SAX callback for end of document.
 *
 * @param ctx  the user data (XML parser context)
 */
typedef void (*endDocumentSAXFunc) (void *ctx);
/**
 * SAX callback for start tags.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The element name, including namespace prefix
 * @param atts  An array of name/value attributes pairs, NULL terminated
 */
typedef void (*startElementSAXFunc) (void *ctx,
				const xmlChar *name,
				const xmlChar **atts);
/**
 * SAX callback for end tags.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The element name
 */
typedef void (*endElementSAXFunc) (void *ctx,
				const xmlChar *name);
/**
 * Callback for attributes.
 *
 * @deprecated This typedef is unused.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The attribute name, including namespace prefix
 * @param value  The attribute value
 */
typedef void (*attributeSAXFunc) (void *ctx,
				const xmlChar *name,
				const xmlChar *value);
/**
 * SAX callback for entity references.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 */
typedef void (*referenceSAXFunc) (void *ctx,
				const xmlChar *name);
/**
 * SAX callback for character data.
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 */
typedef void (*charactersSAXFunc) (void *ctx,
				const xmlChar *ch,
				int len);
/**
 * SAX callback for "ignorable" whitespace.
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 */
typedef void (*ignorableWhitespaceSAXFunc) (void *ctx,
				const xmlChar *ch,
				int len);
/**
 * SAX callback for processing instructions.
 *
 * @param ctx  the user data (XML parser context)
 * @param target  the target name
 * @param data  the PI data's
 */
typedef void (*processingInstructionSAXFunc) (void *ctx,
				const xmlChar *target,
				const xmlChar *data);
/**
 * SAX callback for comments.
 *
 * @param ctx  the user data (XML parser context)
 * @param value  the comment content
 */
typedef void (*commentSAXFunc) (void *ctx,
				const xmlChar *value);
/**
 * SAX callback for CDATA sections.
 *
 * @param ctx  the user data (XML parser context)
 * @param value  The pcdata content
 * @param len  the block length
 */
typedef void (*cdataBlockSAXFunc) (
	                        void *ctx,
				const xmlChar *value,
				int len);
/**
 * SAX callback for warning messages.
 *
 * @param ctx  an XML parser context
 * @param msg  the message to display/transmit
 * @param ...  extra parameters for the message display
 */
typedef void (*warningSAXFunc) (void *ctx,
				const char *msg, ...) LIBXML_ATTR_FORMAT(2,3);
/**
 * SAX callback for error messages.
 *
 * @param ctx  an XML parser context
 * @param msg  the message to display/transmit
 * @param ...  extra parameters for the message display
 */
typedef void (*errorSAXFunc) (void *ctx,
				const char *msg, ...) LIBXML_ATTR_FORMAT(2,3);
/**
 * SAX callback for fatal error messages.
 *
 * @param ctx  an XML parser context
 * @param msg  the message to display/transmit
 * @param ...  extra parameters for the message display
 */
typedef void (*fatalErrorSAXFunc) (void *ctx,
				const char *msg, ...) LIBXML_ATTR_FORMAT(2,3);
/**
 * SAX callback to get standalone status.
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
typedef int (*isStandaloneSAXFunc) (void *ctx);
/**
 * SAX callback to get internal subset status.
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
typedef int (*hasInternalSubsetSAXFunc) (void *ctx);

/**
 * SAX callback to get external subset status.
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
typedef int (*hasExternalSubsetSAXFunc) (void *ctx);

/************************************************************************
 *									*
 *			The SAX version 2 API extensions		*
 *									*
 ************************************************************************/
/**
 * Special constant required for SAX2 handlers.
 */
#define XML_SAX2_MAGIC 0xDEEDBEAF

/**
 * SAX2 callback for start tags.
 *
 * It provides the namespace information for the element, as well as
 * the new namespace declarations on the element.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 * @param nb_namespaces  number of namespace definitions on that node
 * @param namespaces  pointer to the array of prefix/URI pairs namespace definitions
 * @param nb_attributes  the number of attributes on that node
 * @param nb_defaulted  the number of defaulted attributes. The defaulted
 *                  ones are at the end of the array
 * @param attributes  pointer to the array of (localname/prefix/URI/value/end)
 *               attribute values.
 */

typedef void (*startElementNsSAX2Func) (void *ctx,
					const xmlChar *localname,
					const xmlChar *prefix,
					const xmlChar *URI,
					int nb_namespaces,
					const xmlChar **namespaces,
					int nb_attributes,
					int nb_defaulted,
					const xmlChar **attributes);

/**
 * SAX2 callback for end tags.
 *
 * It provides the namespace information for the element.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 */

typedef void (*endElementNsSAX2Func)   (void *ctx,
					const xmlChar *localname,
					const xmlChar *prefix,
					const xmlChar *URI);

/**
 * Callbacks for SAX parser
 *
 * For DTD-related handlers, it's recommended to either use the
 * original libxml2 handler or set them to NULL if DTDs can be
 * ignored.
 */
struct _xmlSAXHandler {
    /**
     * Called after the start of the document type declaration
     * was parsed.
     *
     * Should typically not be modified.
     */
    internalSubsetSAXFunc internalSubset;
    /**
     * Standalone status. Not invoked by the parser. Not supposed
     * to be changed by applications.
     */
    isStandaloneSAXFunc isStandalone;
    /**
     * Internal subset availability. Not invoked by the parser.
     * Not supposed to be changed by applications.
     */
    hasInternalSubsetSAXFunc hasInternalSubset;
    /**
     * External subset availability. Not invoked by the parser.
     * Not supposed to be changed by applications.
     */
    hasExternalSubsetSAXFunc hasExternalSubset;
    /**
     * Only called when loading external DTDs. Not called to load
     * external entities.
     *
     * Should typically not be modified.
     */
    resolveEntitySAXFunc resolveEntity;
    /**
     * Called when looking up general entities.
     *
     * Should typically not be modified.
     */
    getEntitySAXFunc getEntity;
    /**
     * Called after an entity declaration was parsed.
     *
     * Should typically not be modified.
     */
    entityDeclSAXFunc entityDecl;
    /**
     * Called after a notation declaration was parsed.
     *
     * Should typically not be modified.
     */
    notationDeclSAXFunc notationDecl;
    /**
     * Called after an attribute declaration was parsed.
     *
     * Should typically not be modified.
     */
    attributeDeclSAXFunc attributeDecl;
    /**
     * Called after an element declaration was parsed.
     *
     * Should typically not be modified.
     */
    elementDeclSAXFunc elementDecl;
    /**
     * Called after an unparsed entity declaration was parsed.
     *
     * Should typically not be modified.
     */
    unparsedEntityDeclSAXFunc unparsedEntityDecl;
    /**
     * This callback receives the "document locator" at startup,
     * which is always the global xmlDefaultSAXLocator.
     *
     * Everything is available on the context, so this is useless in
     * our case.
     */
    setDocumentLocatorSAXFunc setDocumentLocator;
    /**
     * Called after the XML declaration was parsed.
     *
     * Use xmlCtxtGetVersion(), xmlCtxtGetDeclaredEncoding() and
     * xmlCtxtGetStandalone() to get data from the XML declaration.
     */
    startDocumentSAXFunc startDocument;
    /**
     * Called at the end of the document.
     */
    endDocumentSAXFunc endDocument;
    /**
     * Legacy start tag handler
     *
     * `startElement` and `endElement` are only used by the legacy SAX1
     * interface and should not be used in new software. If you really
     * have to enable SAX1, the preferred way is set the `initialized`
     * member to 1 instead of XML_SAX2_MAGIC.
     *
     * For backward compatibility, it's also possible to set the
     * `startElementNs` and `endElementNs` handlers to NULL.
     *
     * You can also set the XML_PARSE_SAX1 parser option, but versions
     * older than 2.12.0 will probably crash if this option is provided
     * together with custom SAX callbacks.
     */
    startElementSAXFunc startElement;
    /**
     * See _xmlSAXHandler.startElement
     */
    endElementSAXFunc endElement;
    /**
     * Called after an entity reference was parsed.
     */
    referenceSAXFunc reference;
    /**
     * Called after a character data was parsed.
     */
    charactersSAXFunc characters;
    /**
     * Called after "ignorable" whitespace was parsed.
     *
     * `ignorableWhitespace` should always be set to the same value
     * as `characters`. Otherwise, the parser will try to detect
     * whitespace which is unreliable.
     */
    ignorableWhitespaceSAXFunc ignorableWhitespace;
    /**
     * Called after a processing instruction was parsed.
     */
    processingInstructionSAXFunc processingInstruction;
    /**
     * Called after a comment was parsed.
     */
    commentSAXFunc comment;
    /**
     * Callback for warning messages.
     */
    warningSAXFunc warning;
    /**
     * Callback for error messages.
     */
    errorSAXFunc error;
    /**
     * Unused, all errors go to `error`.
     */
    fatalErrorSAXFunc fatalError;
    /**
     * Called when looking up parameter entities.
     *
     * Should typically not be modified.
     */
    getParameterEntitySAXFunc getParameterEntity;
    /**
     * Called after a CDATA section was parsed.
     */
    cdataBlockSAXFunc cdataBlock;
    /**
     * Called to parse the external subset.
     *
     * Should typically not be modified.
     */
    externalSubsetSAXFunc externalSubset;
    /**
     * Legacy magic value
     *
     * `initialized` should always be set to XML_SAX2_MAGIC to
     * enable the modern SAX2 interface.
     */
    unsigned int initialized;
    /**
     * Application data
     */
    void *_private;
    /**
     * Called after a start tag was parsed.
     */
    startElementNsSAX2Func startElementNs;
    /**
     * Called after an end tag was parsed.
     */
    endElementNsSAX2Func endElementNs;
    /**
     * Structured error handler.
     *
     * Takes precedence over `error` or `warning`, but modern code
     * should use xmlCtxtSetErrorHandler().
     */
    xmlStructuredErrorFunc serror;
};

/**
 * SAX handler, version 1.
 *
 * @deprecated Use version 2 handlers.
 */
typedef struct _xmlSAXHandlerV1 xmlSAXHandlerV1;
typedef xmlSAXHandlerV1 *xmlSAXHandlerV1Ptr;
/**
 * SAX handler, version 1.
 *
 * @deprecated Use version 2 handlers.
 */
struct _xmlSAXHandlerV1 {
    internalSubsetSAXFunc internalSubset;
    isStandaloneSAXFunc isStandalone;
    hasInternalSubsetSAXFunc hasInternalSubset;
    hasExternalSubsetSAXFunc hasExternalSubset;
    resolveEntitySAXFunc resolveEntity;
    getEntitySAXFunc getEntity;
    entityDeclSAXFunc entityDecl;
    notationDeclSAXFunc notationDecl;
    attributeDeclSAXFunc attributeDecl;
    elementDeclSAXFunc elementDecl;
    unparsedEntityDeclSAXFunc unparsedEntityDecl;
    setDocumentLocatorSAXFunc setDocumentLocator;
    startDocumentSAXFunc startDocument;
    endDocumentSAXFunc endDocument;
    startElementSAXFunc startElement;
    endElementSAXFunc endElement;
    referenceSAXFunc reference;
    charactersSAXFunc characters;
    ignorableWhitespaceSAXFunc ignorableWhitespace;
    processingInstructionSAXFunc processingInstruction;
    commentSAXFunc comment;
    warningSAXFunc warning;
    errorSAXFunc error;
    fatalErrorSAXFunc fatalError; /* unused error() get all the errors */
    getParameterEntitySAXFunc getParameterEntity;
    cdataBlockSAXFunc cdataBlock;
    externalSubsetSAXFunc externalSubset;
    unsigned int initialized;
};


/**
 * Callback for external entity loader.
 *
 * The URL is not resolved using XML catalogs before being passed
 * to the callback.
 *
 * @param URL  The URL or system ID of the resource requested
 * @param publicId  The public ID of the resource requested (optional)
 * @param context  the XML parser context
 * @returns the entity input parser or NULL on error.
 */
typedef xmlParserInput *(*xmlExternalEntityLoader) (const char *URL,
					 const char *publicId,
					 xmlParserCtxt *context);

/*
 * Variables
 */

XMLPUBVAR const char *const xmlParserVersion;

/** @cond ignore */

XML_DEPRECATED
XMLPUBVAR const xmlSAXLocator xmlDefaultSAXLocator;
#ifdef LIBXML_SAX1_ENABLED
/**
 * @deprecated Use #xmlSAXVersion or #xmlSAX2InitDefaultSAXHandler
 */
XML_DEPRECATED
XMLPUBVAR const xmlSAXHandlerV1 xmlDefaultSAXHandler;
#endif

XML_DEPRECATED
XMLPUBFUN int *__xmlDoValidityCheckingDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlGetWarningsDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlKeepBlanksDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlLineNumbersDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlLoadExtDtdDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlPedanticParserDefaultValue(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlSubstituteEntitiesDefaultValue(void);

#ifdef LIBXML_OUTPUT_ENABLED
XML_DEPRECATED
XMLPUBFUN int *__xmlIndentTreeOutput(void);
XML_DEPRECATED
XMLPUBFUN const char **__xmlTreeIndentString(void);
XML_DEPRECATED
XMLPUBFUN int *__xmlSaveNoEmptyTags(void);
#endif

/** @endcond */

#ifndef XML_GLOBALS_NO_REDEFINITION
  /**
   * Thread-local setting to enable validation. Defaults to 0.
   *
   * @deprecated Use the parser option XML_PARSE_DTDVALID.
   */
  #define xmlDoValidityCheckingDefaultValue \
    (*__xmlDoValidityCheckingDefaultValue())
  /**
   * Thread-local setting to disable warnings. Defaults to 1.
   *
   * @deprecated Use the parser option XML_PARSE_NOWARNING.
   */
  #define xmlGetWarningsDefaultValue \
    (*__xmlGetWarningsDefaultValue())
  /**
   * Thread-local setting to ignore some whitespace. Defaults
   * to 1.
   *
   * @deprecated Use the parser option XML_PARSE_NOBLANKS.
   */
  #define xmlKeepBlanksDefaultValue (*__xmlKeepBlanksDefaultValue())
  /**
   * Thread-local setting to store line numbers. Defaults
   * to 0, but is always enabled after setting parser options.
   *
   * @deprecated Shouldn't be needed when using parser options.
   */
  #define xmlLineNumbersDefaultValue \
    (*__xmlLineNumbersDefaultValue())
  /**
   * Thread-local setting to enable loading of external DTDs.
   * Defaults to 0.
   *
   * @deprecated Use the parser option XML_PARSE_DTDLOAD.
   */
  #define xmlLoadExtDtdDefaultValue (*__xmlLoadExtDtdDefaultValue())
  /**
   * Thread-local setting to enable pedantic warnings.
   * Defaults to 0.
   *
   * @deprecated Use the parser option XML_PARSE_PEDANTIC.
   */
  #define xmlPedanticParserDefaultValue \
    (*__xmlPedanticParserDefaultValue())
  /**
   * Thread-local setting to enable entity substitution.
   * Defaults to 0.
   *
   * @deprecated Use the parser option XML_PARSE_NOENT.
   */
  #define xmlSubstituteEntitiesDefaultValue \
    (*__xmlSubstituteEntitiesDefaultValue())
  #ifdef LIBXML_OUTPUT_ENABLED
    /**
     * Thread-local setting to disable indenting when
     * formatting output. Defaults to 1.
     *
     * @deprecated Use the xmlsave.h API with option
     * XML_SAVE_NO_INDENT.
     */
    #define xmlIndentTreeOutput (*__xmlIndentTreeOutput())
    /**
     * Thread-local setting to change the indent string.
     * Defaults to two spaces.
     *
     * @deprecated Use the xmlsave.h API and
     * xmlSaveSetIndentString().
     */
    #define xmlTreeIndentString (*__xmlTreeIndentString())
    /**
     * Thread-local setting to disable empty tags when
     * serializing. Defaults to 0.
     *
     * @deprecated Use the xmlsave.h API with option
     * XML_SAVE_NO_EMPTY.
     */
    #define xmlSaveNoEmptyTags (*__xmlSaveNoEmptyTags())
  #endif
#endif

/*
 * Init/Cleanup
 */
XMLPUBFUN void
		xmlInitParser		(void);
XMLPUBFUN void
		xmlCleanupParser	(void);
XML_DEPRECATED
XMLPUBFUN void
		xmlInitGlobals		(void);
XML_DEPRECATED
XMLPUBFUN void
		xmlCleanupGlobals	(void);

/*
 * Input functions
 */
XML_DEPRECATED
XMLPUBFUN int
		xmlParserInputRead	(xmlParserInput *in,
					 int len);
XMLPUBFUN int
		xmlParserInputGrow	(xmlParserInput *in,
					 int len);

/*
 * Basic parsing Interfaces
 */
#ifdef LIBXML_SAX1_ENABLED
XMLPUBFUN xmlDoc *
		xmlParseDoc		(const xmlChar *cur);
XMLPUBFUN xmlDoc *
		xmlParseFile		(const char *filename);
XMLPUBFUN xmlDoc *
		xmlParseMemory		(const char *buffer,
					 int size);
#endif /* LIBXML_SAX1_ENABLED */
XML_DEPRECATED
XMLPUBFUN int
		xmlSubstituteEntitiesDefault(int val);
XMLPUBFUN int
		xmlKeepBlanksDefault	(int val);
XMLPUBFUN void
		xmlStopParser		(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
		xmlPedanticParserDefault(int val);
XML_DEPRECATED
XMLPUBFUN int
		xmlLineNumbersDefault	(int val);

XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefSubstituteEntitiesDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
		xmlThrDefKeepBlanksDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefPedanticParserDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefLineNumbersDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefDoValidityCheckingDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefGetWarningsDefaultValue(int v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefLoadExtDtdDefaultValue(int v);

#ifdef LIBXML_SAX1_ENABLED
/*
 * Recovery mode
 */
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlRecoverDoc		(const xmlChar *cur);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlRecoverMemory	(const char *buffer,
					 int size);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlRecoverFile		(const char *filename);
#endif /* LIBXML_SAX1_ENABLED */

/*
 * Less common routines and SAX interfaces
 */
XMLPUBFUN int
		xmlParseDocument	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN int
		xmlParseExtParsedEnt	(xmlParserCtxt *ctxt);
#ifdef LIBXML_SAX1_ENABLED
XML_DEPRECATED
XMLPUBFUN int
		xmlSAXUserParseFile	(xmlSAXHandler *sax,
					 void *user_data,
					 const char *filename);
XML_DEPRECATED
XMLPUBFUN int
		xmlSAXUserParseMemory	(xmlSAXHandler *sax,
					 void *user_data,
					 const char *buffer,
					 int size);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseDoc		(xmlSAXHandler *sax,
					 const xmlChar *cur,
					 int recovery);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseMemory	(xmlSAXHandler *sax,
					 const char *buffer,
					 int size,
					 int recovery);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseMemoryWithData (xmlSAXHandler *sax,
					 const char *buffer,
					 int size,
					 int recovery,
					 void *data);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseFile		(xmlSAXHandler *sax,
					 const char *filename,
					 int recovery);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseFileWithData	(xmlSAXHandler *sax,
					 const char *filename,
					 int recovery,
					 void *data);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlSAXParseEntity	(xmlSAXHandler *sax,
					 const char *filename);
XML_DEPRECATED
XMLPUBFUN xmlDoc *
		xmlParseEntity		(const char *filename);
#endif /* LIBXML_SAX1_ENABLED */

#ifdef LIBXML_VALID_ENABLED
XMLPUBFUN xmlDtd *
		xmlCtxtParseDtd		(xmlParserCtxt *ctxt,
					 xmlParserInput *input,
					 const xmlChar *publicId,
					 const xmlChar *systemId);
XMLPUBFUN int
		xmlCtxtValidateDocument	(xmlParserCtxt *ctxt,
					 xmlDoc *doc);
XMLPUBFUN int
		xmlCtxtValidateDtd	(xmlParserCtxt *ctxt,
					 xmlDoc *doc,
					 xmlDtd *dtd);
XML_DEPRECATED
XMLPUBFUN xmlDtd *
		xmlSAXParseDTD		(xmlSAXHandler *sax,
					 const xmlChar *publicId,
					 const xmlChar *systemId);
XMLPUBFUN xmlDtd *
		xmlParseDTD		(const xmlChar *publicId,
					 const xmlChar *systemId);
XMLPUBFUN xmlDtd *
		xmlIOParseDTD		(xmlSAXHandler *sax,
					 xmlParserInputBuffer *input,
					 xmlCharEncoding enc);
#endif /* LIBXML_VALID_ENABLE */
#ifdef LIBXML_SAX1_ENABLED
XMLPUBFUN int
		xmlParseBalancedChunkMemory(xmlDoc *doc,
					 xmlSAXHandler *sax,
					 void *user_data,
					 int depth,
					 const xmlChar *string,
					 xmlNode **lst);
#endif /* LIBXML_SAX1_ENABLED */
XMLPUBFUN xmlParserErrors
		xmlParseInNodeContext	(xmlNode *node,
					 const char *data,
					 int datalen,
					 int options,
					 xmlNode **lst);
#ifdef LIBXML_SAX1_ENABLED
XMLPUBFUN int
		xmlParseBalancedChunkMemoryRecover(xmlDoc *doc,
                     xmlSAXHandler *sax,
                     void *user_data,
                     int depth,
                     const xmlChar *string,
                     xmlNode **lst,
                     int recover);
XML_DEPRECATED
XMLPUBFUN int
		xmlParseExternalEntity	(xmlDoc *doc,
					 xmlSAXHandler *sax,
					 void *user_data,
					 int depth,
					 const xmlChar *URL,
					 const xmlChar *ID,
					 xmlNode **lst);
#endif /* LIBXML_SAX1_ENABLED */
XMLPUBFUN int
		xmlParseCtxtExternalEntity(xmlParserCtxt *ctx,
					 const xmlChar *URL,
					 const xmlChar *ID,
					 xmlNode **lst);

/*
 * Parser contexts handling.
 */
XMLPUBFUN xmlParserCtxt *
		xmlNewParserCtxt	(void);
XMLPUBFUN xmlParserCtxt *
		xmlNewSAXParserCtxt	(const xmlSAXHandler *sax,
					 void *userData);
XMLPUBFUN int
		xmlInitParserCtxt	(xmlParserCtxt *ctxt);
XML_DEPRECATED
XMLPUBFUN void
		xmlClearParserCtxt	(xmlParserCtxt *ctxt);
XMLPUBFUN void
		xmlFreeParserCtxt	(xmlParserCtxt *ctxt);
#ifdef LIBXML_SAX1_ENABLED
XML_DEPRECATED
XMLPUBFUN void
		xmlSetupParserForBuffer	(xmlParserCtxt *ctxt,
					 const xmlChar* buffer,
					 const char *filename);
#endif /* LIBXML_SAX1_ENABLED */
XMLPUBFUN xmlParserCtxt *
		xmlCreateDocParserCtxt	(const xmlChar *cur);

#ifdef LIBXML_PUSH_ENABLED
/*
 * Interfaces for the Push mode.
 */
XMLPUBFUN xmlParserCtxt *
		xmlCreatePushParserCtxt(xmlSAXHandler *sax,
					 void *user_data,
					 const char *chunk,
					 int size,
					 const char *filename);
XMLPUBFUN int
		xmlParseChunk		(xmlParserCtxt *ctxt,
					 const char *chunk,
					 int size,
					 int terminate);
#endif /* LIBXML_PUSH_ENABLED */

/*
 * Special I/O mode.
 */

XMLPUBFUN xmlParserCtxt *
		xmlCreateIOParserCtxt	(xmlSAXHandler *sax,
					 void *user_data,
					 xmlInputReadCallback   ioread,
					 xmlInputCloseCallback  ioclose,
					 void *ioctx,
					 xmlCharEncoding enc);

XMLPUBFUN xmlParserInput *
		xmlNewIOInputStream	(xmlParserCtxt *ctxt,
					 xmlParserInputBuffer *input,
					 xmlCharEncoding enc);

/*
 * Node infos.
 */
XML_DEPRECATED
XMLPUBFUN const xmlParserNodeInfo*
		xmlParserFindNodeInfo	(xmlParserCtxt *ctxt,
				         xmlNode *node);
XML_DEPRECATED
XMLPUBFUN void
		xmlInitNodeInfoSeq	(xmlParserNodeInfoSeq *seq);
XML_DEPRECATED
XMLPUBFUN void
		xmlClearNodeInfoSeq	(xmlParserNodeInfoSeq *seq);
XML_DEPRECATED
XMLPUBFUN unsigned long
		xmlParserFindNodeInfoIndex(xmlParserNodeInfoSeq *seq,
                                         xmlNode *node);
XML_DEPRECATED
XMLPUBFUN void
		xmlParserAddNodeInfo	(xmlParserCtxt *ctxt,
					 xmlParserNodeInfo *info);

/*
 * External entities handling actually implemented in xmlIO.
 */

XMLPUBFUN void
		xmlSetExternalEntityLoader(xmlExternalEntityLoader f);
XMLPUBFUN xmlExternalEntityLoader
		xmlGetExternalEntityLoader(void);
XMLPUBFUN xmlParserInput *
		xmlLoadExternalEntity	(const char *URL,
					 const char *ID,
					 xmlParserCtxt *ctxt);

XML_DEPRECATED
XMLPUBFUN long
		xmlByteConsumed		(xmlParserCtxt *ctxt);

/*
 * New set of simpler/more flexible APIs
 */

/**
 * This is the set of XML parser options that can be passed to
 * #xmlReadDoc, #xmlCtxtSetOptions and other functions.
 */
typedef enum {
    /**
     * Enable "recovery" mode which allows non-wellformed documents.
     * How this mode behaves exactly is unspecified and may change
     * without further notice. Use of this feature is DISCOURAGED.
     *
     * Not supported by the push parser.
     */
    XML_PARSE_RECOVER = 1<<0,
    /**
     * Despite the confusing name, this option enables substitution
     * of entities. The resulting tree won't contain any entity
     * reference nodes.
     *
     * This option also enables loading of external entities (both
     * general and parameter entities) which is dangerous. If you
     * process untrusted data, it's recommended to set the
     * XML_PARSE_NO_XXE option to disable loading of external
     * entities.
     */
    XML_PARSE_NOENT = 1<<1,
    /**
     * Enables loading of an external DTD and the loading and
     * substitution of external parameter entities. Has no effect
     * if XML_PARSE_NO_XXE is set.
     */
    XML_PARSE_DTDLOAD = 1<<2,
    /**
     * Adds default attributes from the DTD to the result document.
     *
     * Implies XML_PARSE_DTDLOAD, but loading of external content
     * can be disabled with XML_PARSE_NO_XXE.
     */
    XML_PARSE_DTDATTR = 1<<3,
    /**
     * This option enables DTD validation which requires to load
     * external DTDs and external entities (both general and
     * parameter entities) unless XML_PARSE_NO_XXE was set.
     *
     * DTD validation is vulnerable to algorithmic complexity
     * attacks and should never be enabled with untrusted input.
     */
    XML_PARSE_DTDVALID = 1<<4,
    /**
     * Disable error and warning reports to the error handlers.
     * Errors are still accessible with xmlCtxtGetLastError().
     */
    XML_PARSE_NOERROR = 1<<5,
    /**
     * Disable warning reports.
     */
    XML_PARSE_NOWARNING = 1<<6,
    /**
     * Enable some pedantic warnings.
     */
    XML_PARSE_PEDANTIC = 1<<7,
    /**
     * Remove some whitespace from the result document. Where to
     * remove whitespace depends on DTD element declarations or a
     * broken heuristic with unfixable bugs. Use of this option is
     * DISCOURAGED.
     *
     * Not supported by the push parser.
     */
    XML_PARSE_NOBLANKS = 1<<8,
    /**
     * Always invoke the deprecated SAX1 startElement and endElement
     * handlers.
     *
     * @deprecated This option will be removed in a future version.
     */
    XML_PARSE_SAX1 = 1<<9,
    /**
     * Enable XInclude processing. This option only affects the
     * xmlTextReader and XInclude interfaces.
     */
    XML_PARSE_XINCLUDE = 1<<10,
    /**
     * Disable network access with the built-in HTTP or FTP clients.
     *
     * After the last built-in network client was removed in 2.15,
     * this option has no effect expect for being passed on to custom
     * resource loaders.
     */
    XML_PARSE_NONET = 1<<11,
    /**
     * Create a document without interned strings, making all
     * strings separate memory allocations.
     */
    XML_PARSE_NODICT = 1<<12,
    /**
     * Remove redundant namespace declarations from the result
     * document.
     */
    XML_PARSE_NSCLEAN = 1<<13,
    /**
     * Output normal text nodes instead of CDATA nodes.
     */
    XML_PARSE_NOCDATA = 1<<14,
    /**
     * Don't generate XInclude start/end nodes when expanding
     * inclusions. This option only affects the xmlTextReader
     * and XInclude interfaces.
     */
    XML_PARSE_NOXINCNODE = 1<<15,
    /**
     * Store small strings directly in the node struct to save
     * memory.
     */
    XML_PARSE_COMPACT = 1<<16,
    /**
     * Use old Name productions from before XML 1.0 Fifth Edition.
     *
     * @deprecated This option will be removed in a future version.
     */
    XML_PARSE_OLD10 = 1<<17,
    /**
     * Don't fix up XInclude xml:base URIs. This option only affects
     * the xmlTextReader and XInclude interfaces.
     */
    XML_PARSE_NOBASEFIX = 1<<18,
    /**
     * Relax some internal limits.
     *
     * Maximum size of text nodes, tags, comments, processing instructions,
     * CDATA sections, entity values
     *
     * normal: 10M
     * huge:    1B
     *
     * Maximum size of names, system literals, pubid literals
     *
     * normal: 50K
     * huge:   10M
     *
     * Maximum nesting depth of elements
     *
     * normal:  256
     * huge:   2048
     *
     * Maximum nesting depth of entities
     *
     * normal: 20
     * huge:   40
     */
    XML_PARSE_HUGE = 1<<19,
    /**
     * Enable an unspecified legacy mode for SAX parsers.
     *
     * @deprecated This option will be removed in a future version.
     */
    XML_PARSE_OLDSAX = 1<<20,
    /**
     * Ignore the encoding in the XML declaration. This option is
     * mostly unneeded these days. The only effect is to enforce
     * UTF-8 decoding of ASCII-like data.
     */
    XML_PARSE_IGNORE_ENC = 1<<21,
    /**
     * Enable reporting of line numbers larger than 65535.
     */
    XML_PARSE_BIG_LINES = 1<<22,
    /**
     * Disables loading of external DTDs or entities.
     *
     * @since 2.13.0
     */
    XML_PARSE_NO_XXE = 1<<23,
    /**
     * Enable input decompression. Setting this option is discouraged
     * to avoid zip bombs.
     *
     * @since 2.14.0
     */
    XML_PARSE_UNZIP = 1<<24,
    /**
     * Disables the global system XML catalog.
     *
     * @since 2.14.0
     */
    XML_PARSE_NO_SYS_CATALOG = 1<<25,
    /**
     * Enable XML catalog processing instructions.
     *
     * @since 2.14.0
     */
    XML_PARSE_CATALOG_PI = 1<<26,
    /**
     * Force the parser to ignore IDs.
     *
     * @since 2.15.0
     */
    XML_PARSE_SKIP_IDS = 1<<27
} xmlParserOption;

XMLPUBFUN void
		xmlCtxtReset		(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtResetPush	(xmlParserCtxt *ctxt,
					 const char *chunk,
					 int size,
					 const char *filename,
					 const char *encoding);
XMLPUBFUN int
		xmlCtxtGetOptions	(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtSetOptions	(xmlParserCtxt *ctxt,
					 int options);
XMLPUBFUN int
		xmlCtxtUseOptions	(xmlParserCtxt *ctxt,
					 int options);
XMLPUBFUN void *
		xmlCtxtGetPrivate	(xmlParserCtxt *ctxt);
XMLPUBFUN void
		xmlCtxtSetPrivate	(xmlParserCtxt *ctxt,
					 void *priv);
XMLPUBFUN void *
		xmlCtxtGetCatalogs	(xmlParserCtxt *ctxt);
XMLPUBFUN void
		xmlCtxtSetCatalogs	(xmlParserCtxt *ctxt,
					 void *catalogs);
XMLPUBFUN xmlDict *
		xmlCtxtGetDict		(xmlParserCtxt *ctxt);
XMLPUBFUN void
		xmlCtxtSetDict		(xmlParserCtxt *ctxt,
					 xmlDict *);
XMLPUBFUN xmlSAXHandler *
		xmlCtxtGetSaxHandler	(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtSetSaxHandler	(xmlParserCtxt *ctxt,
					 const xmlSAXHandler *sax);
XMLPUBFUN xmlDoc *
		xmlCtxtGetDocument	(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtIsHtml		(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtIsStopped	(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtIsInSubset	(xmlParserCtxt *ctxt);
#ifdef LIBXML_VALID_ENABLED
XMLPUBFUN xmlValidCtxt *
		xmlCtxtGetValidCtxt	(xmlParserCtxt *ctxt);
#endif
XMLPUBFUN const xmlChar *
		xmlCtxtGetVersion	(xmlParserCtxt *ctxt);
XMLPUBFUN const xmlChar *
		xmlCtxtGetDeclaredEncoding(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtGetStandalone	(xmlParserCtxt *ctxt);
XMLPUBFUN xmlParserStatus
		xmlCtxtGetStatus	(xmlParserCtxt *ctxt);
XMLPUBFUN void *
		xmlCtxtGetUserData	(xmlParserCtxt *ctxt);
XMLPUBFUN xmlNode *
		xmlCtxtGetNode		(xmlParserCtxt *ctxt);
XMLPUBFUN int
		xmlCtxtGetDocTypeDecl	(xmlParserCtxt *ctxt,
					 const xmlChar **name,
					 const xmlChar **systemId,
					 const xmlChar **publicId);
XMLPUBFUN int
		xmlCtxtGetInputPosition	(xmlParserCtxt *ctxt,
					 int inputIndex,
					 const char **filname,
					 int *line,
					 int *col,
					 unsigned long *bytePos);
XMLPUBFUN int
		xmlCtxtGetInputWindow	(xmlParserCtxt *ctxt,
					 int inputIndex,
					 const xmlChar **startOut,
					 int *sizeInOut,
					 int *offsetOut);
XMLPUBFUN void
		xmlCtxtSetErrorHandler	(xmlParserCtxt *ctxt,
					 xmlStructuredErrorFunc handler,
					 void *data);
XMLPUBFUN void
		xmlCtxtSetResourceLoader(xmlParserCtxt *ctxt,
					 xmlResourceLoader loader,
					 void *vctxt);
XMLPUBFUN void
		xmlCtxtSetCharEncConvImpl(xmlParserCtxt *ctxt,
					 xmlCharEncConvImpl impl,
					 void *vctxt);
XMLPUBFUN void
		xmlCtxtSetMaxAmplification(xmlParserCtxt *ctxt,
					 unsigned maxAmpl);
XMLPUBFUN xmlDoc *
		xmlReadDoc		(const xmlChar *cur,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlReadFile		(const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlReadMemory		(const char *buffer,
					 int size,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlReadFd		(int fd,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlReadIO		(xmlInputReadCallback ioread,
					 xmlInputCloseCallback ioclose,
					 void *ioctx,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlCtxtParseDocument	(xmlParserCtxt *ctxt,
					 xmlParserInput *input);
XMLPUBFUN xmlNode *
		xmlCtxtParseContent	(xmlParserCtxt *ctxt,
					 xmlParserInput *input,
					 xmlNode *node,
					 int hasTextDecl);
XMLPUBFUN xmlDoc *
		xmlCtxtReadDoc		(xmlParserCtxt *ctxt,
					 const xmlChar *cur,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlCtxtReadFile		(xmlParserCtxt *ctxt,
					 const char *filename,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlCtxtReadMemory		(xmlParserCtxt *ctxt,
					 const char *buffer,
					 int size,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlCtxtReadFd		(xmlParserCtxt *ctxt,
					 int fd,
					 const char *URL,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlDoc *
		xmlCtxtReadIO		(xmlParserCtxt *ctxt,
					 xmlInputReadCallback ioread,
					 xmlInputCloseCallback ioclose,
					 void *ioctx,
					 const char *URL,
					 const char *encoding,
					 int options);

/*
 * New input API
 */

XMLPUBFUN xmlParserErrors
xmlNewInputFromUrl(const char *url, xmlParserInputFlags flags,
                   xmlParserInput **out);
XMLPUBFUN xmlParserInput *
xmlNewInputFromMemory(const char *url, const void *mem, size_t size,
                      xmlParserInputFlags flags);
XMLPUBFUN xmlParserInput *
xmlNewInputFromString(const char *url, const char *str,
                      xmlParserInputFlags flags);
XMLPUBFUN xmlParserInput *
xmlNewInputFromFd(const char *url, int fd, xmlParserInputFlags flags);
XMLPUBFUN xmlParserInput *
xmlNewInputFromIO(const char *url, xmlInputReadCallback ioRead,
                  xmlInputCloseCallback ioClose, void *ioCtxt,
                  xmlParserInputFlags flags);
XMLPUBFUN xmlParserErrors
xmlInputSetEncodingHandler(xmlParserInput *input,
                           xmlCharEncodingHandler *handler);

/*
 * Library wide options
 */

/**
 * Used to examine the existence of features that can be enabled
 * or disabled at compile-time.
 * They used to be called XML_FEATURE_xxx but this clashed with Expat
 */
typedef enum {
    /** Multithreading support */
    XML_WITH_THREAD = 1,
    /** @deprecated Always available */
    XML_WITH_TREE = 2,
    /** Serialization support */
    XML_WITH_OUTPUT = 3,
    /** Push parser */
    XML_WITH_PUSH = 4,
    /** XML Reader */
    XML_WITH_READER = 5,
    /** Streaming patterns */
    XML_WITH_PATTERN = 6,
    /** XML Writer */
    XML_WITH_WRITER = 7,
    /** Legacy SAX1 API */
    XML_WITH_SAX1 = 8,
    /** @deprecated FTP support was removed */
    XML_WITH_FTP = 9,
    /** @deprecated HTTP support was removed */
    XML_WITH_HTTP = 10,
    /** DTD validation */
    XML_WITH_VALID = 11,
    /** HTML parser */
    XML_WITH_HTML = 12,
    /** Legacy symbols */
    XML_WITH_LEGACY = 13,
    /** Canonical XML */
    XML_WITH_C14N = 14,
    /** XML Catalogs */
    XML_WITH_CATALOG = 15,
    /** XPath */
    XML_WITH_XPATH = 16,
    /** XPointer */
    XML_WITH_XPTR = 17,
    /** XInclude */
    XML_WITH_XINCLUDE = 18,
    /** iconv */
    XML_WITH_ICONV = 19,
    /** Built-in ISO-8859-X */
    XML_WITH_ISO8859X = 20,
    /** @deprecated Removed */
    XML_WITH_UNICODE = 21,
    /** Regular expressions */
    XML_WITH_REGEXP = 22,
    /** @deprecated Same as XML_WITH_REGEXP */
    XML_WITH_AUTOMATA = 23,
    /** @deprecated Removed */
    XML_WITH_EXPR = 24,
    /** XML Schemas */
    XML_WITH_SCHEMAS = 25,
    /** Schematron */
    XML_WITH_SCHEMATRON = 26,
    /** Loadable modules */
    XML_WITH_MODULES = 27,
    /** Debugging API */
    XML_WITH_DEBUG = 28,
    /** @deprecated Removed */
    XML_WITH_DEBUG_MEM = 29,
    /** @deprecated Removed */
    XML_WITH_DEBUG_RUN = 30,
    /** GZIP compression */
    XML_WITH_ZLIB = 31,
    /** ICU */
    XML_WITH_ICU = 32,
    /** @deprecated LZMA support was removed */
    XML_WITH_LZMA = 33,
    /** RELAXNG, since 2.14 */
    XML_WITH_RELAXNG = 34,
    XML_WITH_NONE = 99999 /* just to be sure of allocation size */
} xmlFeature;

XMLPUBFUN int
		xmlHasFeature		(xmlFeature feature);

#ifdef __cplusplus
}
#endif
#endif /* __XML_PARSER_H__ */
