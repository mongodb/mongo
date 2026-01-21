/*
 * xpointer.c : Code to handle XML Pointer
 *
 * Base implementation was made accordingly to
 * W3C Candidate Recommendation 7 June 2000
 * http://www.w3.org/TR/2000/CR-xptr-20000607
 *
 * Added support for the element() scheme described in:
 * W3C Proposed Recommendation 13 November 2002
 * http://www.w3.org/TR/2002/PR-xptr-element-20021113/
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

/* To avoid EBCDIC trouble when parsing on zOS */
#if defined(__MVS__)
#pragma convert("ISO8859-1")
#endif

#define IN_LIBXML
#include "libxml.h"

/*
 * TODO: better handling of error cases, the full expression should
 *       be parsed beforehand instead of a progressive evaluation
 * TODO: Access into entities references are not supported now ...
 *       need a start to be able to pop out of entities refs since
 *       parent is the entity declaration, not the ref.
 */

#include <string.h>
#include <libxml/xpointer.h>
#include <libxml/xmlmemory.h>
#include <libxml/parserInternals.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlerror.h>

#ifdef LIBXML_XPTR_ENABLED

/* Add support of the xmlns() xpointer scheme to initialize the namespaces */
#define XPTR_XMLNS_SCHEME

#include "private/error.h"
#include "private/xpath.h"

/************************************************************************
 *									*
 *		Some factorized error routines				*
 *									*
 ************************************************************************/

/**
 * Handle an XPointer error
 *
 * @param ctxt  an XPTR evaluation context
 * @param code  error code
 * @param msg  error message
 * @param extra  extra information
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlXPtrErr(xmlXPathParserContextPtr ctxt, int code,
           const char * msg, const xmlChar *extra)
{
    xmlStructuredErrorFunc serror = NULL;
    void *data = NULL;
    xmlNodePtr node = NULL;
    int res;

    if (ctxt == NULL)
        return;
    /* Only report the first error */
    if (ctxt->error != 0)
        return;

    ctxt->error = code;

    if (ctxt->context != NULL) {
        xmlErrorPtr err = &ctxt->context->lastError;

        /* cleanup current last error */
        xmlResetError(err);

        err->domain = XML_FROM_XPOINTER;
        err->code = code;
        err->level = XML_ERR_ERROR;
        err->str1 = (char *) xmlStrdup(ctxt->base);
        if (err->str1 == NULL) {
            xmlXPathPErrMemory(ctxt);
            return;
        }
        err->int1 = ctxt->cur - ctxt->base;
        err->node = ctxt->context->debugNode;

        serror = ctxt->context->error;
        data = ctxt->context->userData;
        node = ctxt->context->debugNode;
    }

    res = xmlRaiseError(serror, NULL, data, NULL, node,
                        XML_FROM_XPOINTER, code, XML_ERR_ERROR, NULL, 0,
                        (const char *) extra, (const char *) ctxt->base,
                        NULL, ctxt->cur - ctxt->base, 0,
                        msg, extra);
    if (res < 0)
        xmlXPathPErrMemory(ctxt);
}

/************************************************************************
 *									*
 *		A few helper functions for child sequences		*
 *									*
 ************************************************************************/

/**
 * @param cur  the node
 * @param no  the child number
 * @returns the `no`'th element child of `cur` or NULL
 */
static xmlNodePtr
xmlXPtrGetNthChild(xmlNodePtr cur, int no) {
    int i;
    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL))
	return(cur);
    cur = cur->children;
    for (i = 0;i <= no;cur = cur->next) {
	if (cur == NULL)
	    return(cur);
	if ((cur->type == XML_ELEMENT_NODE) ||
	    (cur->type == XML_DOCUMENT_NODE) ||
	    (cur->type == XML_HTML_DOCUMENT_NODE)) {
	    i++;
	    if (i == no)
		break;
	}
    }
    return(cur);
}

/************************************************************************
 *									*
 *			The parser					*
 *									*
 ************************************************************************/

static void xmlXPtrEvalChildSeq(xmlXPathParserContextPtr ctxt, xmlChar *name);

/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one need to make assumption on the context to use them
 *
 *   CUR     returns the current xmlChar value, i.e. a 8 bit value
 *           in ISO-Latin or UTF-8.
 *           This should be used internally by the parser
 *           only to compare to ASCII values otherwise it would break when
 *           running with UTF-8 encoding.
 *   NXT(n)  returns the n'th next xmlChar. Same as CUR is should be used only
 *           to compare on ASCII based substring.
 *   SKIP(n) Skip n xmlChar, and must also be used only to skip ASCII defined
 *           strings within the parser.
 *   CURRENT Returns the current char value, with the full decoding of
 *           UTF-8 if we are using this mode. It returns an int.
 *   NEXT    Skip to the next character, this does the proper decoding
 *           in UTF-8 mode. It also pop-up unfinished entities on the fly.
 *           It returns the pointer to the current xmlChar.
 */

#define CUR (*ctxt->cur)
#define SKIP(val) ctxt->cur += (val)
#define NXT(val) ctxt->cur[(val)]

#define SKIP_BLANKS							\
    while (IS_BLANK_CH(*(ctxt->cur))) NEXT

#define CURRENT (*ctxt->cur)
#define NEXT ((*ctxt->cur) ?  ctxt->cur++: ctxt->cur)

/*
 * @param ctxt  the XPointer Parser context
 * @param index  the child number
 *
 * Move the current node of the nodeset on the stack to the
 * given child if found
 */
static void
xmlXPtrGetChildNo(xmlXPathParserContextPtr ctxt, int indx) {
    xmlNodePtr cur = NULL;
    xmlXPathObjectPtr obj;
    xmlNodeSetPtr oldset;

    CHECK_TYPE(XPATH_NODESET);
    obj = xmlXPathValuePop(ctxt);
    oldset = obj->nodesetval;
    if ((indx <= 0) || (oldset == NULL) || (oldset->nodeNr != 1)) {
	xmlXPathFreeObject(obj);
	xmlXPathValuePush(ctxt, xmlXPathNewNodeSet(NULL));
	return;
    }
    cur = xmlXPtrGetNthChild(oldset->nodeTab[0], indx);
    if (cur == NULL) {
	xmlXPathFreeObject(obj);
	xmlXPathValuePush(ctxt, xmlXPathNewNodeSet(NULL));
	return;
    }
    oldset->nodeTab[0] = cur;
    xmlXPathValuePush(ctxt, obj);
}

/**
 * XPtrPart ::= 'xpointer' '(' XPtrExpr ')'
 *            | Scheme '(' SchemeSpecificExpr ')'
 *
 * Scheme   ::=  NCName - 'xpointer' [VC: Non-XPointer schemes]
 *
 * SchemeSpecificExpr ::= StringWithBalancedParens
 *
 * StringWithBalancedParens ::=
 *              [^()]* ('(' StringWithBalancedParens ')' [^()]*)*
 *              [VC: Parenthesis escaping]
 *
 * XPtrExpr ::= Expr [VC: Parenthesis escaping]
 *
 * VC: Parenthesis escaping:
 *   The end of an XPointer part is signaled by the right parenthesis ")"
 *   character that is balanced with the left parenthesis "(" character
 *   that began the part. Any unbalanced parenthesis character inside the
 *   expression, even within literals, must be escaped with a circumflex (^)
 *   character preceding it. If the expression contains any literal
 *   occurrences of the circumflex, each must be escaped with an additional
 *   circumflex (that is, ^^). If the unescaped parentheses in the expression
 *   are not balanced, a syntax error results.
 *
 * Parse and evaluate an XPtrPart. Basically it generates the unescaped
 * string and if the scheme is 'xpointer' it will call the XPath interpreter.
 *
 * TODO: there is no new scheme registration mechanism
 *
 * @param ctxt  the XPointer Parser context
 * @param name  the preparsed Scheme for the XPtrPart
 */

static void
xmlXPtrEvalXPtrPart(xmlXPathParserContextPtr ctxt, xmlChar *name) {
    xmlChar *buffer, *cur;
    int len;
    int level;

    if (name == NULL)
    name = xmlXPathParseName(ctxt);
    if (name == NULL)
	XP_ERROR(XPATH_EXPR_ERROR);

    if (CUR != '(') {
        xmlFree(name);
	XP_ERROR(XPATH_EXPR_ERROR);
    }
    NEXT;
    level = 1;

    len = xmlStrlen(ctxt->cur);
    len++;
    buffer = xmlMalloc(len);
    if (buffer == NULL) {
        xmlXPathPErrMemory(ctxt);
        xmlFree(name);
	return;
    }

    cur = buffer;
    while (CUR != 0) {
	if (CUR == ')') {
	    level--;
	    if (level == 0) {
		NEXT;
		break;
	    }
	} else if (CUR == '(') {
	    level++;
	} else if (CUR == '^') {
            if ((NXT(1) == ')') || (NXT(1) == '(') || (NXT(1) == '^')) {
                NEXT;
            }
	}
        *cur++ = CUR;
	NEXT;
    }
    *cur = 0;

    if ((level != 0) && (CUR == 0)) {
        xmlFree(name);
	xmlFree(buffer);
	XP_ERROR(XPTR_SYNTAX_ERROR);
    }

    if (xmlStrEqual(name, (xmlChar *) "xpointer") ||
        xmlStrEqual(name, (xmlChar *) "xpath1")) {
	const xmlChar *oldBase = ctxt->base;
	const xmlChar *oldCur = ctxt->cur;

	ctxt->cur = ctxt->base = buffer;
	/*
	 * To evaluate an xpointer scheme element (4.3) we need:
	 *   context initialized to the root
	 *   context position initialized to 1
	 *   context size initialized to 1
	 */
	ctxt->context->node = (xmlNodePtr)ctxt->context->doc;
	ctxt->context->proximityPosition = 1;
	ctxt->context->contextSize = 1;
	xmlXPathEvalExpr(ctxt);
	ctxt->base = oldBase;
        ctxt->cur = oldCur;
    } else if (xmlStrEqual(name, (xmlChar *) "element")) {
	const xmlChar *oldBase = ctxt->base;
	const xmlChar *oldCur = ctxt->cur;
	xmlChar *name2;

	ctxt->cur = ctxt->base = buffer;
	if (buffer[0] == '/') {
	    xmlXPathRoot(ctxt);
	    xmlXPtrEvalChildSeq(ctxt, NULL);
	} else {
	    name2 = xmlXPathParseName(ctxt);
	    if (name2 == NULL) {
                ctxt->base = oldBase;
                ctxt->cur = oldCur;
		xmlFree(buffer);
                xmlFree(name);
		XP_ERROR(XPATH_EXPR_ERROR);
	    }
	    xmlXPtrEvalChildSeq(ctxt, name2);
	}
	ctxt->base = oldBase;
        ctxt->cur = oldCur;
#ifdef XPTR_XMLNS_SCHEME
    } else if (xmlStrEqual(name, (xmlChar *) "xmlns")) {
	const xmlChar *oldBase = ctxt->base;
	const xmlChar *oldCur = ctxt->cur;
	xmlChar *prefix;

	ctxt->cur = ctxt->base = buffer;
        prefix = xmlXPathParseNCName(ctxt);
	if (prefix == NULL) {
            ctxt->base = oldBase;
            ctxt->cur = oldCur;
	    xmlFree(buffer);
	    xmlFree(name);
	    XP_ERROR(XPTR_SYNTAX_ERROR);
	}
	SKIP_BLANKS;
	if (CUR != '=') {
            ctxt->base = oldBase;
            ctxt->cur = oldCur;
	    xmlFree(prefix);
	    xmlFree(buffer);
	    xmlFree(name);
	    XP_ERROR(XPTR_SYNTAX_ERROR);
	}
	NEXT;
	SKIP_BLANKS;

	if (xmlXPathRegisterNs(ctxt->context, prefix, ctxt->cur) < 0)
            xmlXPathPErrMemory(ctxt);
        ctxt->base = oldBase;
        ctxt->cur = oldCur;
	xmlFree(prefix);
#endif /* XPTR_XMLNS_SCHEME */
    } else {
        xmlXPtrErr(ctxt, XML_XPTR_UNKNOWN_SCHEME,
		   "unsupported scheme '%s'\n", name);
    }
    xmlFree(buffer);
    xmlFree(name);
}

/**
 * FullXPtr ::= XPtrPart (S? XPtrPart)*
 *
 * As the specs says:
 * -----------
 * When multiple XPtrParts are provided, they must be evaluated in
 * left-to-right order. If evaluation of one part fails, the nexti
 * is evaluated. The following conditions cause XPointer part failure:
 *
 * - An unknown scheme
 * - A scheme that does not locate any sub-resource present in the resource
 * - A scheme that is not applicable to the media type of the resource
 *
 * The XPointer application must consume a failed XPointer part and
 * attempt to evaluate the next one, if any. The result of the first
 * XPointer part whose evaluation succeeds is taken to be the fragment
 * located by the XPointer as a whole. If all the parts fail, the result
 * for the XPointer as a whole is a sub-resource error.
 * -----------
 *
 * Parse and evaluate a Full XPtr i.e. possibly a cascade of XPath based
 * expressions or other schemes.
 *
 * @param ctxt  the XPointer Parser context
 * @param name  the preparsed Scheme for the first XPtrPart
 */
static void
xmlXPtrEvalFullXPtr(xmlXPathParserContextPtr ctxt, xmlChar *name) {
    if (name == NULL)
    name = xmlXPathParseName(ctxt);
    if (name == NULL)
	XP_ERROR(XPATH_EXPR_ERROR);
    while (name != NULL) {
	ctxt->error = XPATH_EXPRESSION_OK;
	xmlXPtrEvalXPtrPart(ctxt, name);

	/* in case of syntax error, break here */
	if ((ctxt->error != XPATH_EXPRESSION_OK) &&
            (ctxt->error != XML_XPTR_UNKNOWN_SCHEME))
	    return;

	/*
	 * If the returned value is a non-empty nodeset
	 * or location set, return here.
	 */
	if (ctxt->value != NULL) {
	    xmlXPathObjectPtr obj = ctxt->value;

	    switch (obj->type) {
		case XPATH_NODESET: {
		    xmlNodeSetPtr loc = ctxt->value->nodesetval;
		    if ((loc != NULL) && (loc->nodeNr > 0))
			return;
		    break;
		}
		default:
		    break;
	    }

	    /*
	     * Evaluating to improper values is equivalent to
	     * a sub-resource error, clean-up the stack
	     */
	    do {
		obj = xmlXPathValuePop(ctxt);
		if (obj != NULL) {
		    xmlXPathFreeObject(obj);
		}
	    } while (obj != NULL);
	}

	/*
	 * Is there another XPointer part.
	 */
	SKIP_BLANKS;
	name = xmlXPathParseName(ctxt);
    }
}

/**
 *  ChildSeq ::= '/1' ('/' [0-9]*)*
 *             | Name ('/' [0-9]*)+
 *
 * Parse and evaluate a Child Sequence. This routine also handle the
 * case of a Bare Name used to get a document ID.
 *
 * @param ctxt  the XPointer Parser context
 * @param name  a possible ID name of the child sequence
 */
static void
xmlXPtrEvalChildSeq(xmlXPathParserContextPtr ctxt, xmlChar *name) {
    /*
     * XPointer don't allow by syntax to address in multirooted trees
     * this might prove useful in some cases, warn about it.
     */
    if ((name == NULL) && (CUR == '/') && (NXT(1) != '1')) {
        xmlXPtrErr(ctxt, XML_XPTR_CHILDSEQ_START,
		   "warning: ChildSeq not starting by /1\n", NULL);
    }

    if (name != NULL) {
	xmlXPathValuePush(ctxt, xmlXPathNewString(name));
	xmlFree(name);
	xmlXPathIdFunction(ctxt, 1);
	CHECK_ERROR;
    }

    while (CUR == '/') {
	int child = 0, overflow = 0;
	NEXT;

	while ((CUR >= '0') && (CUR <= '9')) {
            int d = CUR - '0';
            if (child > INT_MAX / 10)
                overflow = 1;
            else
                child *= 10;
            if (child > INT_MAX - d)
                overflow = 1;
            else
                child += d;
	    NEXT;
	}
        if (overflow)
            child = 0;
	xmlXPtrGetChildNo(ctxt, child);
    }
}


/**
 *  XPointer ::= Name
 *             | ChildSeq
 *             | FullXPtr
 *
 * Parse and evaluate an XPointer
 *
 * @param ctxt  the XPointer Parser context
 */
static void
xmlXPtrEvalXPointer(xmlXPathParserContextPtr ctxt) {
    if (ctxt->valueTab == NULL) {
	/* Allocate the value stack */
	ctxt->valueTab = (xmlXPathObjectPtr *)
			 xmlMalloc(10 * sizeof(xmlXPathObjectPtr));
	if (ctxt->valueTab == NULL) {
	    xmlXPathPErrMemory(ctxt);
	    return;
	}
	ctxt->valueNr = 0;
	ctxt->valueMax = 10;
	ctxt->value = NULL;
    }
    SKIP_BLANKS;
    if (CUR == '/') {
	xmlXPathRoot(ctxt);
        xmlXPtrEvalChildSeq(ctxt, NULL);
    } else {
	xmlChar *name;

	name = xmlXPathParseName(ctxt);
	if (name == NULL)
	    XP_ERROR(XPATH_EXPR_ERROR);
	if (CUR == '(') {
	    xmlXPtrEvalFullXPtr(ctxt, name);
	    /* Short evaluation */
	    return;
	} else {
	    /* this handle both Bare Names and Child Sequences */
	    xmlXPtrEvalChildSeq(ctxt, name);
	}
    }
    SKIP_BLANKS;
    if (CUR != 0)
	XP_ERROR(XPATH_EXPR_ERROR);
}


/************************************************************************
 *									*
 *			General routines				*
 *									*
 ************************************************************************/

/**
 * Create a new XPointer context
 *
 * @deprecated Same as xmlXPathNewContext.
 *
 * @param doc  the XML document
 * @param here  unused
 * @param origin  unused
 * @returns the xmlXPathContext just allocated.
 */
xmlXPathContext *
xmlXPtrNewContext(xmlDoc *doc, xmlNode *here, xmlNode *origin) {
    xmlXPathContextPtr ret;
    (void) here;
    (void) origin;

    ret = xmlXPathNewContext(doc);
    if (ret == NULL)
	return(ret);

    return(ret);
}

/**
 * Evaluate an XPointer expression.
 *
 * This function can only return nodesets. The caller has to
 * free the object.
 *
 * @param str  an XPointer expression
 * @param ctx  an XPath context
 * @returns the xmlXPathObject resulting from the evaluation or NULL
 * in case of error.
 */
xmlXPathObject *
xmlXPtrEval(const xmlChar *str, xmlXPathContext *ctx) {
    xmlXPathParserContextPtr ctxt;
    xmlXPathObjectPtr res = NULL, tmp;
    xmlXPathObjectPtr init = NULL;
    int stack = 0;

    xmlInitParser();

    if ((ctx == NULL) || (str == NULL))
	return(NULL);

    xmlResetError(&ctx->lastError);

    ctxt = xmlXPathNewParserContext(str, ctx);
    if (ctxt == NULL) {
        xmlXPathErrMemory(ctx);
	return(NULL);
    }
    xmlXPtrEvalXPointer(ctxt);
    if (ctx->lastError.code != XML_ERR_OK)
        goto error;

    if ((ctxt->value != NULL) &&
	(ctxt->value->type != XPATH_NODESET)) {
        xmlXPtrErr(ctxt, XML_XPTR_EVAL_FAILED,
		"xmlXPtrEval: evaluation failed to return a node set\n",
		   NULL);
    } else {
	res = xmlXPathValuePop(ctxt);
    }

    do {
        tmp = xmlXPathValuePop(ctxt);
	if (tmp != NULL) {
	    if (tmp != init) {
		if (tmp->type == XPATH_NODESET) {
		    /*
		     * Evaluation may push a root nodeset which is unused
		     */
		    xmlNodeSetPtr set;
		    set = tmp->nodesetval;
		    if ((set == NULL) || (set->nodeNr != 1) ||
			(set->nodeTab[0] != (xmlNodePtr) ctx->doc))
			stack++;
		} else
		    stack++;
	    }
	    xmlXPathFreeObject(tmp);
        }
    } while (tmp != NULL);
    if (stack != 0) {
        xmlXPtrErr(ctxt, XML_XPTR_EXTRA_OBJECTS,
		   "xmlXPtrEval: object(s) left on the eval stack\n",
		   NULL);
    }
    if (ctx->lastError.code != XML_ERR_OK) {
	xmlXPathFreeObject(res);
	res = NULL;
    }

error:
    xmlXPathFreeParserContext(ctxt);
    return(res);
}

#endif

