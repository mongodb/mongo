/**
 * @file
 * 
 * @brief internal interfaces for XML Path Language implementation
 * 
 * internal interfaces for XML Path Language implementation
 *              used to build new modules on top of XPath like XPointer and
 *              XSLT
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_XPATH_INTERNALS_H__
#define __XML_XPATH_INTERNALS_H__

#include <stdio.h>
#include <libxml/xmlversion.h>
#include <libxml/xpath.h>

#ifdef LIBXML_XPATH_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Push a value on the stack
 *
 * @deprecated Use #xmlXPathValuePush
 */
#define valuePush xmlXPathValuePush
/**
 * Pop a value from the stack
 *
 * @deprecated Use #xmlXPathValuePop
 */
#define valuePop xmlXPathValuePop

/************************************************************************
 *									*
 *			Helpers						*
 *									*
 ************************************************************************/

/*
 * Many of these macros may later turn into functions. They
 * shouldn't be used in \#ifdef's preprocessor instructions.
 */
/**
 * Raises an error.
 *
 * @param ctxt  an XPath parser context
 * @param err  an xmlXPathError code
 */
#define xmlXPathSetError(ctxt, err)					\
    { xmlXPatherror((ctxt), __FILE__, __LINE__, (err));			\
      if ((ctxt) != NULL) (ctxt)->error = (err); }

/**
 * Raises an XPATH_INVALID_ARITY error.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathSetArityError(ctxt)					\
    xmlXPathSetError((ctxt), XPATH_INVALID_ARITY)

/**
 * Raises an XPATH_INVALID_TYPE error.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathSetTypeError(ctxt)					\
    xmlXPathSetError((ctxt), XPATH_INVALID_TYPE)

/**
 * Get the error code of an XPath context.
 *
 * @param ctxt  an XPath parser context
 * @returns the context error.
 */
#define xmlXPathGetError(ctxt)	  ((ctxt)->error)

/**
 * Check if an XPath error was raised.
 *
 * @param ctxt  an XPath parser context
 * @returns true if an error has been raised, false otherwise.
 */
#define xmlXPathCheckError(ctxt)  ((ctxt)->error != XPATH_EXPRESSION_OK)

/**
 * Get the document of an XPath context.
 *
 * @param ctxt  an XPath parser context
 * @returns the context document.
 */
#define xmlXPathGetDocument(ctxt)	((ctxt)->context->doc)

/**
 * Get the context node of an XPath context.
 *
 * @param ctxt  an XPath parser context
 * @returns the context node.
 */
#define xmlXPathGetContextNode(ctxt)	((ctxt)->context->node)

XMLPUBFUN int
		xmlXPathPopBoolean	(xmlXPathParserContext *ctxt);
XMLPUBFUN double
		xmlXPathPopNumber	(xmlXPathParserContext *ctxt);
XMLPUBFUN xmlChar *
		xmlXPathPopString	(xmlXPathParserContext *ctxt);
XMLPUBFUN xmlNodeSet *
		xmlXPathPopNodeSet	(xmlXPathParserContext *ctxt);
XMLPUBFUN void *
		xmlXPathPopExternal	(xmlXPathParserContext *ctxt);

/**
 * Pushes the boolean `val` on the context stack.
 *
 * @param ctxt  an XPath parser context
 * @param val  a boolean
 */
#define xmlXPathReturnBoolean(ctxt, val)				\
    valuePush((ctxt), xmlXPathNewBoolean(val))

/**
 * Pushes true on the context stack.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathReturnTrue(ctxt)   xmlXPathReturnBoolean((ctxt), 1)

/**
 * Pushes false on the context stack.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathReturnFalse(ctxt)  xmlXPathReturnBoolean((ctxt), 0)

/**
 * Pushes the double `val` on the context stack.
 *
 * @param ctxt  an XPath parser context
 * @param val  a double
 */
#define xmlXPathReturnNumber(ctxt, val)					\
    valuePush((ctxt), xmlXPathNewFloat(val))

/**
 * Pushes the string `str` on the context stack.
 *
 * @param ctxt  an XPath parser context
 * @param str  a string
 */
#define xmlXPathReturnString(ctxt, str)					\
    valuePush((ctxt), xmlXPathWrapString(str))

/**
 * Pushes an empty string on the stack.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathReturnEmptyString(ctxt)					\
    valuePush((ctxt), xmlXPathNewCString(""))

/**
 * Pushes the node-set `ns` on the context stack.
 *
 * @param ctxt  an XPath parser context
 * @param ns  a node-set
 */
#define xmlXPathReturnNodeSet(ctxt, ns)					\
    valuePush((ctxt), xmlXPathWrapNodeSet(ns))

/**
 * Pushes an empty node-set on the context stack.
 *
 * @param ctxt  an XPath parser context
 */
#define xmlXPathReturnEmptyNodeSet(ctxt)				\
    valuePush((ctxt), xmlXPathNewNodeSet(NULL))

/**
 * Pushes user data on the context stack.
 *
 * @param ctxt  an XPath parser context
 * @param val  user data
 */
#define xmlXPathReturnExternal(ctxt, val)				\
    valuePush((ctxt), xmlXPathWrapExternal(val))

/**
 * Check if the current value on the XPath stack is a node set or
 * an XSLT value tree.
 *
 * @param ctxt  an XPath parser context
 * @returns true if the current object on the stack is a node-set.
 */
#define xmlXPathStackIsNodeSet(ctxt)					\
    (((ctxt)->value != NULL)						\
     && (((ctxt)->value->type == XPATH_NODESET)				\
         || ((ctxt)->value->type == XPATH_XSLT_TREE)))

/**
 * Checks if the current value on the XPath stack is an external
 * object.
 *
 * @param ctxt  an XPath parser context
 * @returns true if the current object on the stack is an external
 * object.
 */
#define xmlXPathStackIsExternal(ctxt)					\
	((ctxt->value != NULL) && (ctxt->value->type == XPATH_USERS))

/**
 * Empties a node-set.
 *
 * @param ns  a node-set
 */
#define xmlXPathEmptyNodeSet(ns)					\
    { while ((ns)->nodeNr > 0) (ns)->nodeTab[--(ns)->nodeNr] = NULL; }

/**
 * Macro to return from the function if an XPath error was detected.
 */
#define CHECK_ERROR							\
    if (ctxt->error != XPATH_EXPRESSION_OK) return

/**
 * Macro to return 0 from the function if an XPath error was detected.
 */
#define CHECK_ERROR0							\
    if (ctxt->error != XPATH_EXPRESSION_OK) return(0)

/**
 * Macro to raise an XPath error and return.
 *
 * @param X  the error code
 */
#define XP_ERROR(X)							\
    { xmlXPathErr(ctxt, X); return; }

/**
 * Macro to raise an XPath error and return 0.
 *
 * @param X  the error code
 */
#define XP_ERROR0(X)							\
    { xmlXPathErr(ctxt, X); return(0); }

/**
 * Macro to check that the value on top of the XPath stack is of a given
 * type.
 *
 * @param typeval  the XPath type
 */
#define CHECK_TYPE(typeval)						\
    if ((ctxt->value == NULL) || (ctxt->value->type != typeval))	\
        XP_ERROR(XPATH_INVALID_TYPE)

/**
 * Macro to check that the value on top of the XPath stack is of a given
 * type. Return(0) in case of failure
 *
 * @param typeval  the XPath type
 */
#define CHECK_TYPE0(typeval)						\
    if ((ctxt->value == NULL) || (ctxt->value->type != typeval))	\
        XP_ERROR0(XPATH_INVALID_TYPE)

/**
 * Macro to check that the number of args passed to an XPath function matches.
 *
 * @param x  the number of expected args
 */
#define CHECK_ARITY(x)							\
    if (ctxt == NULL) return;						\
    if (nargs != (x))							\
        XP_ERROR(XPATH_INVALID_ARITY);					\
    if (ctxt->valueNr < (x))						\
        XP_ERROR(XPATH_STACK_ERROR);

/**
 * Macro to try to cast the value on the top of the XPath stack to a string.
 */
#define CAST_TO_STRING							\
    if ((ctxt->value != NULL) && (ctxt->value->type != XPATH_STRING))	\
        xmlXPathStringFunction(ctxt, 1);

/**
 * Macro to try to cast the value on the top of the XPath stack to a number.
 */
#define CAST_TO_NUMBER							\
    if ((ctxt->value != NULL) && (ctxt->value->type != XPATH_NUMBER))	\
        xmlXPathNumberFunction(ctxt, 1);

/**
 * Macro to try to cast the value on the top of the XPath stack to a boolean.
 */
#define CAST_TO_BOOLEAN							\
    if ((ctxt->value != NULL) && (ctxt->value->type != XPATH_BOOLEAN))	\
        xmlXPathBooleanFunction(ctxt, 1);

/*
 * Variable Lookup forwarding.
 */

XMLPUBFUN void
	xmlXPathRegisterVariableLookup	(xmlXPathContext *ctxt,
					 xmlXPathVariableLookupFunc f,
					 void *data);

/*
 * Function Lookup forwarding.
 */

XMLPUBFUN void
	    xmlXPathRegisterFuncLookup	(xmlXPathContext *ctxt,
					 xmlXPathFuncLookupFunc f,
					 void *funcCtxt);

/*
 * Error reporting.
 */
XMLPUBFUN void
		xmlXPatherror	(xmlXPathParserContext *ctxt,
				 const char *file,
				 int line,
				 int no);

XMLPUBFUN void
		xmlXPathErr	(xmlXPathParserContext *ctxt,
				 int error);

#ifdef LIBXML_DEBUG_ENABLED
XMLPUBFUN void
		xmlXPathDebugDumpObject	(FILE *output,
					 xmlXPathObject *cur,
					 int depth);
XMLPUBFUN void
	    xmlXPathDebugDumpCompExpr(FILE *output,
					 xmlXPathCompExpr *comp,
					 int depth);
#endif
/**
 * NodeSet handling.
 */
XMLPUBFUN int
		xmlXPathNodeSetContains		(xmlNodeSet *cur,
						 xmlNode *val);
XMLPUBFUN xmlNodeSet *
		xmlXPathDifference		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);
XMLPUBFUN xmlNodeSet *
		xmlXPathIntersection		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);

XMLPUBFUN xmlNodeSet *
		xmlXPathDistinctSorted		(xmlNodeSet *nodes);
XMLPUBFUN xmlNodeSet *
		xmlXPathDistinct		(xmlNodeSet *nodes);

XMLPUBFUN int
		xmlXPathHasSameNodes		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);

XMLPUBFUN xmlNodeSet *
		xmlXPathNodeLeadingSorted	(xmlNodeSet *nodes,
						 xmlNode *node);
XMLPUBFUN xmlNodeSet *
		xmlXPathLeadingSorted		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);
XMLPUBFUN xmlNodeSet *
		xmlXPathNodeLeading		(xmlNodeSet *nodes,
						 xmlNode *node);
XMLPUBFUN xmlNodeSet *
		xmlXPathLeading			(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);

XMLPUBFUN xmlNodeSet *
		xmlXPathNodeTrailingSorted	(xmlNodeSet *nodes,
						 xmlNode *node);
XMLPUBFUN xmlNodeSet *
		xmlXPathTrailingSorted		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);
XMLPUBFUN xmlNodeSet *
		xmlXPathNodeTrailing		(xmlNodeSet *nodes,
						 xmlNode *node);
XMLPUBFUN xmlNodeSet *
		xmlXPathTrailing		(xmlNodeSet *nodes1,
						 xmlNodeSet *nodes2);


/**
 * Extending a context.
 */

XMLPUBFUN int
		xmlXPathRegisterNs		(xmlXPathContext *ctxt,
						 const xmlChar *prefix,
						 const xmlChar *ns_uri);
XMLPUBFUN const xmlChar *
		xmlXPathNsLookup		(xmlXPathContext *ctxt,
						 const xmlChar *prefix);
XMLPUBFUN void
		xmlXPathRegisteredNsCleanup	(xmlXPathContext *ctxt);

XMLPUBFUN int
		xmlXPathRegisterFunc		(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 xmlXPathFunction f);
XMLPUBFUN int
		xmlXPathRegisterFuncNS		(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 const xmlChar *ns_uri,
						 xmlXPathFunction f);
XMLPUBFUN int
		xmlXPathRegisterVariable	(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 xmlXPathObject *value);
XMLPUBFUN int
		xmlXPathRegisterVariableNS	(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 const xmlChar *ns_uri,
						 xmlXPathObject *value);
XMLPUBFUN xmlXPathFunction
		xmlXPathFunctionLookup		(xmlXPathContext *ctxt,
						 const xmlChar *name);
XMLPUBFUN xmlXPathFunction
		xmlXPathFunctionLookupNS	(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 const xmlChar *ns_uri);
XMLPUBFUN void
		xmlXPathRegisteredFuncsCleanup	(xmlXPathContext *ctxt);
XMLPUBFUN xmlXPathObject *
		xmlXPathVariableLookup		(xmlXPathContext *ctxt,
						 const xmlChar *name);
XMLPUBFUN xmlXPathObject *
		xmlXPathVariableLookupNS	(xmlXPathContext *ctxt,
						 const xmlChar *name,
						 const xmlChar *ns_uri);
XMLPUBFUN void
		xmlXPathRegisteredVariablesCleanup(xmlXPathContext *ctxt);

/**
 * Utilities to extend XPath.
 */
XMLPUBFUN xmlXPathParserContext *
		  xmlXPathNewParserContext	(const xmlChar *str,
						 xmlXPathContext *ctxt);
XMLPUBFUN void
		xmlXPathFreeParserContext	(xmlXPathParserContext *ctxt);

XMLPUBFUN xmlXPathObject *
		xmlXPathValuePop		(xmlXPathParserContext *ctxt);
XMLPUBFUN int
		xmlXPathValuePush		(xmlXPathParserContext *ctxt,
						 xmlXPathObject *value);

XMLPUBFUN xmlXPathObject *
		xmlXPathNewString		(const xmlChar *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewCString		(const char *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathWrapString		(xmlChar *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathWrapCString		(char * val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewFloat		(double val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewBoolean		(int val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewNodeSet		(xmlNode *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewValueTree		(xmlNode *val);
XMLPUBFUN int
		xmlXPathNodeSetAdd		(xmlNodeSet *cur,
						 xmlNode *val);
XMLPUBFUN int
		xmlXPathNodeSetAddUnique	(xmlNodeSet *cur,
						 xmlNode *val);
XMLPUBFUN int
		xmlXPathNodeSetAddNs		(xmlNodeSet *cur,
						 xmlNode *node,
						 xmlNs *ns);
XMLPUBFUN void
		xmlXPathNodeSetSort		(xmlNodeSet *set);

XMLPUBFUN void
		xmlXPathRoot			(xmlXPathParserContext *ctxt);
XML_DEPRECATED
XMLPUBFUN void
		xmlXPathEvalExpr		(xmlXPathParserContext *ctxt);
XMLPUBFUN xmlChar *
		xmlXPathParseName		(xmlXPathParserContext *ctxt);
XMLPUBFUN xmlChar *
		xmlXPathParseNCName		(xmlXPathParserContext *ctxt);

/*
 * Existing functions.
 */
XMLPUBFUN double
		xmlXPathStringEvalNumber	(const xmlChar *str);
XMLPUBFUN int
		xmlXPathEvaluatePredicateResult (xmlXPathParserContext *ctxt,
						 xmlXPathObject *res);
XMLPUBFUN void
		xmlXPathRegisterAllFunctions	(xmlXPathContext *ctxt);
XMLPUBFUN xmlNodeSet *
		xmlXPathNodeSetMerge		(xmlNodeSet *val1,
						 xmlNodeSet *val2);
XMLPUBFUN void
		xmlXPathNodeSetDel		(xmlNodeSet *cur,
						 xmlNode *val);
XMLPUBFUN void
		xmlXPathNodeSetRemove		(xmlNodeSet *cur,
						 int val);
XMLPUBFUN xmlXPathObject *
		xmlXPathNewNodeSetList		(xmlNodeSet *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathWrapNodeSet		(xmlNodeSet *val);
XMLPUBFUN xmlXPathObject *
		xmlXPathWrapExternal		(void *val);

XMLPUBFUN int xmlXPathEqualValues(xmlXPathParserContext *ctxt);
XMLPUBFUN int xmlXPathNotEqualValues(xmlXPathParserContext *ctxt);
XMLPUBFUN int xmlXPathCompareValues(xmlXPathParserContext *ctxt, int inf, int strict);
XMLPUBFUN void xmlXPathValueFlipSign(xmlXPathParserContext *ctxt);
XMLPUBFUN void xmlXPathAddValues(xmlXPathParserContext *ctxt);
XMLPUBFUN void xmlXPathSubValues(xmlXPathParserContext *ctxt);
XMLPUBFUN void xmlXPathMultValues(xmlXPathParserContext *ctxt);
XMLPUBFUN void xmlXPathDivValues(xmlXPathParserContext *ctxt);
XMLPUBFUN void xmlXPathModValues(xmlXPathParserContext *ctxt);

XMLPUBFUN int xmlXPathIsNodeType(const xmlChar *name);

/*
 * Some of the axis navigation routines.
 */
XMLPUBFUN xmlNode *xmlXPathNextSelf(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextChild(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextDescendant(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextDescendantOrSelf(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextParent(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextAncestorOrSelf(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextFollowingSibling(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextFollowing(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextNamespace(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextAttribute(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextPreceding(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextAncestor(xmlXPathParserContext *ctxt,
			xmlNode *cur);
XMLPUBFUN xmlNode *xmlXPathNextPrecedingSibling(xmlXPathParserContext *ctxt,
			xmlNode *cur);
/*
 * The official core of XPath functions.
 */
XMLPUBFUN void xmlXPathLastFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathPositionFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathCountFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathIdFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathLocalNameFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathNamespaceURIFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathStringFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathStringLengthFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathConcatFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathContainsFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathStartsWithFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathSubstringFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathSubstringBeforeFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathSubstringAfterFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathNormalizeFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathTranslateFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathNotFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathTrueFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathFalseFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathLangFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathNumberFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathSumFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathFloorFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathCeilingFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathRoundFunction(xmlXPathParserContext *ctxt, int nargs);
XMLPUBFUN void xmlXPathBooleanFunction(xmlXPathParserContext *ctxt, int nargs);

/**
 * Really internal functions
 */
XMLPUBFUN void xmlXPathNodeSetFreeNs(xmlNs *ns);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_XPATH_ENABLED */
#endif /* ! __XML_XPATH_INTERNALS_H__ */
