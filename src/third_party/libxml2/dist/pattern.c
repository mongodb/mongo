/*
 * pattern.c: Implementation of selectors for nodes
 *
 * Reference:
 *   http://www.w3.org/TR/2001/REC-xmlschema-1-20010502/
 *   to some extent
 *   http://www.w3.org/TR/1999/REC-xml-19991116
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

/*
 * TODO:
 * - compilation flags to check for specific syntaxes
 *   using flags of #xmlPatterncompile
 * - making clear how pattern starting with / or . need to be handled,
 *   currently push(NULL, NULL) means a reset of the streaming context
 *   and indicating we are on / (the document node), probably need
 *   something similar for .
 * - get rid of the "compile" starting with lowercase
 * - DONE (2006-05-16): get rid of the Strdup/Strndup in case of dictionary
 */

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <libxml/pattern.h>
#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/dict.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>

#include "private/memory.h"
#include "private/parser.h"

#ifdef LIBXML_PATTERN_ENABLED

#ifdef ERROR
#undef ERROR
#endif
#define ERROR(a, b, c, d)
#define ERROR5(a, b, c, d, e)

#define XML_STREAM_STEP_DESC	1
#define XML_STREAM_STEP_FINAL	2
#define XML_STREAM_STEP_ROOT	4
#define XML_STREAM_STEP_ATTR	8
#define XML_STREAM_STEP_NODE	16
#define XML_STREAM_STEP_IN_SET	32

/*
* NOTE: Those private flags (XML_STREAM_xxx) are used
*   in _xmlStreamCtxt->flag. They extend the public
*   xmlPatternFlags, so be careful not to interfere with the
*   reserved values for xmlPatternFlags.
*/
#define XML_STREAM_FINAL_IS_ANY_NODE 1<<14
#define XML_STREAM_FROM_ROOT 1<<15
#define XML_STREAM_DESC 1<<16

/*
* XML_STREAM_ANY_NODE is used for comparison against
* xmlElementType enums, to indicate a node of any type.
*/
#define XML_STREAM_ANY_NODE 100

#define XML_PATTERN_NOTPATTERN  (XML_PATTERN_XPATH | \
				 XML_PATTERN_XSSEL | \
				 XML_PATTERN_XSFIELD)

#define XML_STREAM_XS_IDC(c) ((c)->flags & \
    (XML_PATTERN_XSSEL | XML_PATTERN_XSFIELD))

#define XML_STREAM_XS_IDC_SEL(c) ((c)->flags & XML_PATTERN_XSSEL)

#define XML_STREAM_XS_IDC_FIELD(c) ((c)->flags & XML_PATTERN_XSFIELD)

#define XML_PAT_COPY_NSNAME(c, r, nsname) \
    if ((c)->comp->dict) \
	r = (xmlChar *) xmlDictLookup((c)->comp->dict, BAD_CAST nsname, -1); \
    else r = xmlStrdup(BAD_CAST nsname);

#define XML_PAT_FREE_STRING(c, r) if ((c)->comp->dict == NULL) xmlFree(r);

typedef struct _xmlStreamStep xmlStreamStep;
typedef xmlStreamStep *xmlStreamStepPtr;
struct _xmlStreamStep {
    int flags;			/* properties of that step */
    const xmlChar *name;	/* first string value if NULL accept all */
    const xmlChar *ns;		/* second string value */
    int nodeType;		/* type of node */
};

typedef struct _xmlStreamComp xmlStreamComp;
typedef xmlStreamComp *xmlStreamCompPtr;
struct _xmlStreamComp {
    xmlDict *dict;		/* the dictionary if any */
    int nbStep;			/* number of steps in the automata */
    int maxStep;		/* allocated number of steps */
    xmlStreamStepPtr steps;	/* the array of steps */
    int flags;
};

struct _xmlStreamCtxt {
    struct _xmlStreamCtxt *next;/* link to next sub pattern if | */
    xmlStreamCompPtr comp;	/* the compiled stream */
    int nbState;		/* number of states in the automata */
    int maxState;		/* allocated number of states */
    int level;			/* how deep are we ? */
    int *states;		/* the array of step indexes */
    int flags;			/* validation options */
    int blockLevel;
};

static void xmlFreeStreamComp(xmlStreamCompPtr comp);

/*
 * Types are private:
 */

typedef enum {
    XML_OP_END=0,
    XML_OP_ROOT,
    XML_OP_ELEM,
    XML_OP_CHILD,
    XML_OP_ATTR,
    XML_OP_PARENT,
    XML_OP_ANCESTOR,
    XML_OP_NS,
    XML_OP_ALL
} xmlPatOp;


typedef struct _xmlStepState xmlStepState;
typedef xmlStepState *xmlStepStatePtr;
struct _xmlStepState {
    int step;
    xmlNodePtr node;
};

typedef struct _xmlStepStates xmlStepStates;
typedef xmlStepStates *xmlStepStatesPtr;
struct _xmlStepStates {
    int nbstates;
    int maxstates;
    xmlStepStatePtr states;
};

typedef struct _xmlStepOp xmlStepOp;
typedef xmlStepOp *xmlStepOpPtr;
struct _xmlStepOp {
    xmlPatOp op;
    const xmlChar *value;
    const xmlChar *value2; /* The namespace name */
};

#define PAT_FROM_ROOT	(1<<8)
#define PAT_FROM_CUR	(1<<9)

struct _xmlPattern {
    void *data;		/* the associated template */
    xmlDictPtr dict;		/* the optional dictionary */
    struct _xmlPattern *next;	/* next pattern if | is used */
    const xmlChar *pattern;	/* the pattern */
    int flags;			/* flags */
    int nbStep;
    int maxStep;
    xmlStepOpPtr steps;        /* ops for computation */
    xmlStreamCompPtr stream;	/* the streaming data if any */
};

typedef struct _xmlPatParserContext xmlPatParserContext;
typedef xmlPatParserContext *xmlPatParserContextPtr;
struct _xmlPatParserContext {
    const xmlChar *cur;			/* the current char being parsed */
    const xmlChar *base;		/* the full expression */
    int	           error;		/* error code */
    xmlDictPtr     dict;		/* the dictionary if any */
    xmlPatternPtr  comp;		/* the result */
    xmlNodePtr     elem;		/* the current node if any */
    const xmlChar **namespaces;		/* the namespaces definitions */
    int   nb_namespaces;		/* the number of namespaces */
};

/************************************************************************
 *									*
 *			Type functions					*
 *									*
 ************************************************************************/

/**
 * Create a new XSLT Pattern
 *
 * @returns the newly allocated xmlPattern or NULL in case of error
 */
static xmlPatternPtr
xmlNewPattern(void) {
    xmlPatternPtr cur;

    cur = (xmlPatternPtr) xmlMalloc(sizeof(xmlPattern));
    if (cur == NULL) {
	ERROR(NULL, NULL, NULL,
		"xmlNewPattern : malloc failed\n");
	return(NULL);
    }
    memset(cur, 0, sizeof(xmlPattern));
    cur->steps = NULL;
    cur->maxStep = 0;
    return(cur);
}

/**
 * Free up the memory allocated by `comp`
 *
 * @param comp  an XSLT comp
 */
void
xmlFreePattern(xmlPattern *comp) {
    xmlFreePatternList(comp);
}

static void
xmlFreePatternInternal(xmlPatternPtr comp) {
    xmlStepOpPtr op;
    int i;

    if (comp == NULL)
	return;
    if (comp->stream != NULL)
        xmlFreeStreamComp(comp->stream);
    if (comp->pattern != NULL)
	xmlFree((xmlChar *)comp->pattern);
    if (comp->steps != NULL) {
        if (comp->dict == NULL) {
	    for (i = 0;i < comp->nbStep;i++) {
		op = &comp->steps[i];
		if (op->value != NULL)
		    xmlFree((xmlChar *) op->value);
		if (op->value2 != NULL)
		    xmlFree((xmlChar *) op->value2);
	    }
	}
	xmlFree(comp->steps);
    }
    if (comp->dict != NULL)
        xmlDictFree(comp->dict);

    memset(comp, -1, sizeof(xmlPattern));
    xmlFree(comp);
}

/**
 * Free up the memory allocated by all the elements of `comp`
 *
 * @param comp  an XSLT comp list
 */
void
xmlFreePatternList(xmlPattern *comp) {
    xmlPatternPtr cur;

    while (comp != NULL) {
	cur = comp;
	comp = comp->next;
	cur->next = NULL;
	xmlFreePatternInternal(cur);
    }
}

/**
 * Create a new XML pattern parser context
 *
 * @param pattern  the pattern context
 * @param dict  the inherited dictionary or NULL
 * @param namespaces  the prefix definitions, array of [URI, prefix] terminated
 *              with [NULL, NULL] or NULL if no namespace is used
 * @returns the newly allocated xmlPatParserContext or NULL in case of error
 */
static xmlPatParserContextPtr
xmlNewPatParserContext(const xmlChar *pattern, xmlDictPtr dict,
                       const xmlChar **namespaces) {
    xmlPatParserContextPtr cur;

    if (pattern == NULL)
        return(NULL);

    cur = (xmlPatParserContextPtr) xmlMalloc(sizeof(xmlPatParserContext));
    if (cur == NULL) {
	ERROR(NULL, NULL, NULL,
		"xmlNewPatParserContext : malloc failed\n");
	return(NULL);
    }
    memset(cur, 0, sizeof(xmlPatParserContext));
    cur->dict = dict;
    cur->cur = pattern;
    cur->base = pattern;
    if (namespaces != NULL) {
        int i;
        for (i = 0;namespaces[2 * i] != NULL;i++)
            ;
        cur->nb_namespaces = i;
    } else {
        cur->nb_namespaces = 0;
    }
    cur->namespaces = namespaces;
    return(cur);
}

/**
 * Free up the memory allocated by `ctxt`
 *
 * @param ctxt  an XSLT parser context
 */
static void
xmlFreePatParserContext(xmlPatParserContextPtr ctxt) {
    if (ctxt == NULL)
	return;
    memset(ctxt, -1, sizeof(xmlPatParserContext));
    xmlFree(ctxt);
}

static int
xmlPatternGrow(xmlPatternPtr comp) {
    xmlStepOpPtr temp;
    int newSize;

    newSize = xmlGrowCapacity(comp->maxStep, sizeof(temp[0]),
                              10, XML_MAX_ITEMS);
    if (newSize < 0)
        return(-1);
    temp = xmlRealloc(comp->steps, newSize * sizeof(temp[0]));
    if (temp == NULL)
        return(-1);
    comp->steps = temp;
    comp->maxStep = newSize;

    return(0);
}

/**
 * Add a step to an XSLT Compiled Match
 *
 * @param ctxt  the pattern parser context
 * @param comp  the compiled match expression
 * @param op  an op
 * @param value  the first value
 * @param value2  the second value
 * @returns -1 in case of failure, 0 otherwise.
 */
static int
xmlPatternAdd(xmlPatParserContextPtr ctxt, xmlPatternPtr comp,
              xmlPatOp op, xmlChar * value, xmlChar * value2)
{
    if (comp->nbStep >= comp->maxStep) {
        if (xmlPatternGrow(comp) < 0) {
            ctxt->error = -1;
            return(-1);
        }
    }
    comp->steps[comp->nbStep].op = op;
    comp->steps[comp->nbStep].value = value;
    comp->steps[comp->nbStep].value2 = value2;
    comp->nbStep++;
    return(0);
}

/**
 * reverse all the stack of expressions
 *
 * @param comp  the compiled match expression
 * @returns 0 in case of success and -1 in case of error.
 */
static int
xmlReversePattern(xmlPatternPtr comp) {
    int i, j;

    /*
     * remove the leading // for //a or .//a
     */
    if ((comp->nbStep > 0) && (comp->steps[0].op == XML_OP_ANCESTOR)) {
        for (i = 0, j = 1;j < comp->nbStep;i++,j++) {
	    comp->steps[i].value = comp->steps[j].value;
	    comp->steps[i].value2 = comp->steps[j].value2;
	    comp->steps[i].op = comp->steps[j].op;
	}
	comp->nbStep--;
    }

    /*
     * Grow to add OP_END later
     */
    if (comp->nbStep >= comp->maxStep) {
        if (xmlPatternGrow(comp) < 0)
            return(-1);
    }

    i = 0;
    j = comp->nbStep - 1;
    while (j > i) {
	register const xmlChar *tmp;
	register xmlPatOp op;
	tmp = comp->steps[i].value;
	comp->steps[i].value = comp->steps[j].value;
	comp->steps[j].value = tmp;
	tmp = comp->steps[i].value2;
	comp->steps[i].value2 = comp->steps[j].value2;
	comp->steps[j].value2 = tmp;
	op = comp->steps[i].op;
	comp->steps[i].op = comp->steps[j].op;
	comp->steps[j].op = op;
	j--;
	i++;
    }

    comp->steps[comp->nbStep].value = NULL;
    comp->steps[comp->nbStep].value2 = NULL;
    comp->steps[comp->nbStep++].op = XML_OP_END;
    return(0);
}

/************************************************************************
 *									*
 *		The interpreter for the precompiled patterns		*
 *									*
 ************************************************************************/

static int
xmlPatPushState(xmlStepStates *states, int step, xmlNodePtr node) {
    if (states->maxstates <= states->nbstates) {
        xmlStepState *tmp;
        int newSize;

        newSize = xmlGrowCapacity(states->maxstates, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
        if (newSize < 0)
	    return(-1);
	tmp = xmlRealloc(states->states, newSize * sizeof(tmp[0]));
	if (tmp == NULL)
	    return(-1);
	states->states = tmp;
	states->maxstates = newSize;
    }
    states->states[states->nbstates].step = step;
    states->states[states->nbstates++].node = node;
    return(0);
}

/**
 * Test whether the node matches the pattern
 *
 * @param comp  the precompiled pattern
 * @param node  a node
 * @returns 1 if it matches, 0 if it doesn't and -1 in case of failure
 */
static int
xmlPatMatch(xmlPatternPtr comp, xmlNodePtr node) {
    int i;
    xmlStepOpPtr step;
    xmlStepStates states = {0, 0, NULL}; /* // may require backtrack */

    if ((comp == NULL) || (node == NULL)) return(-1);
    i = 0;
restart:
    for (;i < comp->nbStep;i++) {
	step = &comp->steps[i];
	switch (step->op) {
            case XML_OP_END:
		goto found;
            case XML_OP_ROOT:
		if (node->type == XML_NAMESPACE_DECL)
		    goto rollback;
		node = node->parent;
		if ((node->type == XML_DOCUMENT_NODE) ||
		    (node->type == XML_HTML_DOCUMENT_NODE))
		    continue;
		goto rollback;
            case XML_OP_ELEM:
		if (node->type != XML_ELEMENT_NODE)
		    goto rollback;
		if (step->value == NULL)
		    continue;
		if (step->value[0] != node->name[0])
		    goto rollback;
		if (!xmlStrEqual(step->value, node->name))
		    goto rollback;

		/* Namespace test */
		if (node->ns == NULL) {
		    if (step->value2 != NULL)
			goto rollback;
		} else if (node->ns->href != NULL) {
		    if (step->value2 == NULL)
			goto rollback;
		    if (!xmlStrEqual(step->value2, node->ns->href))
			goto rollback;
		}
		continue;
            case XML_OP_CHILD: {
		xmlNodePtr lst;

		if ((node->type != XML_ELEMENT_NODE) &&
		    (node->type != XML_DOCUMENT_NODE) &&
		    (node->type != XML_HTML_DOCUMENT_NODE))
		    goto rollback;

		lst = node->children;

		if (step->value != NULL) {
		    while (lst != NULL) {
			if ((lst->type == XML_ELEMENT_NODE) &&
			    (step->value[0] == lst->name[0]) &&
			    (xmlStrEqual(step->value, lst->name)))
			    break;
			lst = lst->next;
		    }
		    if (lst != NULL)
			continue;
		}
		goto rollback;
	    }
            case XML_OP_ATTR:
		if (node->type != XML_ATTRIBUTE_NODE)
		    goto rollback;
		if (step->value != NULL) {
		    if (step->value[0] != node->name[0])
			goto rollback;
		    if (!xmlStrEqual(step->value, node->name))
			goto rollback;
		}
		/* Namespace test */
		if (node->ns == NULL) {
		    if (step->value2 != NULL)
			goto rollback;
		} else if (step->value2 != NULL) {
		    if (!xmlStrEqual(step->value2, node->ns->href))
			goto rollback;
		}
		continue;
            case XML_OP_PARENT:
		if ((node->type == XML_DOCUMENT_NODE) ||
		    (node->type == XML_HTML_DOCUMENT_NODE) ||
		    (node->type == XML_NAMESPACE_DECL))
		    goto rollback;
		node = node->parent;
		if (node == NULL)
		    goto rollback;
		if (step->value == NULL)
		    continue;
		if (step->value[0] != node->name[0])
		    goto rollback;
		if (!xmlStrEqual(step->value, node->name))
		    goto rollback;
		/* Namespace test */
		if (node->ns == NULL) {
		    if (step->value2 != NULL)
			goto rollback;
		} else if (node->ns->href != NULL) {
		    if (step->value2 == NULL)
			goto rollback;
		    if (!xmlStrEqual(step->value2, node->ns->href))
			goto rollback;
		}
		continue;
            case XML_OP_ANCESTOR:
		/* TODO: implement coalescing of ANCESTOR/NODE ops */
		if (step->value == NULL) {
		    i++;
		    step = &comp->steps[i];
		    if (step->op == XML_OP_ROOT)
			goto found;
		    if (step->op != XML_OP_ELEM)
			goto rollback;
		    if (step->value == NULL)
			return(-1);
		}
		if (node == NULL)
		    goto rollback;
		if ((node->type == XML_DOCUMENT_NODE) ||
		    (node->type == XML_HTML_DOCUMENT_NODE) ||
		    (node->type == XML_NAMESPACE_DECL))
		    goto rollback;
		node = node->parent;
		while (node != NULL) {
		    if ((node->type == XML_ELEMENT_NODE) &&
			(step->value[0] == node->name[0]) &&
			(xmlStrEqual(step->value, node->name))) {
			/* Namespace test */
			if (node->ns == NULL) {
			    if (step->value2 == NULL)
				break;
			} else if (node->ns->href != NULL) {
			    if ((step->value2 != NULL) &&
			        (xmlStrEqual(step->value2, node->ns->href)))
				break;
			}
		    }
		    node = node->parent;
		}
		if (node == NULL)
		    goto rollback;
		/*
		 * prepare a potential rollback from here
		 * for ancestors of that node.
		 */
		if (step->op == XML_OP_ANCESTOR)
		    xmlPatPushState(&states, i, node);
		else
		    xmlPatPushState(&states, i - 1, node);
		continue;
            case XML_OP_NS:
		if (node->type != XML_ELEMENT_NODE)
		    goto rollback;
		if (node->ns == NULL) {
		    if (step->value != NULL)
			goto rollback;
		} else if (node->ns->href != NULL) {
		    if (step->value == NULL)
			goto rollback;
		    if (!xmlStrEqual(step->value, node->ns->href))
			goto rollback;
		}
		break;
            case XML_OP_ALL:
		if (node->type != XML_ELEMENT_NODE)
		    goto rollback;
		break;
	}
    }
found:
    if (states.states != NULL) {
        /* Free the rollback states */
	xmlFree(states.states);
    }
    return(1);
rollback:
    /* got an error try to rollback */
    if (states.states == NULL)
	return(0);
    if (states.nbstates <= 0) {
	xmlFree(states.states);
	return(0);
    }
    states.nbstates--;
    i = states.states[states.nbstates].step;
    node = states.states[states.nbstates].node;
    goto restart;
}

/************************************************************************
 *									*
 *			Dedicated parser for templates			*
 *									*
 ************************************************************************/

#define CUR (*ctxt->cur)
#define SKIP(val) ctxt->cur += (val)
#define NXT(val) ctxt->cur[(val)]
#define PEEKPREV(val) ctxt->cur[-(val)]
#define CUR_PTR ctxt->cur

#define SKIP_BLANKS							\
    while (IS_BLANK_CH(CUR)) NEXT

#define CURRENT (*ctxt->cur)
#define NEXT ((*ctxt->cur) ?  ctxt->cur++: ctxt->cur)


#define PUSH(op, val, val2)						\
    if (xmlPatternAdd(ctxt, ctxt->comp, (op), (val), (val2))) goto error;

/**
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (S Name)*
 *
 * @param ctxt  the XPath Parser context
 * @returns the Name parsed or NULL
 */

static xmlChar *
xmlPatScanName(xmlPatParserContextPtr ctxt) {
    const xmlChar *q, *cur;
    xmlChar *ret = NULL;

    SKIP_BLANKS;

    q = CUR_PTR;
    cur = xmlScanName(q, XML_MAX_NAME_LENGTH, 0);
    if ((cur == NULL) || (cur == q))
        return(NULL);
    if (ctxt->dict)
	ret = (xmlChar *) xmlDictLookup(ctxt->dict, q, cur - q);
    else
	ret = xmlStrndup(q, cur - q);
    CUR_PTR = cur;
    return(ret);
}

/**
 * Parses a non qualified name
 *
 * @param ctxt  the XPath Parser context
 * @returns the Name parsed or NULL
 */

static xmlChar *
xmlPatScanNCName(xmlPatParserContextPtr ctxt) {
    const xmlChar *q, *cur;
    xmlChar *ret = NULL;

    SKIP_BLANKS;

    q = CUR_PTR;
    cur = xmlScanName(q, XML_MAX_NAME_LENGTH, XML_SCAN_NC);
    if ((cur == NULL) || (cur == q))
        return(NULL);
    if (ctxt->dict)
	ret = (xmlChar *) xmlDictLookup(ctxt->dict, q, cur - q);
    else
	ret = xmlStrndup(q, cur - q);
    if (ret == NULL)
        ctxt->error = -1;
    CUR_PTR = cur;
    return(ret);
}

/**
 * Compile an attribute test.
 *
 * @param ctxt  the compilation context
 */
static void
xmlCompileAttributeTest(xmlPatParserContextPtr ctxt) {
    xmlChar *token = NULL;
    xmlChar *name = NULL;
    xmlChar *URL = NULL;

    SKIP_BLANKS;
    name = xmlPatScanNCName(ctxt);
    if (ctxt->error < 0)
        return;
    if (name == NULL) {
	if (CUR == '*') {
	    PUSH(XML_OP_ATTR, NULL, NULL);
	    NEXT;
	} else {
	    ERROR(NULL, NULL, NULL,
		"xmlCompileAttributeTest : Name expected\n");
	    ctxt->error = 1;
	}
	return;
    }
    if (CUR == ':') {
	int i;
	xmlChar *prefix = name;

	NEXT;

	if (IS_BLANK_CH(CUR)) {
	    ERROR5(NULL, NULL, NULL, "Invalid QName.\n", NULL);
	    ctxt->error = 1;
	    goto error;
	}
	/*
	* This is a namespace match
	*/
	token = xmlPatScanName(ctxt);
	if ((prefix[0] == 'x') &&
	    (prefix[1] == 'm') &&
	    (prefix[2] == 'l') &&
	    (prefix[3] == 0))
	{
	    XML_PAT_COPY_NSNAME(ctxt, URL, XML_XML_NAMESPACE);
	} else {
	    for (i = 0;i < ctxt->nb_namespaces;i++) {
		if (xmlStrEqual(ctxt->namespaces[2 * i + 1], prefix)) {
		    XML_PAT_COPY_NSNAME(ctxt, URL, ctxt->namespaces[2 * i])
		    break;
		}
	    }
	    if (i >= ctxt->nb_namespaces) {
		ERROR5(NULL, NULL, NULL,
		    "xmlCompileAttributeTest : no namespace bound to prefix %s\n",
		    prefix);
		ctxt->error = 1;
		goto error;
	    }
	}
        XML_PAT_FREE_STRING(ctxt, name);
        name = NULL;
	if (token == NULL) {
	    if (CUR == '*') {
		NEXT;
		PUSH(XML_OP_ATTR, NULL, URL);
	    } else {
		ERROR(NULL, NULL, NULL,
		    "xmlCompileAttributeTest : Name expected\n");
		ctxt->error = 1;
		goto error;
	    }
	} else {
	    PUSH(XML_OP_ATTR, token, URL);
	}
    } else {
	PUSH(XML_OP_ATTR, name, NULL);
    }
    return;
error:
    if (name != NULL)
	XML_PAT_FREE_STRING(ctxt, name);
    if (URL != NULL)
	XML_PAT_FREE_STRING(ctxt, URL)
    if (token != NULL)
	XML_PAT_FREE_STRING(ctxt, token);
}

/**
 * Compile the Step Pattern and generates a precompiled
 * form suitable for fast matching.
 *
 * [3]    Step    ::=    '.' | NameTest
 * [4]    NameTest    ::=    QName | '*' | NCName ':' '*'
 *
 * @param ctxt  the compilation context
 */

static void
xmlCompileStepPattern(xmlPatParserContextPtr ctxt) {
    xmlChar *token = NULL;
    xmlChar *name = NULL;
    xmlChar *URL = NULL;
    int hasBlanks = 0;

    SKIP_BLANKS;
    if (CUR == '.') {
	/*
	* Context node.
	*/
	NEXT;
	PUSH(XML_OP_ELEM, NULL, NULL);
	return;
    }
    if (CUR == '@') {
	/*
	* Attribute test.
	*/
	if (XML_STREAM_XS_IDC_SEL(ctxt->comp)) {
	    ERROR5(NULL, NULL, NULL,
		"Unexpected attribute axis in '%s'.\n", ctxt->base);
	    ctxt->error = 1;
	    return;
	}
	NEXT;
	xmlCompileAttributeTest(ctxt);
	if (ctxt->error != 0)
	    goto error;
	return;
    }
    name = xmlPatScanNCName(ctxt);
    if (ctxt->error < 0)
        return;
    if (name == NULL) {
	if (CUR == '*') {
	    NEXT;
	    PUSH(XML_OP_ALL, NULL, NULL);
	    return;
	} else {
	    ERROR(NULL, NULL, NULL,
		    "xmlCompileStepPattern : Name expected\n");
	    ctxt->error = 1;
	    return;
	}
    }
    if (IS_BLANK_CH(CUR)) {
	hasBlanks = 1;
	SKIP_BLANKS;
    }
    if (CUR == ':') {
	NEXT;
	if (CUR != ':') {
	    xmlChar *prefix = name;
	    int i;

	    if (hasBlanks || IS_BLANK_CH(CUR)) {
		ERROR5(NULL, NULL, NULL, "Invalid QName.\n", NULL);
		ctxt->error = 1;
		goto error;
	    }
	    /*
	     * This is a namespace match
	     */
	    token = xmlPatScanName(ctxt);
	    if ((prefix[0] == 'x') &&
		(prefix[1] == 'm') &&
		(prefix[2] == 'l') &&
		(prefix[3] == 0))
	    {
		XML_PAT_COPY_NSNAME(ctxt, URL, XML_XML_NAMESPACE)
	    } else {
		for (i = 0;i < ctxt->nb_namespaces;i++) {
		    if (xmlStrEqual(ctxt->namespaces[2 * i + 1], prefix)) {
			XML_PAT_COPY_NSNAME(ctxt, URL, ctxt->namespaces[2 * i])
			break;
		    }
		}
		if (i >= ctxt->nb_namespaces) {
		    ERROR5(NULL, NULL, NULL,
			"xmlCompileStepPattern : no namespace bound to prefix %s\n",
			prefix);
		    ctxt->error = 1;
		    goto error;
		}
	    }
	    XML_PAT_FREE_STRING(ctxt, prefix);
	    name = NULL;
	    if (token == NULL) {
		if (CUR == '*') {
		    NEXT;
		    PUSH(XML_OP_NS, URL, NULL);
		} else {
		    ERROR(NULL, NULL, NULL,
			    "xmlCompileStepPattern : Name expected\n");
		    ctxt->error = 1;
		    goto error;
		}
	    } else {
		PUSH(XML_OP_ELEM, token, URL);
	    }
	} else {
	    NEXT;
	    if (xmlStrEqual(name, (const xmlChar *) "child")) {
		XML_PAT_FREE_STRING(ctxt, name);
		name = xmlPatScanName(ctxt);
		if (name == NULL) {
		    if (CUR == '*') {
			NEXT;
			PUSH(XML_OP_ALL, NULL, NULL);
			return;
		    } else {
			ERROR(NULL, NULL, NULL,
			    "xmlCompileStepPattern : QName expected\n");
			ctxt->error = 1;
			goto error;
		    }
		}
		if (CUR == ':') {
		    xmlChar *prefix = name;
		    int i;

		    NEXT;
		    if (IS_BLANK_CH(CUR)) {
			ERROR5(NULL, NULL, NULL, "Invalid QName.\n", NULL);
			ctxt->error = 1;
			goto error;
		    }
		    /*
		    * This is a namespace match
		    */
		    token = xmlPatScanName(ctxt);
		    if ((prefix[0] == 'x') &&
			(prefix[1] == 'm') &&
			(prefix[2] == 'l') &&
			(prefix[3] == 0))
		    {
			XML_PAT_COPY_NSNAME(ctxt, URL, XML_XML_NAMESPACE)
		    } else {
			for (i = 0;i < ctxt->nb_namespaces;i++) {
			    if (xmlStrEqual(ctxt->namespaces[2 * i + 1], prefix)) {
				XML_PAT_COPY_NSNAME(ctxt, URL, ctxt->namespaces[2 * i])
				break;
			    }
			}
			if (i >= ctxt->nb_namespaces) {
			    ERROR5(NULL, NULL, NULL,
				"xmlCompileStepPattern : no namespace bound "
				"to prefix %s\n", prefix);
			    ctxt->error = 1;
			    goto error;
			}
		    }
		    XML_PAT_FREE_STRING(ctxt, prefix);
		    name = NULL;
		    if (token == NULL) {
			if (CUR == '*') {
			    NEXT;
			    PUSH(XML_OP_NS, URL, NULL);
			} else {
			    ERROR(NULL, NULL, NULL,
				"xmlCompileStepPattern : Name expected\n");
			    ctxt->error = 1;
			    goto error;
			}
		    } else {
			PUSH(XML_OP_ELEM, token, URL);
		    }
		} else
		    PUSH(XML_OP_ELEM, name, NULL);
		return;
	    } else if (xmlStrEqual(name, (const xmlChar *) "attribute")) {
		XML_PAT_FREE_STRING(ctxt, name)
		name = NULL;
		if (XML_STREAM_XS_IDC_SEL(ctxt->comp)) {
		    ERROR5(NULL, NULL, NULL,
			"Unexpected attribute axis in '%s'.\n", ctxt->base);
		    ctxt->error = 1;
		    goto error;
		}
		xmlCompileAttributeTest(ctxt);
		if (ctxt->error != 0)
		    goto error;
		return;
	    } else {
		ERROR5(NULL, NULL, NULL,
		    "The 'element' or 'attribute' axis is expected.\n", NULL);
		ctxt->error = 1;
		goto error;
	    }
	}
    } else if (CUR == '*') {
        if (name != NULL) {
	    ctxt->error = 1;
	    goto error;
	}
	NEXT;
	PUSH(XML_OP_ALL, token, NULL);
    } else {
	PUSH(XML_OP_ELEM, name, NULL);
    }
    return;
error:
    if (URL != NULL)
	XML_PAT_FREE_STRING(ctxt, URL)
    if (token != NULL)
	XML_PAT_FREE_STRING(ctxt, token)
    if (name != NULL)
	XML_PAT_FREE_STRING(ctxt, name)
}

/**
 * Compile the Path Pattern and generates a precompiled
 * form suitable for fast matching.
 *
 * [5]    Path    ::=    ('.//')? ( Step '/' )* ( Step | '@' NameTest )
 *
 * @param ctxt  the compilation context
 */
static void
xmlCompilePathPattern(xmlPatParserContextPtr ctxt) {
    SKIP_BLANKS;
    if (CUR == '/') {
        ctxt->comp->flags |= PAT_FROM_ROOT;
    } else if ((CUR == '.') || (ctxt->comp->flags & XML_PATTERN_NOTPATTERN)) {
        ctxt->comp->flags |= PAT_FROM_CUR;
    }

    if ((CUR == '/') && (NXT(1) == '/')) {
	PUSH(XML_OP_ANCESTOR, NULL, NULL);
	NEXT;
	NEXT;
    } else if ((CUR == '.') && (NXT(1) == '/') && (NXT(2) == '/')) {
	PUSH(XML_OP_ANCESTOR, NULL, NULL);
	NEXT;
	NEXT;
	NEXT;
	/* Check for incompleteness. */
	SKIP_BLANKS;
	if (CUR == 0) {
	    ERROR5(NULL, NULL, NULL,
	       "Incomplete expression '%s'.\n", ctxt->base);
	    ctxt->error = 1;
	    goto error;
	}
    }
    if (CUR == '@') {
	NEXT;
	xmlCompileAttributeTest(ctxt);
        if (ctxt->error != 0)
            goto error;
	SKIP_BLANKS;
	/* TODO: check for incompleteness */
	if (CUR != 0) {
	    xmlCompileStepPattern(ctxt);
	    if (ctxt->error != 0)
		goto error;
	}
    } else {
        if (CUR == '/') {
	    PUSH(XML_OP_ROOT, NULL, NULL);
	    NEXT;
	    /* Check for incompleteness. */
	    SKIP_BLANKS;
	    if (CUR == 0) {
		ERROR5(NULL, NULL, NULL,
		    "Incomplete expression '%s'.\n", ctxt->base);
		ctxt->error = 1;
		goto error;
	    }
	}
	xmlCompileStepPattern(ctxt);
	if (ctxt->error != 0)
	    goto error;
	SKIP_BLANKS;
	while (CUR == '/') {
	    if (NXT(1) == '/') {
	        PUSH(XML_OP_ANCESTOR, NULL, NULL);
		NEXT;
		NEXT;
		SKIP_BLANKS;
		xmlCompileStepPattern(ctxt);
		if (ctxt->error != 0)
		    goto error;
	    } else {
	        PUSH(XML_OP_PARENT, NULL, NULL);
		NEXT;
		SKIP_BLANKS;
		if (CUR == 0) {
		    ERROR5(NULL, NULL, NULL,
		    "Incomplete expression '%s'.\n", ctxt->base);
		    ctxt->error = 1;
		    goto error;
		}
		xmlCompileStepPattern(ctxt);
		if (ctxt->error != 0)
		    goto error;
	    }
	}
    }
    if (CUR != 0) {
	ERROR5(NULL, NULL, NULL,
	       "Failed to compile pattern %s\n", ctxt->base);
	ctxt->error = 1;
    }
error:
    return;
}

/**
 * Compile the Path Pattern and generates a precompiled
 * form suitable for fast matching.
 *
 * [5]    Path    ::=    ('.//')? ( Step '/' )* ( Step | '@' NameTest )
 *
 * @param ctxt  the compilation context
 */
static void
xmlCompileIDCXPathPath(xmlPatParserContextPtr ctxt) {
    SKIP_BLANKS;
    if (CUR == '/') {
	ERROR5(NULL, NULL, NULL,
	    "Unexpected selection of the document root in '%s'.\n",
	    ctxt->base);
	goto error;
    }
    ctxt->comp->flags |= PAT_FROM_CUR;

    if (CUR == '.') {
	/* "." - "self::node()" */
	NEXT;
	SKIP_BLANKS;
	if (CUR == 0) {
	    /*
	    * Selection of the context node.
	    */
	    PUSH(XML_OP_ELEM, NULL, NULL);
	    return;
	}
	if (CUR != '/') {
	    /* TODO: A more meaningful error message. */
	    ERROR5(NULL, NULL, NULL,
	    "Unexpected token after '.' in '%s'.\n", ctxt->base);
	    goto error;
	}
	/* "./" - "self::node()/" */
	NEXT;
	SKIP_BLANKS;
	if (CUR == '/') {
	    if (IS_BLANK_CH(PEEKPREV(1))) {
		/*
		* Disallow "./ /"
		*/
		ERROR5(NULL, NULL, NULL,
		    "Unexpected '/' token in '%s'.\n", ctxt->base);
		goto error;
	    }
	    /* ".//" - "self:node()/descendant-or-self::node()/" */
	    PUSH(XML_OP_ANCESTOR, NULL, NULL);
	    NEXT;
	    SKIP_BLANKS;
	}
	if (CUR == 0)
	    goto error_unfinished;
    }
    /*
    * Process steps.
    */
    do {
	xmlCompileStepPattern(ctxt);
	if (ctxt->error != 0)
	    goto error;
	SKIP_BLANKS;
	if (CUR != '/')
	    break;
	PUSH(XML_OP_PARENT, NULL, NULL);
	NEXT;
	SKIP_BLANKS;
	if (CUR == '/') {
	    /*
	    * Disallow subsequent '//'.
	    */
	    ERROR5(NULL, NULL, NULL,
		"Unexpected subsequent '//' in '%s'.\n",
		ctxt->base);
	    goto error;
	}
	if (CUR == 0)
	    goto error_unfinished;

    } while (CUR != 0);

    if (CUR != 0) {
	ERROR5(NULL, NULL, NULL,
	    "Failed to compile expression '%s'.\n", ctxt->base);
	ctxt->error = 1;
    }
    return;
error:
    ctxt->error = 1;
    return;

error_unfinished:
    ctxt->error = 1;
    ERROR5(NULL, NULL, NULL,
	"Unfinished expression '%s'.\n", ctxt->base);
}

/************************************************************************
 *									*
 *			The streaming code				*
 *									*
 ************************************************************************/

/**
 * build a new compiled pattern for streaming
 *
 * @param size  the number of expected steps
 * @returns the new structure or NULL in case of error.
 */
static xmlStreamCompPtr
xmlNewStreamComp(int size) {
    xmlStreamCompPtr cur;

    if (size < 4)
        size  = 4;

    cur = (xmlStreamCompPtr) xmlMalloc(sizeof(xmlStreamComp));
    if (cur == NULL) {
	ERROR(NULL, NULL, NULL,
		"xmlNewStreamComp: malloc failed\n");
	return(NULL);
    }
    memset(cur, 0, sizeof(xmlStreamComp));
    cur->steps = (xmlStreamStepPtr) xmlMalloc(size * sizeof(xmlStreamStep));
    if (cur->steps == NULL) {
	xmlFree(cur);
	ERROR(NULL, NULL, NULL,
	      "xmlNewStreamComp: malloc failed\n");
	return(NULL);
    }
    cur->nbStep = 0;
    cur->maxStep = size;
    return(cur);
}

/**
 * Free the compiled pattern for streaming
 *
 * @param comp  the compiled pattern for streaming
 */
static void
xmlFreeStreamComp(xmlStreamCompPtr comp) {
    if (comp != NULL) {
        if (comp->steps != NULL)
	    xmlFree(comp->steps);
	if (comp->dict != NULL)
	    xmlDictFree(comp->dict);
        xmlFree(comp);
    }
}

/**
 * Add a new step to the compiled pattern
 *
 * @param comp  the compiled pattern for streaming
 * @param name  the first string, the name, or NULL for *
 * @param ns  the second step, the namespace name
 * @param nodeType  the node type
 * @param flags  the flags for that step
 * @returns -1 in case of error or the step index if successful
 */
static int
xmlStreamCompAddStep(xmlStreamCompPtr comp, const xmlChar *name,
                     const xmlChar *ns, int nodeType, int flags) {
    xmlStreamStepPtr cur;

    if (comp->nbStep >= comp->maxStep) {
        xmlStreamStepPtr tmp;
        int newSize;

        newSize = xmlGrowCapacity(comp->maxStep, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
        if (newSize < 0) {
	    ERROR(NULL, NULL, NULL,
		  "xmlNewStreamComp: growCapacity failed\n");
	    return(-1);
        }
	cur = xmlRealloc(comp->steps, newSize * sizeof(tmp[0]));
	if (cur == NULL) {
	    ERROR(NULL, NULL, NULL,
		  "xmlNewStreamComp: malloc failed\n");
	    return(-1);
	}
	comp->steps = cur;
        comp->maxStep = newSize;
    }
    cur = &comp->steps[comp->nbStep++];
    cur->flags = flags;
    cur->name = name;
    cur->ns = ns;
    cur->nodeType = nodeType;
    return(comp->nbStep - 1);
}

/**
 * Tries to stream compile a pattern
 *
 * @param comp  the precompiled pattern
 * @returns -1 in case of failure and 0 in case of success.
 */
static int
xmlStreamCompile(xmlPatternPtr comp) {
    xmlStreamCompPtr stream;
    int i, s = 0, root = 0, flags = 0, prevs = -1;
    xmlStepOp step;

    if ((comp == NULL) || (comp->steps == NULL))
        return(-1);
    /*
     * special case for .
     */
    if ((comp->nbStep == 1) &&
        (comp->steps[0].op == XML_OP_ELEM) &&
	(comp->steps[0].value == NULL) &&
	(comp->steps[0].value2 == NULL)) {
	stream = xmlNewStreamComp(0);
	if (stream == NULL)
	    return(-1);
	/* Note that the stream will have no steps in this case. */
	stream->flags |= XML_STREAM_FINAL_IS_ANY_NODE;
	comp->stream = stream;
	return(0);
    }

    stream = xmlNewStreamComp((comp->nbStep / 2) + 1);
    if (stream == NULL)
        return(-1);
    if (comp->dict != NULL) {
        stream->dict = comp->dict;
	xmlDictReference(stream->dict);
    }

    i = 0;
    if (comp->flags & PAT_FROM_ROOT)
	stream->flags |= XML_STREAM_FROM_ROOT;

    for (;i < comp->nbStep;i++) {
	step = comp->steps[i];
        switch (step.op) {
	    case XML_OP_END:
	        break;
	    case XML_OP_ROOT:
	        if (i != 0)
		    goto error;
		root = 1;
		break;
	    case XML_OP_NS:
		s = xmlStreamCompAddStep(stream, NULL, step.value,
		    XML_ELEMENT_NODE, flags);
		if (s < 0)
		    goto error;
		prevs = s;
		flags = 0;
		break;
	    case XML_OP_ATTR:
		flags |= XML_STREAM_STEP_ATTR;
		prevs = -1;
		s = xmlStreamCompAddStep(stream,
		    step.value, step.value2, XML_ATTRIBUTE_NODE, flags);
		flags = 0;
		if (s < 0)
		    goto error;
		break;
	    case XML_OP_ELEM:
	        if ((step.value == NULL) && (step.value2 == NULL)) {
		    /*
		    * We have a "." or "self::node()" here.
		    * Eliminate redundant self::node() tests like in "/./."
		    * or "//./"
		    * The only case we won't eliminate is "//.", i.e. if
		    * self::node() is the last node test and we had
		    * continuation somewhere beforehand.
		    */
		    if ((comp->nbStep == i + 1) &&
			(flags & XML_STREAM_STEP_DESC)) {
			/*
			* Mark the special case where the expression resolves
			* to any type of node.
			*/
			if (comp->nbStep == i + 1) {
			    stream->flags |= XML_STREAM_FINAL_IS_ANY_NODE;
			}
			flags |= XML_STREAM_STEP_NODE;
			s = xmlStreamCompAddStep(stream, NULL, NULL,
			    XML_STREAM_ANY_NODE, flags);
			if (s < 0)
			    goto error;
			flags = 0;
			/*
			* If there was a previous step, mark it to be added to
			* the result node-set; this is needed since only
			* the last step will be marked as "final" and only
			* "final" nodes are added to the resulting set.
			*/
			if (prevs != -1) {
			    stream->steps[prevs].flags |= XML_STREAM_STEP_IN_SET;
			    prevs = -1;
			}
			break;

		    } else {
			/* Just skip this one. */
			continue;
		    }
		}
		/* An element node. */
	        s = xmlStreamCompAddStep(stream, step.value, step.value2,
		    XML_ELEMENT_NODE, flags);
		if (s < 0)
		    goto error;
		prevs = s;
		flags = 0;
		break;
	    case XML_OP_CHILD:
		/* An element node child. */
	        s = xmlStreamCompAddStep(stream, step.value, step.value2,
		    XML_ELEMENT_NODE, flags);
		if (s < 0)
		    goto error;
		prevs = s;
		flags = 0;
		break;
	    case XML_OP_ALL:
	        s = xmlStreamCompAddStep(stream, NULL, NULL,
		    XML_ELEMENT_NODE, flags);
		if (s < 0)
		    goto error;
		prevs = s;
		flags = 0;
		break;
	    case XML_OP_PARENT:
	        break;
	    case XML_OP_ANCESTOR:
		/* Skip redundant continuations. */
		if (flags & XML_STREAM_STEP_DESC)
		    break;
	        flags |= XML_STREAM_STEP_DESC;
		/*
		* Mark the expression as having "//".
		*/
		if ((stream->flags & XML_STREAM_DESC) == 0)
		    stream->flags |= XML_STREAM_DESC;
		break;
	}
    }
    if ((! root) && (comp->flags & XML_PATTERN_NOTPATTERN) == 0) {
	/*
	* If this should behave like a real pattern, we will mark
	* the first step as having "//", to be reentrant on every
	* tree level.
	*/
	if ((stream->flags & XML_STREAM_DESC) == 0)
	    stream->flags |= XML_STREAM_DESC;

	if (stream->nbStep > 0) {
	    if ((stream->steps[0].flags & XML_STREAM_STEP_DESC) == 0)
		stream->steps[0].flags |= XML_STREAM_STEP_DESC;
	}
    }
    if (stream->nbStep <= s)
	goto error;
    stream->steps[s].flags |= XML_STREAM_STEP_FINAL;
    if (root)
	stream->steps[0].flags |= XML_STREAM_STEP_ROOT;
    comp->stream = stream;
    return(0);
error:
    xmlFreeStreamComp(stream);
    return(0);
}

/**
 * build a new stream context
 *
 * @param stream  the copmiled stream
 * @returns the new structure or NULL in case of error.
 */
static xmlStreamCtxtPtr
xmlNewStreamCtxt(xmlStreamCompPtr stream) {
    xmlStreamCtxtPtr cur;

    cur = (xmlStreamCtxtPtr) xmlMalloc(sizeof(xmlStreamCtxt));
    if (cur == NULL) {
	ERROR(NULL, NULL, NULL,
		"xmlNewStreamCtxt: malloc failed\n");
	return(NULL);
    }
    memset(cur, 0, sizeof(xmlStreamCtxt));
    cur->states = NULL;
    cur->nbState = 0;
    cur->maxState = 0;
    cur->level = 0;
    cur->comp = stream;
    cur->blockLevel = -1;
    return(cur);
}

/**
 * Free the stream context
 *
 * @param stream  the stream context
 */
void
xmlFreeStreamCtxt(xmlStreamCtxt *stream) {
    xmlStreamCtxtPtr next;

    while (stream != NULL) {
        next = stream->next;
        if (stream->states != NULL)
	    xmlFree(stream->states);
        xmlFree(stream);
	stream = next;
    }
}

/**
 * Add a new state to the stream context
 *
 * @param comp  the stream context
 * @param idx  the step index for that streaming state
 * @param level  the level
 * @returns -1 in case of error or the state index if successful
 */
static int
xmlStreamCtxtAddState(xmlStreamCtxtPtr comp, int idx, int level) {
    int i;
    for (i = 0;i < comp->nbState;i++) {
        if (comp->states[2 * i] < 0) {
	    comp->states[2 * i] = idx;
	    comp->states[2 * i + 1] = level;
	    return(i);
	}
    }
    if (comp->nbState >= comp->maxState) {
        int *tmp;
        int newSize;

        newSize = xmlGrowCapacity(comp->maxState, sizeof(tmp[0]) * 2,
                                  4, XML_MAX_ITEMS);
        if (newSize < 0) {
	    ERROR(NULL, NULL, NULL,
		  "xmlNewStreamCtxt: growCapacity failed\n");
	    return(-1);
        }
	tmp = xmlRealloc(comp->states, newSize * sizeof(tmp[0]) * 2);
	if (tmp == NULL) {
	    ERROR(NULL, NULL, NULL,
		  "xmlNewStreamCtxt: malloc failed\n");
	    return(-1);
	}
	comp->states = tmp;
        comp->maxState = newSize;
    }
    comp->states[2 * comp->nbState] = idx;
    comp->states[2 * comp->nbState++ + 1] = level;
    return(comp->nbState - 1);
}

/**
 * Push new data onto the stream. NOTE: if the call #xmlPatterncompile
 * indicated a dictionary, then strings for name and ns will be expected
 * to come from the dictionary.
 * Both `name` and `ns` being NULL means the / i.e. the root of the document.
 * This can also act as a reset.
 *
 * @param stream  the stream context
 * @param name  the current name
 * @param ns  the namespace name
 * @param nodeType  the type of the node
 * @returns -1 in case of error, 1 if the current state in the stream is a
 *    match and 0 otherwise.
 */
static int
xmlStreamPushInternal(xmlStreamCtxtPtr stream,
		      const xmlChar *name, const xmlChar *ns,
		      int nodeType) {
    int ret = 0, final = 0, tmp, i, m, match, stepNr, desc;
    xmlStreamCompPtr comp;
    xmlStreamStep step;

    if ((stream == NULL) || (stream->nbState < 0))
        return(-1);

    while (stream != NULL) {
	comp = stream->comp;

	if ((nodeType == XML_ELEMENT_NODE) &&
	    (name == NULL) && (ns == NULL)) {
	    /* We have a document node here (or a reset). */
	    stream->nbState = 0;
	    stream->level = 0;
	    stream->blockLevel = -1;
	    if (comp->flags & XML_STREAM_FROM_ROOT) {
		if (comp->nbStep == 0) {
		    /* TODO: We have a "/." here? */
		    ret = 1;
		} else {
		    if ((comp->nbStep == 1) &&
			(comp->steps[0].nodeType == XML_STREAM_ANY_NODE) &&
			(comp->steps[0].flags & XML_STREAM_STEP_DESC))
		    {
			/*
			* In the case of "//." the document node will match
			* as well.
			*/
			ret = 1;
		    } else if (comp->steps[0].flags & XML_STREAM_STEP_ROOT) {
			if (xmlStreamCtxtAddState(stream, 0, 0) < 0)
                            return(-1);
		    }
		}
	    }
	    stream = stream->next;
	    continue; /* while */
	}

	/*
	* Fast check for ".".
	*/
	if (comp->nbStep == 0) {
	    /*
	     * / and . are handled at the XPath node set creation
	     * level by checking min depth
	     */
	    if (stream->flags & XML_PATTERN_XPATH) {
		stream = stream->next;
		continue; /* while */
	    }
	    /*
	    * For non-pattern like evaluation like XML Schema IDCs
	    * or traditional XPath expressions, this will match if
	    * we are at the first level only, otherwise on every level.
	    */
	    if ((nodeType != XML_ATTRIBUTE_NODE) &&
		(((stream->flags & XML_PATTERN_NOTPATTERN) == 0) ||
		(stream->level == 0))) {
		    ret = 1;
	    }
	    stream->level++;
	    goto stream_next;
	}
	if (stream->blockLevel != -1) {
	    /*
	    * Skip blocked expressions.
	    */
	    stream->level++;
	    goto stream_next;
	}

	if ((nodeType != XML_ELEMENT_NODE) &&
	    (nodeType != XML_ATTRIBUTE_NODE) &&
	    ((comp->flags & XML_STREAM_FINAL_IS_ANY_NODE) == 0)) {
	    /*
	    * No need to process nodes of other types if we don't
	    * resolve to those types.
	    * TODO: Do we need to block the context here?
	    */
	    stream->level++;
	    goto stream_next;
	}

	/*
	 * Check evolution of existing states
	 */
	i = 0;
	m = stream->nbState;
	while (i < m) {
	    if ((comp->flags & XML_STREAM_DESC) == 0) {
		/*
		* If there is no "//", then only the last
		* added state is of interest.
		*/
		stepNr = stream->states[2 * (stream->nbState -1)];
		/*
		* TODO: Security check, should not happen, remove it.
		*/
		if (stream->states[(2 * (stream->nbState -1)) + 1] <
		    stream->level) {
		    return (-1);
		}
		desc = 0;
		/* loop-stopper */
		i = m;
	    } else {
		/*
		* If there are "//", then we need to process every "//"
		* occurring in the states, plus any other state for this
		* level.
		*/
		stepNr = stream->states[2 * i];

		/* TODO: should not happen anymore: dead states */
		if (stepNr < 0)
		    goto next_state;

		tmp = stream->states[(2 * i) + 1];

		/* skip new states just added */
		if (tmp > stream->level)
		    goto next_state;

		/* skip states at ancestor levels, except if "//" */
		desc = comp->steps[stepNr].flags & XML_STREAM_STEP_DESC;
		if ((tmp < stream->level) && (!desc))
		    goto next_state;
	    }
	    /*
	    * Check for correct node-type.
	    */
	    step = comp->steps[stepNr];
	    if (step.nodeType != nodeType) {
		if (step.nodeType == XML_ATTRIBUTE_NODE) {
		    /*
		    * Block this expression for deeper evaluation.
		    */
		    if ((comp->flags & XML_STREAM_DESC) == 0)
			stream->blockLevel = stream->level +1;
		    goto next_state;
		} else if (step.nodeType != XML_STREAM_ANY_NODE)
		    goto next_state;
	    }
	    /*
	    * Compare local/namespace-name.
	    */
	    match = 0;
	    if (step.nodeType == XML_STREAM_ANY_NODE) {
		match = 1;
	    } else if (step.name == NULL) {
		if (step.ns == NULL) {
		    /*
		    * This lets through all elements/attributes.
		    */
		    match = 1;
		} else if (ns != NULL)
		    match = xmlStrEqual(step.ns, ns);
	    } else if (((step.ns != NULL) == (ns != NULL)) &&
		(name != NULL) &&
		(step.name[0] == name[0]) &&
		xmlStrEqual(step.name, name) &&
		((step.ns == ns) || xmlStrEqual(step.ns, ns)))
	    {
		match = 1;
	    }
	    if (match) {
		final = step.flags & XML_STREAM_STEP_FINAL;
                if (final) {
                    ret = 1;
                } else if (xmlStreamCtxtAddState(stream, stepNr + 1,
                                                 stream->level + 1) < 0) {
                    return(-1);
                }
		if ((ret != 1) && (step.flags & XML_STREAM_STEP_IN_SET)) {
		    /*
		    * Check if we have a special case like "foo/bar//.", where
		    * "foo" is selected as well.
		    */
		    ret = 1;
		}
	    }
	    if (((comp->flags & XML_STREAM_DESC) == 0) &&
		((! match) || final))  {
		/*
		* Mark this expression as blocked for any evaluation at
		* deeper levels. Note that this includes "/foo"
		* expressions if the *pattern* behaviour is used.
		*/
		stream->blockLevel = stream->level +1;
	    }
next_state:
	    i++;
	}

	stream->level++;

	/*
	* Re/enter the expression.
	* Don't reenter if it's an absolute expression like "/foo",
	*   except "//foo".
	*/
	step = comp->steps[0];
	if (step.flags & XML_STREAM_STEP_ROOT)
	    goto stream_next;

	desc = step.flags & XML_STREAM_STEP_DESC;
	if (stream->flags & XML_PATTERN_NOTPATTERN) {
	    /*
	    * Re/enter the expression if it is a "descendant" one,
	    * or if we are at the 1st level of evaluation.
	    */

	    if (stream->level == 1) {
		if (XML_STREAM_XS_IDC(stream)) {
		    /*
		    * XS-IDC: The missing "self::node()" will always
		    * match the first given node.
		    */
		    goto stream_next;
		} else
		    goto compare;
	    }
	    /*
	    * A "//" is always reentrant.
	    */
	    if (desc)
		goto compare;

	    /*
	    * XS-IDC: Process the 2nd level, since the missing
	    * "self::node()" is responsible for the 2nd level being
	    * the real start level.
	    */
	    if ((stream->level == 2) && XML_STREAM_XS_IDC(stream))
		goto compare;

	    goto stream_next;
	}

compare:
	/*
	* Check expected node-type.
	*/
	if (step.nodeType != nodeType) {
	    if (nodeType == XML_ATTRIBUTE_NODE)
		goto stream_next;
	    else if (step.nodeType != XML_STREAM_ANY_NODE)
		goto stream_next;
	}
	/*
	* Compare local/namespace-name.
	*/
	match = 0;
	if (step.nodeType == XML_STREAM_ANY_NODE) {
	    match = 1;
	} else if (step.name == NULL) {
	    if (step.ns == NULL) {
		/*
		* This lets through all elements/attributes.
		*/
		match = 1;
	    } else if (ns != NULL)
		match = xmlStrEqual(step.ns, ns);
	} else if (((step.ns != NULL) == (ns != NULL)) &&
	    (name != NULL) &&
	    (step.name[0] == name[0]) &&
	    xmlStrEqual(step.name, name) &&
	    ((step.ns == ns) || xmlStrEqual(step.ns, ns)))
	{
	    match = 1;
	}
	final = step.flags & XML_STREAM_STEP_FINAL;
	if (match) {
	    if (final) {
		ret = 1;
            } else if (xmlStreamCtxtAddState(stream, 1, stream->level) < 0) {
                return(-1);
            }
	    if ((ret != 1) && (step.flags & XML_STREAM_STEP_IN_SET)) {
		/*
		* Check if we have a special case like "foo//.", where
		* "foo" is selected as well.
		*/
		ret = 1;
	    }
	}
	if (((comp->flags & XML_STREAM_DESC) == 0) &&
	    ((! match) || final))  {
	    /*
	    * Mark this expression as blocked for any evaluation at
	    * deeper levels.
	    */
	    stream->blockLevel = stream->level;
	}

stream_next:
        stream = stream->next;
    } /* while stream != NULL */

    return(ret);
}

/**
 * Push new data onto the stream. NOTE: if the call #xmlPatterncompile
 * indicated a dictionary, then strings for name and ns will be expected
 * to come from the dictionary.
 * Both `name` and `ns` being NULL means the / i.e. the root of the document.
 * This can also act as a reset.
 * Otherwise the function will act as if it has been given an element-node.
 *
 * @param stream  the stream context
 * @param name  the current name
 * @param ns  the namespace name
 * @returns -1 in case of error, 1 if the current state in the stream is a
 *    match and 0 otherwise.
 */
int
xmlStreamPush(xmlStreamCtxt *stream,
              const xmlChar *name, const xmlChar *ns) {
    return (xmlStreamPushInternal(stream, name, ns, XML_ELEMENT_NODE));
}

/**
 * Push new data onto the stream. NOTE: if the call #xmlPatterncompile
 * indicated a dictionary, then strings for name and ns will be expected
 * to come from the dictionary.
 * Both `name` and `ns` being NULL means the / i.e. the root of the document.
 * This can also act as a reset.
 * Different from #xmlStreamPush this function can be fed with nodes of type:
 * element-, attribute-, text-, cdata-section-, comment- and
 * processing-instruction-node.
 *
 * @param stream  the stream context
 * @param name  the current name
 * @param ns  the namespace name
 * @param nodeType  the type of the node being pushed
 * @returns -1 in case of error, 1 if the current state in the stream is a
 *    match and 0 otherwise.
 */
int
xmlStreamPushNode(xmlStreamCtxt *stream,
		  const xmlChar *name, const xmlChar *ns,
		  int nodeType)
{
    return (xmlStreamPushInternal(stream, name, ns,
	nodeType));
}

/**
 * Push new attribute data onto the stream.
 *
 * NOTE: If the call to #xmlPatterncompile indicated a dictionary,
 * then strings for `name` and `ns` will be expected to come from
 * the dictionary.
 *
 * Both `name` and `ns` being NULL means the root of the document.
 * This can also act as a reset. Otherwise the function will act as
 * if it has been given an attribute-node.
 *
 * @param stream  the stream context
 * @param name  the current name
 * @param ns  the namespace name
 * @returns -1 in case of error, 1 if the current state in the stream
 * is a match and 0 otherwise.
 */
int
xmlStreamPushAttr(xmlStreamCtxt *stream,
		  const xmlChar *name, const xmlChar *ns) {
    return (xmlStreamPushInternal(stream, name, ns, XML_ATTRIBUTE_NODE));
}

/**
 * push one level from the stream.
 *
 * @param stream  the stream context
 * @returns -1 in case of error, 0 otherwise.
 */
int
xmlStreamPop(xmlStreamCtxt *stream) {
    int i, lev;

    if (stream == NULL)
        return(-1);
    while (stream != NULL) {
	/*
	* Reset block-level.
	*/
	if (stream->blockLevel == stream->level)
	    stream->blockLevel = -1;

	/*
	 *  stream->level can be zero when XML_FINAL_IS_ANY_NODE is set
	 *  (see the thread at
	 *  http://mail.gnome.org/archives/xslt/2008-July/msg00027.html)
	 */
	if (stream->level)
	    stream->level--;
	/*
	 * Check evolution of existing states
	 */
	for (i = stream->nbState -1; i >= 0; i--) {
	    /* discard obsoleted states */
	    lev = stream->states[(2 * i) + 1];
	    if (lev > stream->level)
		stream->nbState--;
	    if (lev <= stream->level)
		break;
	}
	stream = stream->next;
    }
    return(0);
}

/**
 * Query if the streaming pattern additionally needs to be fed with
 * text-, cdata-section-, comment- and processing-instruction-nodes.
 * If the result is 0 then only element-nodes and attribute-nodes
 * need to be pushed.
 *
 * @param streamCtxt  the stream context
 * @returns 1 in case of need of nodes of the above described types,
 *          0 otherwise. -1 on API errors.
 */
int
xmlStreamWantsAnyNode(xmlStreamCtxt *streamCtxt)
{
    if (streamCtxt == NULL)
	return(-1);
    while (streamCtxt != NULL) {
	if (streamCtxt->comp->flags & XML_STREAM_FINAL_IS_ANY_NODE)
	    return(1);
	streamCtxt = streamCtxt->next;
    }
    return(0);
}

/************************************************************************
 *									*
 *			The public interfaces				*
 *									*
 ************************************************************************/

/**
 * Compile a pattern.
 *
 * @since 2.13.0
 *
 * @param pattern  the pattern to compile
 * @param dict  an optional dictionary for interned strings
 * @param flags  compilation flags, see xmlPatternFlags
 * @param namespaces  the prefix definitions, array of [URI, prefix] or NULL
 * @param patternOut  output pattern
 * @returns 0 on success, 1 on error, -1 if a memory allocation failed.
 */
int
xmlPatternCompileSafe(const xmlChar *pattern, xmlDict *dict, int flags,
                      const xmlChar **namespaces, xmlPattern **patternOut) {
    xmlPatternPtr ret = NULL, cur;
    xmlPatParserContextPtr ctxt = NULL;
    const xmlChar *or, *start;
    xmlChar *tmp = NULL;
    int type = 0;
    int streamable = 1;
    int error;

    if (patternOut == NULL)
        return(1);

    if (pattern == NULL) {
        error = 1;
        goto error;
    }

    start = pattern;
    or = start;
    while (*or != 0) {
	tmp = NULL;
	while ((*or != 0) && (*or != '|')) or++;
        if (*or == 0)
	    ctxt = xmlNewPatParserContext(start, dict, namespaces);
	else {
	    tmp = xmlStrndup(start, or - start);
	    if (tmp != NULL) {
		ctxt = xmlNewPatParserContext(tmp, dict, namespaces);
	    }
	    or++;
	}
	if (ctxt == NULL) {
            error = -1;
            goto error;
        }
	cur = xmlNewPattern();
	if (cur == NULL) {
            error = -1;
            goto error;
        }
	/*
	* Assign string dict.
	*/
	if (dict) {
	    cur->dict = dict;
	    xmlDictReference(dict);
	}
	if (ret == NULL)
	    ret = cur;
	else {
	    cur->next = ret->next;
	    ret->next = cur;
	}
	cur->flags = flags;
	ctxt->comp = cur;

	if (XML_STREAM_XS_IDC(cur))
	    xmlCompileIDCXPathPath(ctxt);
	else
	    xmlCompilePathPattern(ctxt);
	if (ctxt->error != 0) {
            error = ctxt->error;
	    goto error;
        }
	xmlFreePatParserContext(ctxt);
	ctxt = NULL;


        if (streamable) {
	    if (type == 0) {
	        type = cur->flags & (PAT_FROM_ROOT | PAT_FROM_CUR);
	    } else if (type == PAT_FROM_ROOT) {
	        if (cur->flags & PAT_FROM_CUR)
		    streamable = 0;
	    } else if (type == PAT_FROM_CUR) {
	        if (cur->flags & PAT_FROM_ROOT)
		    streamable = 0;
	    }
	}
	if (streamable) {
	    error = xmlStreamCompile(cur);
            if (error != 0)
                goto error;
        }
	error = xmlReversePattern(cur);
        if (error != 0)
	    goto error;
	if (tmp != NULL) {
	    xmlFree(tmp);
	    tmp = NULL;
	}
	start = or;
    }
    if (streamable == 0) {
        cur = ret;
	while (cur != NULL) {
	    if (cur->stream != NULL) {
		xmlFreeStreamComp(cur->stream);
		cur->stream = NULL;
	    }
	    cur = cur->next;
	}
    }

    *patternOut = ret;
    return(0);
error:
    if (ctxt != NULL) xmlFreePatParserContext(ctxt);
    if (ret != NULL) xmlFreePattern(ret);
    if (tmp != NULL) xmlFree(tmp);
    *patternOut = NULL;
    return(error);
}

/**
 * Compile a pattern.
 *
 * @param pattern  the pattern to compile
 * @param dict  an optional dictionary for interned strings
 * @param flags  compilation flags, see xmlPatternFlags
 * @param namespaces  the prefix definitions, array of [URI, prefix] or NULL
 * @returns the compiled form of the pattern or NULL in case of error
 */
xmlPattern *
xmlPatterncompile(const xmlChar *pattern, xmlDict *dict, int flags,
                  const xmlChar **namespaces) {
    xmlPatternPtr ret;
    xmlPatternCompileSafe(pattern, dict, flags, namespaces, &ret);
    return(ret);
}

/**
 * Test whether the node matches the pattern
 *
 * @param comp  the precompiled pattern
 * @param node  a node
 * @returns 1 if it matches, 0 if it doesn't and -1 in case of failure
 */
int
xmlPatternMatch(xmlPattern *comp, xmlNode *node)
{
    int ret = 0;

    if ((comp == NULL) || (node == NULL))
        return(-1);

    while (comp != NULL) {
        ret = xmlPatMatch(comp, node);
	if (ret != 0)
	    return(ret);
	comp = comp->next;
    }
    return(ret);
}

/**
 * Get a streaming context for that pattern
 * Use #xmlFreeStreamCtxt to free the context.
 *
 * @param comp  the precompiled pattern
 * @returns a pointer to the context or NULL in case of failure
 */
xmlStreamCtxt *
xmlPatternGetStreamCtxt(xmlPattern *comp)
{
    xmlStreamCtxtPtr ret = NULL, cur;

    if ((comp == NULL) || (comp->stream == NULL))
        return(NULL);

    while (comp != NULL) {
        if (comp->stream == NULL)
	    goto failed;
	cur = xmlNewStreamCtxt(comp->stream);
	if (cur == NULL)
	    goto failed;
	if (ret == NULL)
	    ret = cur;
	else {
	    cur->next = ret->next;
	    ret->next = cur;
	}
	cur->flags = comp->flags;
	comp = comp->next;
    }
    return(ret);
failed:
    xmlFreeStreamCtxt(ret);
    return(NULL);
}

/**
 * Check if the pattern is streamable i.e. #xmlPatternGetStreamCtxt
 * should work.
 *
 * @param comp  the precompiled pattern
 * @returns 1 if streamable, 0 if not and -1 in case of error.
 */
int
xmlPatternStreamable(xmlPattern *comp) {
    if (comp == NULL)
        return(-1);
    while (comp != NULL) {
        if (comp->stream == NULL)
	    return(0);
	comp = comp->next;
    }
    return(1);
}

/**
 * Check the maximum depth reachable by a pattern
 *
 * @param comp  the precompiled pattern
 * @returns -2 if no limit (using //), otherwise the depth,
 *         and -1 in case of error
 */
int
xmlPatternMaxDepth(xmlPattern *comp) {
    int ret = 0, i;
    if (comp == NULL)
        return(-1);
    while (comp != NULL) {
        if (comp->stream == NULL)
	    return(-1);
	for (i = 0;i < comp->stream->nbStep;i++)
	    if (comp->stream->steps[i].flags & XML_STREAM_STEP_DESC)
	        return(-2);
	if (comp->stream->nbStep > ret)
	    ret = comp->stream->nbStep;
	comp = comp->next;
    }
    return(ret);
}

/**
 * Check the minimum depth reachable by a pattern, 0 mean the / or . are
 * part of the set.
 *
 * @param comp  the precompiled pattern
 * @returns -1 in case of error otherwise the depth,
 */
int
xmlPatternMinDepth(xmlPattern *comp) {
    int ret = 12345678;
    if (comp == NULL)
        return(-1);
    while (comp != NULL) {
        if (comp->stream == NULL)
	    return(-1);
	if (comp->stream->nbStep < ret)
	    ret = comp->stream->nbStep;
	if (ret == 0)
	    return(0);
	comp = comp->next;
    }
    return(ret);
}

/**
 * Check if the pattern must be looked at from the root.
 *
 * @param comp  the precompiled pattern
 * @returns 1 if true, 0 if false and -1 in case of error
 */
int
xmlPatternFromRoot(xmlPattern *comp) {
    if (comp == NULL)
        return(-1);
    while (comp != NULL) {
        if (comp->stream == NULL)
	    return(-1);
	if (comp->flags & PAT_FROM_ROOT)
	    return(1);
	comp = comp->next;
    }
    return(0);

}

#endif /* LIBXML_PATTERN_ENABLED */
