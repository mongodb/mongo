/**
 * @file
 * 
 * @brief XML Path Language implementation
 * 
 * API for the XML Path Language implementation
 *
 * XML Path Language implementation
 * XPath is a language for addressing parts of an XML document,
 * designed to be used by both XSLT and XPointer
 *     http://www.w3.org/TR/xpath
 *
 * Implements
 * W3C Recommendation 16 November 1999
 *     http://www.w3.org/TR/1999/REC-xpath-19991116
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_XPATH_H__
#define __XML_XPATH_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_XPATH_ENABLED

#include <libxml/xmlerror.h>
#include <libxml/tree.h>
#include <libxml/hash.h>

#ifdef __cplusplus
extern "C" {
#endif

/** XPath context */
typedef struct _xmlXPathContext xmlXPathContext;
typedef xmlXPathContext *xmlXPathContextPtr;
/** XPath parser and evaluation context */
typedef struct _xmlXPathParserContext xmlXPathParserContext;
typedef xmlXPathParserContext *xmlXPathParserContextPtr;

/**
 * The set of XPath error codes.
 */

typedef enum {
    XPATH_EXPRESSION_OK = 0,
    XPATH_NUMBER_ERROR,
    XPATH_UNFINISHED_LITERAL_ERROR,
    XPATH_START_LITERAL_ERROR,
    XPATH_VARIABLE_REF_ERROR,
    XPATH_UNDEF_VARIABLE_ERROR,
    XPATH_INVALID_PREDICATE_ERROR,
    XPATH_EXPR_ERROR,
    XPATH_UNCLOSED_ERROR,
    XPATH_UNKNOWN_FUNC_ERROR,
    XPATH_INVALID_OPERAND,
    XPATH_INVALID_TYPE,
    XPATH_INVALID_ARITY,
    XPATH_INVALID_CTXT_SIZE,
    XPATH_INVALID_CTXT_POSITION,
    XPATH_MEMORY_ERROR,
    XPTR_SYNTAX_ERROR,
    XPTR_RESOURCE_ERROR,
    XPTR_SUB_RESOURCE_ERROR,
    XPATH_UNDEF_PREFIX_ERROR,
    XPATH_ENCODING_ERROR,
    XPATH_INVALID_CHAR_ERROR,
    XPATH_INVALID_CTXT,
    XPATH_STACK_ERROR,
    XPATH_FORBID_VARIABLE_ERROR,
    XPATH_OP_LIMIT_EXCEEDED,
    XPATH_RECURSION_LIMIT_EXCEEDED
} xmlXPathError;

/** XPath node set */
typedef struct _xmlNodeSet xmlNodeSet;
typedef xmlNodeSet *xmlNodeSetPtr;
/**
 * A node-set (an unordered collection of nodes without duplicates).
 */
struct _xmlNodeSet {
    /** number of nodes in the set */
    int nodeNr;
    /** size of the array as allocated */
    int nodeMax;
    /** array of nodes in no particular order */
    xmlNode **nodeTab;
};

/**
 * An expression is evaluated to yield an object, which
 * has one of the following four basic types:
 *
 *   - node-set
 *   - boolean
 *   - number
 *   - string
 */
typedef enum {
    XPATH_UNDEFINED = 0,
    XPATH_NODESET = 1,
    XPATH_BOOLEAN = 2,
    XPATH_NUMBER = 3,
    XPATH_STRING = 4,
    XPATH_USERS = 8,
    XPATH_XSLT_TREE = 9  /* An XSLT value tree, non modifiable */
} xmlXPathObjectType;

/** @cond IGNORE */
#define XPATH_POINT 5
#define XPATH_RANGE 6
#define XPATH_LOCATIONSET 7
/** @endcond */

/** XPath object */
typedef struct _xmlXPathObject xmlXPathObject;
typedef xmlXPathObject *xmlXPathObjectPtr;
/**
 * An XPath object
 */
struct _xmlXPathObject {
    /** object type */
    xmlXPathObjectType type;
    /** node set */
    xmlNodeSet *nodesetval;
    /** boolean */
    int boolval;
    /** number */
    double floatval;
    /** string */
    xmlChar *stringval;
    void *user;
    int index;
    void *user2;
    int index2;
};

/** @cond ignore */

/*
 * unused
 */
typedef int (*xmlXPathConvertFunc) (xmlXPathObject *obj, int type);
typedef struct _xmlXPathType xmlXPathType;
typedef xmlXPathType *xmlXPathTypePtr;
struct _xmlXPathType {
    const xmlChar         *name;		/* the type name */
    xmlXPathConvertFunc func;		/* the conversion function */
};

/*
 * unused
 */
typedef struct _xmlXPathVariable xmlXPathVariable;
typedef xmlXPathVariable *xmlXPathVariablePtr;
struct _xmlXPathVariable {
    const xmlChar       *name;		/* the variable name */
    xmlXPathObject *value;		/* the value */
};

/*
 * unused
 */
typedef void (*xmlXPathEvalFunc)(xmlXPathParserContext *ctxt,
	                         int nargs);
typedef struct _xmlXPathFunct xmlXPathFunct;
typedef xmlXPathFunct *xmlXPathFuncPtr;
struct _xmlXPathFunct {
    const xmlChar      *name;		/* the function name */
    xmlXPathEvalFunc func;		/* the evaluation function */
};

/*
 * unused
 */
typedef xmlXPathObject *(*xmlXPathAxisFunc) (xmlXPathParserContext *ctxt,
				 xmlXPathObject *cur);
typedef struct _xmlXPathAxis xmlXPathAxis;
typedef xmlXPathAxis *xmlXPathAxisPtr;
struct _xmlXPathAxis {
    const xmlChar      *name;		/* the axis name */
    xmlXPathAxisFunc func;		/* the search function */
};

/** @endcond */

/**
 * An XPath function.
 * The arguments (if any) are popped out from the context stack
 * and the result is pushed on the stack.
 *
 * @param ctxt  the XPath interprestation context
 * @param nargs  the number of arguments
 */

typedef void (*xmlXPathFunction) (xmlXPathParserContext *ctxt, int nargs);

/*
 * Function and Variable Lookup.
 */

/**
 * Prototype for callbacks used to plug variable lookup in the XPath
 * engine.
 *
 * @param ctxt  an XPath context
 * @param name  name of the variable
 * @param ns_uri  the namespace name hosting this variable
 * @returns the XPath object value or NULL if not found.
 */
typedef xmlXPathObject *(*xmlXPathVariableLookupFunc) (void *ctxt,
                                         const xmlChar *name,
                                         const xmlChar *ns_uri);

/**
 * Prototype for callbacks used to plug function lookup in the XPath
 * engine.
 *
 * @param ctxt  an XPath context
 * @param name  name of the function
 * @param ns_uri  the namespace name hosting this function
 * @returns the XPath function or NULL if not found.
 */
typedef xmlXPathFunction (*xmlXPathFuncLookupFunc) (void *ctxt,
					 const xmlChar *name,
					 const xmlChar *ns_uri);

/**
 * Flags for XPath engine compilation and runtime
 */
/**
 * check namespaces at compilation
 */
#define XML_XPATH_CHECKNS (1<<0)
/**
 * forbid variables in expression
 */
#define XML_XPATH_NOVAR	  (1<<1)

/**
 * Expression evaluation occurs with respect to a context.
 * he context consists of:
 *    - a node (the context node)
 *    - a node list (the context node list)
 *    - a set of variable bindings
 *    - a function library
 *    - the set of namespace declarations in scope for the expression
 * Following the switch to hash tables, this need to be trimmed up at
 * the next binary incompatible release.
 * The node may be modified when the context is passed to libxml2
 * for an XPath evaluation so you may need to initialize it again
 * before the next call.
 */
struct _xmlXPathContext {
    /** The current document */
    xmlDoc *doc;
    /** The current node */
    xmlNode *node;

    /* unused (hash table) */
    int nb_variables_unused;
    /* unused (hash table) */
    int max_variables_unused;
    /* Hash table of defined variables */
    xmlHashTable *varHash;

    /* number of defined types */
    int nb_types;
    /* max number of types */
    int max_types;
    /* Array of defined types */
    xmlXPathType *types;

    /* unused (hash table) */
    int nb_funcs_unused;
    /* unused (hash table) */
    int max_funcs_unused;
    /* Hash table of defined funcs */
    xmlHashTable *funcHash;

    /* number of defined axis */
    int nb_axis;
    /* max number of axis */
    int max_axis;
    /* Array of defined axis */
    xmlXPathAxis *axis;

    /* Array of namespaces */
    xmlNs **namespaces;
    /* number of namespace in scope */
    int nsNr;
    /* function to free */
    void *user;

    /** the context size */
    int contextSize;
    /** the proximity position */
    int proximityPosition;

    /* is this an XPointer context? */
    int xptr;
    /* for here() */
    xmlNode *here;
    /* for origin() */
    xmlNode *origin;

    /* The namespaces hash table */
    xmlHashTable *nsHash;
    /* variable lookup func */
    xmlXPathVariableLookupFunc varLookupFunc;
    /* variable lookup data */
    void *varLookupData;

    /* needed for XSLT */
    void *extra;

    /* The function name when calling a function */
    const xmlChar *function;
    /* The namespace URI when calling a function */
    const xmlChar *functionURI;

    /* function lookup func */
    xmlXPathFuncLookupFunc funcLookupFunc;
    /* function lookup data */
    void *funcLookupData;

    /* Array of temp namespaces */
    xmlNs **tmpNsList;
    /* number of namespaces in scope */
    int tmpNsNr;

    /* user specific data block */
    void *userData;
    /* the callback in case of errors */
    xmlStructuredErrorFunc error;
    /* the last error */
    xmlError lastError;
    /* the source node XSLT */
    xmlNode *debugNode;

    /* dictionary if any */
    xmlDict *dict;

    /** flags to control compilation */
    int flags;

    /* Cache for reusal of XPath objects */
    void *cache;

    /* Resource limits */
    unsigned long opLimit;
    unsigned long opCount;
    int depth;
};

/** Compiled XPath expression */
typedef struct _xmlXPathCompExpr xmlXPathCompExpr;
typedef xmlXPathCompExpr *xmlXPathCompExprPtr;

/**
 * An XPath parser context. It contains pure parsing information,
 * an xmlXPathContext, and the stack of objects.
 *
 * This struct is used for evaluation as well and misnamed.
 */
struct _xmlXPathParserContext {
    /* the current char being parsed */
    const xmlChar *cur;
    /* the full expression */
    const xmlChar *base;

    /** error code */
    int error;

    /** the evaluation context */
    xmlXPathContext    *context;
    /** the current value */
    xmlXPathObject       *value;
    /* number of values stacked */
    int                 valueNr;
    /* max number of values stacked */
    int                valueMax;
    /* stack of values */
    xmlXPathObject **valueTab;

    /* the precompiled expression */
    xmlXPathCompExpr *comp;
    /* it this an XPointer expression */
    int xptr;
    /* used for walking preceding axis */
    xmlNode           *ancestor;

    /* always zero for compatibility */
    int              valueFrame;
};

/************************************************************************
 *									*
 *			Public API					*
 *									*
 ************************************************************************/

/**
 * Objects and Nodesets handling
 */

/** @cond ignore */

XML_DEPRECATED
XMLPUBVAR double xmlXPathNAN;
XML_DEPRECATED
XMLPUBVAR double xmlXPathPINF;
XML_DEPRECATED
XMLPUBVAR double xmlXPathNINF;

/* These macros may later turn into functions */
/**
 * Implement a functionality similar to the DOM NodeList.length.
 *
 * @param ns  a node-set
 * @returns the number of nodes in the node-set.
 */
#define xmlXPathNodeSetGetLength(ns) ((ns) ? (ns)->nodeNr : 0)
/**
 * Implements a functionality similar to the DOM NodeList.item().
 *
 * @param ns  a node-set
 * @param index  index of a node in the set
 * @returns the xmlNode at the given `index` in `ns` or NULL if
 *         `index` is out of range (0 to length-1)
 */
#define xmlXPathNodeSetItem(ns, index)				\
		((((ns) != NULL) &&				\
		  ((index) >= 0) && ((index) < (ns)->nodeNr)) ?	\
		 (ns)->nodeTab[(index)]				\
		 : NULL)
/**
 * Checks whether `ns` is empty or not.
 *
 * @param ns  a node-set
 * @returns %TRUE if `ns` is an empty node-set.
 */
#define xmlXPathNodeSetIsEmpty(ns)                                      \
    (((ns) == NULL) || ((ns)->nodeNr == 0) || ((ns)->nodeTab == NULL))

/** @endcond */

XMLPUBFUN void
		    xmlXPathFreeObject		(xmlXPathObject *obj);
XMLPUBFUN xmlNodeSet *
		    xmlXPathNodeSetCreate	(xmlNode *val);
XMLPUBFUN void
		    xmlXPathFreeNodeSetList	(xmlXPathObject *obj);
XMLPUBFUN void
		    xmlXPathFreeNodeSet		(xmlNodeSet *obj);
XMLPUBFUN xmlXPathObject *
		    xmlXPathObjectCopy		(xmlXPathObject *val);
XMLPUBFUN int
		    xmlXPathCmpNodes		(xmlNode *node1,
						 xmlNode *node2);
/**
 * Conversion functions to basic types.
 */
XMLPUBFUN int
		    xmlXPathCastNumberToBoolean	(double val);
XMLPUBFUN int
		    xmlXPathCastStringToBoolean	(const xmlChar * val);
XMLPUBFUN int
		    xmlXPathCastNodeSetToBoolean(xmlNodeSet *ns);
XMLPUBFUN int
		    xmlXPathCastToBoolean	(xmlXPathObject *val);

XMLPUBFUN double
		    xmlXPathCastBooleanToNumber	(int val);
XMLPUBFUN double
		    xmlXPathCastStringToNumber	(const xmlChar * val);
XMLPUBFUN double
		    xmlXPathCastNodeToNumber	(xmlNode *node);
XMLPUBFUN double
		    xmlXPathCastNodeSetToNumber	(xmlNodeSet *ns);
XMLPUBFUN double
		    xmlXPathCastToNumber	(xmlXPathObject *val);

XMLPUBFUN xmlChar *
		    xmlXPathCastBooleanToString	(int val);
XMLPUBFUN xmlChar *
		    xmlXPathCastNumberToString	(double val);
XMLPUBFUN xmlChar *
		    xmlXPathCastNodeToString	(xmlNode *node);
XMLPUBFUN xmlChar *
		    xmlXPathCastNodeSetToString	(xmlNodeSet *ns);
XMLPUBFUN xmlChar *
		    xmlXPathCastToString	(xmlXPathObject *val);

XMLPUBFUN xmlXPathObject *
		    xmlXPathConvertBoolean	(xmlXPathObject *val);
XMLPUBFUN xmlXPathObject *
		    xmlXPathConvertNumber	(xmlXPathObject *val);
XMLPUBFUN xmlXPathObject *
		    xmlXPathConvertString	(xmlXPathObject *val);

/**
 * Context handling.
 */
XMLPUBFUN xmlXPathContext *
		    xmlXPathNewContext		(xmlDoc *doc);
XMLPUBFUN void
		    xmlXPathFreeContext		(xmlXPathContext *ctxt);
XMLPUBFUN void
		    xmlXPathSetErrorHandler(xmlXPathContext *ctxt,
					    xmlStructuredErrorFunc handler,
					    void *context);
XMLPUBFUN int
		    xmlXPathContextSetCache(xmlXPathContext *ctxt,
				            int active,
					    int value,
					    int options);
/**
 * Evaluation functions.
 */
XMLPUBFUN long
		    xmlXPathOrderDocElems	(xmlDoc *doc);
XMLPUBFUN int
		    xmlXPathSetContextNode	(xmlNode *node,
						 xmlXPathContext *ctx);
XMLPUBFUN xmlXPathObject *
		    xmlXPathNodeEval		(xmlNode *node,
						 const xmlChar *str,
						 xmlXPathContext *ctx);
XMLPUBFUN xmlXPathObject *
		    xmlXPathEval		(const xmlChar *str,
						 xmlXPathContext *ctx);
XMLPUBFUN xmlXPathObject *
		    xmlXPathEvalExpression	(const xmlChar *str,
						 xmlXPathContext *ctxt);
XMLPUBFUN int
		    xmlXPathEvalPredicate	(xmlXPathContext *ctxt,
						 xmlXPathObject *res);
/**
 * Separate compilation/evaluation entry points.
 */
XMLPUBFUN xmlXPathCompExpr *
		    xmlXPathCompile		(const xmlChar *str);
XMLPUBFUN xmlXPathCompExpr *
		    xmlXPathCtxtCompile		(xmlXPathContext *ctxt,
						 const xmlChar *str);
XMLPUBFUN xmlXPathObject *
		    xmlXPathCompiledEval	(xmlXPathCompExpr *comp,
						 xmlXPathContext *ctx);
XMLPUBFUN int
		    xmlXPathCompiledEvalToBoolean(xmlXPathCompExpr *comp,
						 xmlXPathContext *ctxt);
XMLPUBFUN void
		    xmlXPathFreeCompExpr	(xmlXPathCompExpr *comp);

XML_DEPRECATED
XMLPUBFUN void
		    xmlXPathInit		(void);
XMLPUBFUN int
		xmlXPathIsNaN	(double val);
XMLPUBFUN int
		xmlXPathIsInf	(double val);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_XPATH_ENABLED */
#endif /* ! __XML_XPATH_H__ */
