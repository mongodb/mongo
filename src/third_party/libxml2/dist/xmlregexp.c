/*
 * regexp.c: generic and extensible Regular Expression engine
 *
 * Basically designed with the purpose of compiling regexps for
 * the variety of validation/schemas mechanisms now available in
 * XML related specifications these include:
 *    - XML-1.0 DTD validation
 *    - XML Schemas structure part 1
 *    - XML Schemas Datatypes part 2 especially Appendix F
 *    - RELAX-NG/TREX i.e. the counter proposal
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_REGEXP_ENABLED

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <libxml/tree.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlregexp.h>
#include <libxml/xmlautomata.h>

#include "private/error.h"
#include "private/memory.h"
#include "private/regexp.h"

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t) -1)
#endif

/* #define DEBUG_REGEXP */

#define MAX_PUSH 10000000

#ifdef ERROR
#undef ERROR
#endif
#define ERROR(str)							\
    ctxt->error = XML_REGEXP_COMPILE_ERROR;				\
    xmlRegexpErrCompile(ctxt, str);
#define NEXT ctxt->cur++
#define CUR (*(ctxt->cur))
#define NXT(index) (ctxt->cur[index])

#define NEXTL(l) ctxt->cur += l;
#define XML_REG_STRING_SEPARATOR '|'
/*
 * Need PREV to check on a '-' within a Character Group. May only be used
 * when it's guaranteed that cur is not at the beginning of ctxt->string!
 */
#define PREV (ctxt->cur[-1])

/************************************************************************
 *									*
 *			Unicode support					*
 *									*
 ************************************************************************/

typedef struct {
    const char *rangename;
    const xmlChRangeGroup group;
} xmlUnicodeRange;

#include "codegen/unicode.inc"

/**
 * binary table lookup for user-supplied name
 *
 * @param sptr  a table of xmlUnicodeRange structs
 * @param numentries  number of table entries
 * @param tname  name to be found
 * @returns pointer to range function if found, otherwise NULL
 */
static const xmlChRangeGroup *
xmlUnicodeLookup(const xmlUnicodeRange *sptr, int numentries,
                 const char *tname) {
    int low, high, mid, cmp;

    if (tname == NULL) return(NULL);

    low = 0;
    high = numentries - 1;
    while (low <= high) {
	mid = (low + high) / 2;
	cmp = strcmp(tname, sptr[mid].rangename);
	if (cmp == 0)
	    return (&sptr[mid].group);
	if (cmp < 0)
	    high = mid - 1;
	else
	    low = mid + 1;
    }
    return (NULL);
}

/**
 * Check whether the character is part of the UCS Block
 *
 * @param code  UCS code point
 * @param block  UCS block name
 * @returns 1 if true, 0 if false and -1 on unknown block
 */
static int
xmlUCSIsBlock(int code, const char *block) {
    const xmlChRangeGroup *group;

    group = xmlUnicodeLookup(xmlUnicodeBlocks,
            sizeof(xmlUnicodeBlocks) / sizeof(xmlUnicodeBlocks[0]), block);
    if (group == NULL)
	return (-1);
    return (xmlCharInRange(code, group));
}

/************************************************************************
 *									*
 *			Datatypes and structures			*
 *									*
 ************************************************************************/

/*
 * Note: the order of the enums below is significant, do not shuffle
 */
typedef enum {
    XML_REGEXP_EPSILON = 1,
    XML_REGEXP_CHARVAL,
    XML_REGEXP_RANGES,
    XML_REGEXP_SUBREG,  /* used for () sub regexps */
    XML_REGEXP_STRING,
    XML_REGEXP_ANYCHAR, /* . */
    XML_REGEXP_ANYSPACE, /* \s */
    XML_REGEXP_NOTSPACE, /* \S */
    XML_REGEXP_INITNAME, /* \l */
    XML_REGEXP_NOTINITNAME, /* \L */
    XML_REGEXP_NAMECHAR, /* \c */
    XML_REGEXP_NOTNAMECHAR, /* \C */
    XML_REGEXP_DECIMAL, /* \d */
    XML_REGEXP_NOTDECIMAL, /* \D */
    XML_REGEXP_REALCHAR, /* \w */
    XML_REGEXP_NOTREALCHAR, /* \W */
    XML_REGEXP_LETTER = 100,
    XML_REGEXP_LETTER_UPPERCASE,
    XML_REGEXP_LETTER_LOWERCASE,
    XML_REGEXP_LETTER_TITLECASE,
    XML_REGEXP_LETTER_MODIFIER,
    XML_REGEXP_LETTER_OTHERS,
    XML_REGEXP_MARK,
    XML_REGEXP_MARK_NONSPACING,
    XML_REGEXP_MARK_SPACECOMBINING,
    XML_REGEXP_MARK_ENCLOSING,
    XML_REGEXP_NUMBER,
    XML_REGEXP_NUMBER_DECIMAL,
    XML_REGEXP_NUMBER_LETTER,
    XML_REGEXP_NUMBER_OTHERS,
    XML_REGEXP_PUNCT,
    XML_REGEXP_PUNCT_CONNECTOR,
    XML_REGEXP_PUNCT_DASH,
    XML_REGEXP_PUNCT_OPEN,
    XML_REGEXP_PUNCT_CLOSE,
    XML_REGEXP_PUNCT_INITQUOTE,
    XML_REGEXP_PUNCT_FINQUOTE,
    XML_REGEXP_PUNCT_OTHERS,
    XML_REGEXP_SEPAR,
    XML_REGEXP_SEPAR_SPACE,
    XML_REGEXP_SEPAR_LINE,
    XML_REGEXP_SEPAR_PARA,
    XML_REGEXP_SYMBOL,
    XML_REGEXP_SYMBOL_MATH,
    XML_REGEXP_SYMBOL_CURRENCY,
    XML_REGEXP_SYMBOL_MODIFIER,
    XML_REGEXP_SYMBOL_OTHERS,
    XML_REGEXP_OTHER,
    XML_REGEXP_OTHER_CONTROL,
    XML_REGEXP_OTHER_FORMAT,
    XML_REGEXP_OTHER_PRIVATE,
    XML_REGEXP_OTHER_NA,
    XML_REGEXP_BLOCK_NAME
} xmlRegAtomType;

typedef enum {
    XML_REGEXP_QUANT_EPSILON = 1,
    XML_REGEXP_QUANT_ONCE,
    XML_REGEXP_QUANT_OPT,
    XML_REGEXP_QUANT_MULT,
    XML_REGEXP_QUANT_PLUS,
    XML_REGEXP_QUANT_ONCEONLY,
    XML_REGEXP_QUANT_ALL,
    XML_REGEXP_QUANT_RANGE
} xmlRegQuantType;

typedef enum {
    XML_REGEXP_START_STATE = 1,
    XML_REGEXP_FINAL_STATE,
    XML_REGEXP_TRANS_STATE,
    XML_REGEXP_SINK_STATE,
    XML_REGEXP_UNREACH_STATE
} xmlRegStateType;

typedef enum {
    XML_REGEXP_MARK_NORMAL = 0,
    XML_REGEXP_MARK_START,
    XML_REGEXP_MARK_VISITED
} xmlRegMarkedType;

typedef struct _xmlRegRange xmlRegRange;
typedef xmlRegRange *xmlRegRangePtr;

struct _xmlRegRange {
    int neg;		/* 0 normal, 1 not, 2 exclude */
    xmlRegAtomType type;
    int start;
    int end;
    xmlChar *blockName;
};

typedef struct _xmlRegAtom xmlRegAtom;
typedef xmlRegAtom *xmlRegAtomPtr;

typedef struct _xmlAutomataState xmlRegState;
typedef xmlRegState *xmlRegStatePtr;

struct _xmlRegAtom {
    int no;
    xmlRegAtomType type;
    xmlRegQuantType quant;
    int min;
    int max;

    void *valuep;
    void *valuep2;
    int neg;
    int codepoint;
    xmlRegStatePtr start;
    xmlRegStatePtr start0;
    xmlRegStatePtr stop;
    int maxRanges;
    int nbRanges;
    xmlRegRangePtr *ranges;
    void *data;
};

typedef struct _xmlRegCounter xmlRegCounter;
typedef xmlRegCounter *xmlRegCounterPtr;

struct _xmlRegCounter {
    int min;
    int max;
};

typedef struct _xmlRegTrans xmlRegTrans;
typedef xmlRegTrans *xmlRegTransPtr;

struct _xmlRegTrans {
    xmlRegAtomPtr atom;
    int to;
    int counter;
    int count;
    int nd;
};

struct _xmlAutomataState {
    xmlRegStateType type;
    xmlRegMarkedType mark;
    xmlRegMarkedType markd;
    xmlRegMarkedType reached;
    int no;
    int maxTrans;
    int nbTrans;
    xmlRegTrans *trans;
    /*  knowing states pointing to us can speed things up */
    int maxTransTo;
    int nbTransTo;
    int *transTo;
};

typedef struct _xmlAutomata xmlRegParserCtxt;
typedef xmlRegParserCtxt *xmlRegParserCtxtPtr;

#define AM_AUTOMATA_RNG 1

struct _xmlAutomata {
    xmlChar *string;
    xmlChar *cur;

    int error;
    int neg;

    xmlRegStatePtr start;
    xmlRegStatePtr end;
    xmlRegStatePtr state;

    xmlRegAtomPtr atom;

    int maxAtoms;
    int nbAtoms;
    xmlRegAtomPtr *atoms;

    int maxStates;
    int nbStates;
    xmlRegStatePtr *states;

    int maxCounters;
    int nbCounters;
    xmlRegCounter *counters;

    int determinist;
    int negs;
    int flags;

    int depth;
};

struct _xmlRegexp {
    xmlChar *string;
    int nbStates;
    xmlRegStatePtr *states;
    int nbAtoms;
    xmlRegAtomPtr *atoms;
    int nbCounters;
    xmlRegCounter *counters;
    int determinist;
    int flags;
    /*
     * That's the compact form for determinists automatas
     */
    int nbstates;
    int *compact;
    void **transdata;
    int nbstrings;
    xmlChar **stringMap;
};

typedef struct _xmlRegExecRollback xmlRegExecRollback;
typedef xmlRegExecRollback *xmlRegExecRollbackPtr;

struct _xmlRegExecRollback {
    xmlRegStatePtr state;/* the current state */
    int index;		/* the index in the input stack */
    int nextbranch;	/* the next transition to explore in that state */
    int *counts;	/* save the automata state if it has some */
};

typedef struct _xmlRegInputToken xmlRegInputToken;
typedef xmlRegInputToken *xmlRegInputTokenPtr;

struct _xmlRegInputToken {
    xmlChar *value;
    void *data;
};

struct _xmlRegExecCtxt {
    int status;		/* execution status != 0 indicate an error */
    int determinist;	/* did we find an indeterministic behaviour */
    xmlRegexpPtr comp;	/* the compiled regexp */
    xmlRegExecCallbacks callback;
    void *data;

    xmlRegStatePtr state;/* the current state */
    int transno;	/* the current transition on that state */
    int transcount;	/* the number of chars in char counted transitions */

    /*
     * A stack of rollback states
     */
    int maxRollbacks;
    int nbRollbacks;
    xmlRegExecRollback *rollbacks;

    /*
     * The state of the automata if any
     */
    int *counts;

    /*
     * The input stack
     */
    int inputStackMax;
    int inputStackNr;
    int index;
    int *charStack;
    const xmlChar *inputString; /* when operating on characters */
    xmlRegInputTokenPtr inputStack;/* when operating on strings */

    /*
     * error handling
     */
    int errStateNo;		/* the error state number */
    xmlRegStatePtr errState;    /* the error state */
    xmlChar *errString;		/* the string raising the error */
    int *errCounts;		/* counters at the error state */
    int nbPush;
};

#define REGEXP_ALL_COUNTER	0x123456
#define REGEXP_ALL_LAX_COUNTER	0x123457

static void xmlFAParseRegExp(xmlRegParserCtxtPtr ctxt, int top);
static void xmlRegFreeState(xmlRegStatePtr state);
static void xmlRegFreeAtom(xmlRegAtomPtr atom);
static int xmlRegStrEqualWildcard(const xmlChar *expStr, const xmlChar *valStr);
static int xmlRegCheckCharacter(xmlRegAtomPtr atom, int codepoint);
static int xmlRegCheckCharacterRange(xmlRegAtomType type, int codepoint,
                  int neg, int start, int end, const xmlChar *blockName);

/************************************************************************
 *									*
 *		Regexp memory error handler				*
 *									*
 ************************************************************************/
/**
 * Handle an out of memory condition
 *
 * @param ctxt  regexp parser context
 */
static void
xmlRegexpErrMemory(xmlRegParserCtxtPtr ctxt)
{
    if (ctxt != NULL)
        ctxt->error = XML_ERR_NO_MEMORY;

    xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_REGEXP, NULL);
}

/**
 * Handle a compilation failure
 *
 * @param ctxt  regexp parser context
 * @param extra  extra information
 */
static void
xmlRegexpErrCompile(xmlRegParserCtxtPtr ctxt, const char *extra)
{
    const char *regexp = NULL;
    int idx = 0;
    int res;

    if (ctxt != NULL) {
        regexp = (const char *) ctxt->string;
	idx = ctxt->cur - ctxt->string;
	ctxt->error = XML_REGEXP_COMPILE_ERROR;
    }

    res = xmlRaiseError(NULL, NULL, NULL, NULL, NULL, XML_FROM_REGEXP,
                        XML_REGEXP_COMPILE_ERROR, XML_ERR_FATAL,
                        NULL, 0, extra, regexp, NULL, idx, 0,
                        "failed to compile: %s\n", extra);
    if (res < 0)
        xmlRegexpErrMemory(ctxt);
}

/************************************************************************
 *									*
 *			Allocation/Deallocation				*
 *									*
 ************************************************************************/

static int xmlFAComputesDeterminism(xmlRegParserCtxtPtr ctxt);

/**
 * Allocate a two-dimensional array and set all elements to zero.
 *
 * @param dim1  size of first dimension
 * @param dim2  size of second dimension
 * @param elemSize  size of element
 * @returns the new array or NULL in case of error.
 */
static void*
xmlRegCalloc2(size_t dim1, size_t dim2, size_t elemSize) {
    size_t numElems, totalSize;
    void *ret;

    /* Check for overflow */
    if ((dim2 == 0) || (elemSize == 0) ||
        (dim1 > SIZE_MAX / dim2 / elemSize))
        return (NULL);
    numElems = dim1 * dim2;
    if (numElems > XML_MAX_ITEMS)
        return NULL;
    totalSize = numElems * elemSize;
    ret = xmlMalloc(totalSize);
    if (ret != NULL)
        memset(ret, 0, totalSize);
    return (ret);
}

/**
 * Allocate a new regexp and fill it with the result from the parser
 *
 * @param ctxt  the parser context used to build it
 * @returns the new regexp or NULL in case of error
 */
static xmlRegexpPtr
xmlRegEpxFromParse(xmlRegParserCtxtPtr ctxt) {
    xmlRegexpPtr ret;

    ret = (xmlRegexpPtr) xmlMalloc(sizeof(xmlRegexp));
    if (ret == NULL) {
	xmlRegexpErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlRegexp));
    ret->string = ctxt->string;
    ret->nbStates = ctxt->nbStates;
    ret->states = ctxt->states;
    ret->nbAtoms = ctxt->nbAtoms;
    ret->atoms = ctxt->atoms;
    ret->nbCounters = ctxt->nbCounters;
    ret->counters = ctxt->counters;
    ret->determinist = ctxt->determinist;
    ret->flags = ctxt->flags;
    if (ret->determinist == -1) {
        if (xmlRegexpIsDeterminist(ret) < 0) {
            xmlRegexpErrMemory(ctxt);
            xmlFree(ret);
            return(NULL);
        }
    }

    if ((ret->determinist != 0) &&
	(ret->nbCounters == 0) &&
	(ctxt->negs == 0) &&
	(ret->atoms != NULL) &&
	(ret->atoms[0] != NULL) &&
	(ret->atoms[0]->type == XML_REGEXP_STRING)) {
	int i, j, nbstates = 0, nbatoms = 0;
	int *stateRemap;
	int *stringRemap;
	int *transitions;
	void **transdata;
	xmlChar **stringMap;
        xmlChar *value;

	/*
	 * Switch to a compact representation
	 * 1/ counting the effective number of states left
	 * 2/ counting the unique number of atoms, and check that
	 *    they are all of the string type
	 * 3/ build a table state x atom for the transitions
	 */

	stateRemap = xmlMalloc(ret->nbStates * sizeof(int));
	if (stateRemap == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    xmlFree(ret);
	    return(NULL);
	}
	for (i = 0;i < ret->nbStates;i++) {
	    if (ret->states[i] != NULL) {
		stateRemap[i] = nbstates;
		nbstates++;
	    } else {
		stateRemap[i] = -1;
	    }
	}
	stringMap = xmlMalloc(ret->nbAtoms * sizeof(char *));
	if (stringMap == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    xmlFree(stateRemap);
	    xmlFree(ret);
	    return(NULL);
	}
	stringRemap = xmlMalloc(ret->nbAtoms * sizeof(int));
	if (stringRemap == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    xmlFree(stringMap);
	    xmlFree(stateRemap);
	    xmlFree(ret);
	    return(NULL);
	}
	for (i = 0;i < ret->nbAtoms;i++) {
	    if ((ret->atoms[i]->type == XML_REGEXP_STRING) &&
		(ret->atoms[i]->quant == XML_REGEXP_QUANT_ONCE)) {
		value = ret->atoms[i]->valuep;
                for (j = 0;j < nbatoms;j++) {
		    if (xmlStrEqual(stringMap[j], value)) {
			stringRemap[i] = j;
			break;
		    }
		}
		if (j >= nbatoms) {
		    stringRemap[i] = nbatoms;
		    stringMap[nbatoms] = xmlStrdup(value);
		    if (stringMap[nbatoms] == NULL) {
			for (i = 0;i < nbatoms;i++)
			    xmlFree(stringMap[i]);
			xmlFree(stringRemap);
			xmlFree(stringMap);
			xmlFree(stateRemap);
			xmlFree(ret);
			return(NULL);
		    }
		    nbatoms++;
		}
	    } else {
		xmlFree(stateRemap);
		xmlFree(stringRemap);
		for (i = 0;i < nbatoms;i++)
		    xmlFree(stringMap[i]);
		xmlFree(stringMap);
		xmlFree(ret);
		return(NULL);
	    }
	}
	transitions = (int *) xmlRegCalloc2(nbstates + 1, nbatoms + 1,
                                            sizeof(int));
	if (transitions == NULL) {
	    xmlFree(stateRemap);
	    xmlFree(stringRemap);
            for (i = 0;i < nbatoms;i++)
		xmlFree(stringMap[i]);
	    xmlFree(stringMap);
	    xmlFree(ret);
	    return(NULL);
	}

	/*
	 * Allocate the transition table. The first entry for each
	 * state corresponds to the state type.
	 */
	transdata = NULL;

	for (i = 0;i < ret->nbStates;i++) {
	    int stateno, atomno, targetno, prev;
	    xmlRegStatePtr state;
	    xmlRegTransPtr trans;

	    stateno = stateRemap[i];
	    if (stateno == -1)
		continue;
	    state = ret->states[i];

	    transitions[stateno * (nbatoms + 1)] = state->type;

	    for (j = 0;j < state->nbTrans;j++) {
		trans = &(state->trans[j]);
		if ((trans->to < 0) || (trans->atom == NULL))
		    continue;
                atomno = stringRemap[trans->atom->no];
		if ((trans->atom->data != NULL) && (transdata == NULL)) {
		    transdata = (void **) xmlRegCalloc2(nbstates, nbatoms,
			                                sizeof(void *));
		    if (transdata == NULL) {
			xmlRegexpErrMemory(ctxt);
			break;
		    }
		}
		targetno = stateRemap[trans->to];
		/*
		 * if the same atom can generate transitions to 2 different
		 * states then it means the automata is not deterministic and
		 * the compact form can't be used !
		 */
		prev = transitions[stateno * (nbatoms + 1) + atomno + 1];
		if (prev != 0) {
		    if (prev != targetno + 1) {
			ret->determinist = 0;
			if (transdata != NULL)
			    xmlFree(transdata);
			xmlFree(transitions);
			xmlFree(stateRemap);
			xmlFree(stringRemap);
			for (i = 0;i < nbatoms;i++)
			    xmlFree(stringMap[i]);
			xmlFree(stringMap);
			goto not_determ;
		    }
		} else {
		    transitions[stateno * (nbatoms + 1) + atomno + 1] =
			targetno + 1; /* to avoid 0 */
		    if (transdata != NULL)
			transdata[stateno * nbatoms + atomno] =
			    trans->atom->data;
		}
	    }
	}
	ret->determinist = 1;
	/*
	 * Cleanup of the old data
	 */
	if (ret->states != NULL) {
	    for (i = 0;i < ret->nbStates;i++)
		xmlRegFreeState(ret->states[i]);
	    xmlFree(ret->states);
	}
	ret->states = NULL;
	ret->nbStates = 0;
	if (ret->atoms != NULL) {
	    for (i = 0;i < ret->nbAtoms;i++)
		xmlRegFreeAtom(ret->atoms[i]);
	    xmlFree(ret->atoms);
	}
	ret->atoms = NULL;
	ret->nbAtoms = 0;

	ret->compact = transitions;
	ret->transdata = transdata;
	ret->stringMap = stringMap;
	ret->nbstrings = nbatoms;
	ret->nbstates = nbstates;
	xmlFree(stateRemap);
	xmlFree(stringRemap);
    }
not_determ:
    ctxt->string = NULL;
    ctxt->nbStates = 0;
    ctxt->states = NULL;
    ctxt->nbAtoms = 0;
    ctxt->atoms = NULL;
    ctxt->nbCounters = 0;
    ctxt->counters = NULL;
    return(ret);
}

/**
 * Allocate a new regexp parser context
 *
 * @param string  the string to parse
 * @returns the new context or NULL in case of error
 */
static xmlRegParserCtxtPtr
xmlRegNewParserCtxt(const xmlChar *string) {
    xmlRegParserCtxtPtr ret;

    ret = (xmlRegParserCtxtPtr) xmlMalloc(sizeof(xmlRegParserCtxt));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlRegParserCtxt));
    if (string != NULL) {
	ret->string = xmlStrdup(string);
        if (ret->string == NULL) {
            xmlFree(ret);
            return(NULL);
        }
    }
    ret->cur = ret->string;
    ret->neg = 0;
    ret->negs = 0;
    ret->error = 0;
    ret->determinist = -1;
    return(ret);
}

/**
 * Allocate a new regexp range
 *
 * @param ctxt  the regexp parser context
 * @param neg  is that negative
 * @param type  the type of range
 * @param start  the start codepoint
 * @param end  the end codepoint
 * @returns the new range or NULL in case of error
 */
static xmlRegRangePtr
xmlRegNewRange(xmlRegParserCtxtPtr ctxt,
	       int neg, xmlRegAtomType type, int start, int end) {
    xmlRegRangePtr ret;

    ret = (xmlRegRangePtr) xmlMalloc(sizeof(xmlRegRange));
    if (ret == NULL) {
	xmlRegexpErrMemory(ctxt);
	return(NULL);
    }
    ret->neg = neg;
    ret->type = type;
    ret->start = start;
    ret->end = end;
    return(ret);
}

/**
 * Free a regexp range
 *
 * @param range  the regexp range
 */
static void
xmlRegFreeRange(xmlRegRangePtr range) {
    if (range == NULL)
	return;

    if (range->blockName != NULL)
	xmlFree(range->blockName);
    xmlFree(range);
}

/**
 * Copy a regexp range
 *
 * @param ctxt  regexp parser context
 * @param range  the regexp range
 * @returns the new copy or NULL in case of error.
 */
static xmlRegRangePtr
xmlRegCopyRange(xmlRegParserCtxtPtr ctxt, xmlRegRangePtr range) {
    xmlRegRangePtr ret;

    if (range == NULL)
	return(NULL);

    ret = xmlRegNewRange(ctxt, range->neg, range->type, range->start,
                         range->end);
    if (ret == NULL)
        return(NULL);
    if (range->blockName != NULL) {
	ret->blockName = xmlStrdup(range->blockName);
	if (ret->blockName == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    xmlRegFreeRange(ret);
	    return(NULL);
	}
    }
    return(ret);
}

/**
 * Allocate a new atom
 *
 * @param ctxt  the regexp parser context
 * @param type  the type of atom
 * @returns the new atom or NULL in case of error
 */
static xmlRegAtomPtr
xmlRegNewAtom(xmlRegParserCtxtPtr ctxt, xmlRegAtomType type) {
    xmlRegAtomPtr ret;

    ret = (xmlRegAtomPtr) xmlMalloc(sizeof(xmlRegAtom));
    if (ret == NULL) {
	xmlRegexpErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlRegAtom));
    ret->type = type;
    ret->quant = XML_REGEXP_QUANT_ONCE;
    ret->min = 0;
    ret->max = 0;
    return(ret);
}

/**
 * Free a regexp atom
 *
 * @param atom  the regexp atom
 */
static void
xmlRegFreeAtom(xmlRegAtomPtr atom) {
    int i;

    if (atom == NULL)
	return;

    for (i = 0;i < atom->nbRanges;i++)
	xmlRegFreeRange(atom->ranges[i]);
    if (atom->ranges != NULL)
	xmlFree(atom->ranges);
    if ((atom->type == XML_REGEXP_STRING) && (atom->valuep != NULL))
	xmlFree(atom->valuep);
    if ((atom->type == XML_REGEXP_STRING) && (atom->valuep2 != NULL))
	xmlFree(atom->valuep2);
    if ((atom->type == XML_REGEXP_BLOCK_NAME) && (atom->valuep != NULL))
	xmlFree(atom->valuep);
    xmlFree(atom);
}

/**
 * Allocate a new regexp range
 *
 * @param ctxt  the regexp parser context
 * @param atom  the original atom
 * @returns the new atom or NULL in case of error
 */
static xmlRegAtomPtr
xmlRegCopyAtom(xmlRegParserCtxtPtr ctxt, xmlRegAtomPtr atom) {
    xmlRegAtomPtr ret;

    ret = (xmlRegAtomPtr) xmlMalloc(sizeof(xmlRegAtom));
    if (ret == NULL) {
	xmlRegexpErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlRegAtom));
    ret->type = atom->type;
    ret->quant = atom->quant;
    ret->min = atom->min;
    ret->max = atom->max;
    if (atom->nbRanges > 0) {
        int i;

        ret->ranges = (xmlRegRangePtr *) xmlMalloc(sizeof(xmlRegRangePtr) *
	                                           atom->nbRanges);
	if (ret->ranges == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    goto error;
	}
	for (i = 0;i < atom->nbRanges;i++) {
	    ret->ranges[i] = xmlRegCopyRange(ctxt, atom->ranges[i]);
	    if (ret->ranges[i] == NULL)
	        goto error;
	    ret->nbRanges = i + 1;
	}
    }
    return(ret);

error:
    xmlRegFreeAtom(ret);
    return(NULL);
}

static xmlRegStatePtr
xmlRegNewState(xmlRegParserCtxtPtr ctxt) {
    xmlRegStatePtr ret;

    ret = (xmlRegStatePtr) xmlMalloc(sizeof(xmlRegState));
    if (ret == NULL) {
	xmlRegexpErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlRegState));
    ret->type = XML_REGEXP_TRANS_STATE;
    ret->mark = XML_REGEXP_MARK_NORMAL;
    return(ret);
}

/**
 * Free a regexp state
 *
 * @param state  the regexp state
 */
static void
xmlRegFreeState(xmlRegStatePtr state) {
    if (state == NULL)
	return;

    if (state->trans != NULL)
	xmlFree(state->trans);
    if (state->transTo != NULL)
	xmlFree(state->transTo);
    xmlFree(state);
}

/**
 * Free a regexp parser context
 *
 * @param ctxt  the regexp parser context
 */
static void
xmlRegFreeParserCtxt(xmlRegParserCtxtPtr ctxt) {
    int i;
    if (ctxt == NULL)
	return;

    if (ctxt->string != NULL)
	xmlFree(ctxt->string);
    if (ctxt->states != NULL) {
	for (i = 0;i < ctxt->nbStates;i++)
	    xmlRegFreeState(ctxt->states[i]);
	xmlFree(ctxt->states);
    }
    if (ctxt->atoms != NULL) {
	for (i = 0;i < ctxt->nbAtoms;i++)
	    xmlRegFreeAtom(ctxt->atoms[i]);
	xmlFree(ctxt->atoms);
    }
    if (ctxt->counters != NULL)
	xmlFree(ctxt->counters);
    xmlFree(ctxt);
}

/************************************************************************
 *									*
 *			Display of Data structures			*
 *									*
 ************************************************************************/

#ifdef DEBUG_REGEXP
static void
xmlRegPrintAtomType(FILE *output, xmlRegAtomType type) {
    switch (type) {
        case XML_REGEXP_EPSILON:
	    fprintf(output, "epsilon "); break;
        case XML_REGEXP_CHARVAL:
	    fprintf(output, "charval "); break;
        case XML_REGEXP_RANGES:
	    fprintf(output, "ranges "); break;
        case XML_REGEXP_SUBREG:
	    fprintf(output, "subexpr "); break;
        case XML_REGEXP_STRING:
	    fprintf(output, "string "); break;
        case XML_REGEXP_ANYCHAR:
	    fprintf(output, "anychar "); break;
        case XML_REGEXP_ANYSPACE:
	    fprintf(output, "anyspace "); break;
        case XML_REGEXP_NOTSPACE:
	    fprintf(output, "notspace "); break;
        case XML_REGEXP_INITNAME:
	    fprintf(output, "initname "); break;
        case XML_REGEXP_NOTINITNAME:
	    fprintf(output, "notinitname "); break;
        case XML_REGEXP_NAMECHAR:
	    fprintf(output, "namechar "); break;
        case XML_REGEXP_NOTNAMECHAR:
	    fprintf(output, "notnamechar "); break;
        case XML_REGEXP_DECIMAL:
	    fprintf(output, "decimal "); break;
        case XML_REGEXP_NOTDECIMAL:
	    fprintf(output, "notdecimal "); break;
        case XML_REGEXP_REALCHAR:
	    fprintf(output, "realchar "); break;
        case XML_REGEXP_NOTREALCHAR:
	    fprintf(output, "notrealchar "); break;
        case XML_REGEXP_LETTER:
            fprintf(output, "LETTER "); break;
        case XML_REGEXP_LETTER_UPPERCASE:
            fprintf(output, "LETTER_UPPERCASE "); break;
        case XML_REGEXP_LETTER_LOWERCASE:
            fprintf(output, "LETTER_LOWERCASE "); break;
        case XML_REGEXP_LETTER_TITLECASE:
            fprintf(output, "LETTER_TITLECASE "); break;
        case XML_REGEXP_LETTER_MODIFIER:
            fprintf(output, "LETTER_MODIFIER "); break;
        case XML_REGEXP_LETTER_OTHERS:
            fprintf(output, "LETTER_OTHERS "); break;
        case XML_REGEXP_MARK:
            fprintf(output, "MARK "); break;
        case XML_REGEXP_MARK_NONSPACING:
            fprintf(output, "MARK_NONSPACING "); break;
        case XML_REGEXP_MARK_SPACECOMBINING:
            fprintf(output, "MARK_SPACECOMBINING "); break;
        case XML_REGEXP_MARK_ENCLOSING:
            fprintf(output, "MARK_ENCLOSING "); break;
        case XML_REGEXP_NUMBER:
            fprintf(output, "NUMBER "); break;
        case XML_REGEXP_NUMBER_DECIMAL:
            fprintf(output, "NUMBER_DECIMAL "); break;
        case XML_REGEXP_NUMBER_LETTER:
            fprintf(output, "NUMBER_LETTER "); break;
        case XML_REGEXP_NUMBER_OTHERS:
            fprintf(output, "NUMBER_OTHERS "); break;
        case XML_REGEXP_PUNCT:
            fprintf(output, "PUNCT "); break;
        case XML_REGEXP_PUNCT_CONNECTOR:
            fprintf(output, "PUNCT_CONNECTOR "); break;
        case XML_REGEXP_PUNCT_DASH:
            fprintf(output, "PUNCT_DASH "); break;
        case XML_REGEXP_PUNCT_OPEN:
            fprintf(output, "PUNCT_OPEN "); break;
        case XML_REGEXP_PUNCT_CLOSE:
            fprintf(output, "PUNCT_CLOSE "); break;
        case XML_REGEXP_PUNCT_INITQUOTE:
            fprintf(output, "PUNCT_INITQUOTE "); break;
        case XML_REGEXP_PUNCT_FINQUOTE:
            fprintf(output, "PUNCT_FINQUOTE "); break;
        case XML_REGEXP_PUNCT_OTHERS:
            fprintf(output, "PUNCT_OTHERS "); break;
        case XML_REGEXP_SEPAR:
            fprintf(output, "SEPAR "); break;
        case XML_REGEXP_SEPAR_SPACE:
            fprintf(output, "SEPAR_SPACE "); break;
        case XML_REGEXP_SEPAR_LINE:
            fprintf(output, "SEPAR_LINE "); break;
        case XML_REGEXP_SEPAR_PARA:
            fprintf(output, "SEPAR_PARA "); break;
        case XML_REGEXP_SYMBOL:
            fprintf(output, "SYMBOL "); break;
        case XML_REGEXP_SYMBOL_MATH:
            fprintf(output, "SYMBOL_MATH "); break;
        case XML_REGEXP_SYMBOL_CURRENCY:
            fprintf(output, "SYMBOL_CURRENCY "); break;
        case XML_REGEXP_SYMBOL_MODIFIER:
            fprintf(output, "SYMBOL_MODIFIER "); break;
        case XML_REGEXP_SYMBOL_OTHERS:
            fprintf(output, "SYMBOL_OTHERS "); break;
        case XML_REGEXP_OTHER:
            fprintf(output, "OTHER "); break;
        case XML_REGEXP_OTHER_CONTROL:
            fprintf(output, "OTHER_CONTROL "); break;
        case XML_REGEXP_OTHER_FORMAT:
            fprintf(output, "OTHER_FORMAT "); break;
        case XML_REGEXP_OTHER_PRIVATE:
            fprintf(output, "OTHER_PRIVATE "); break;
        case XML_REGEXP_OTHER_NA:
            fprintf(output, "OTHER_NA "); break;
        case XML_REGEXP_BLOCK_NAME:
	    fprintf(output, "BLOCK "); break;
    }
}

static void
xmlRegPrintQuantType(FILE *output, xmlRegQuantType type) {
    switch (type) {
        case XML_REGEXP_QUANT_EPSILON:
	    fprintf(output, "epsilon "); break;
        case XML_REGEXP_QUANT_ONCE:
	    fprintf(output, "once "); break;
        case XML_REGEXP_QUANT_OPT:
	    fprintf(output, "? "); break;
        case XML_REGEXP_QUANT_MULT:
	    fprintf(output, "* "); break;
        case XML_REGEXP_QUANT_PLUS:
	    fprintf(output, "+ "); break;
	case XML_REGEXP_QUANT_RANGE:
	    fprintf(output, "range "); break;
	case XML_REGEXP_QUANT_ONCEONLY:
	    fprintf(output, "onceonly "); break;
	case XML_REGEXP_QUANT_ALL:
	    fprintf(output, "all "); break;
    }
}
static void
xmlRegPrintRange(FILE *output, xmlRegRangePtr range) {
    fprintf(output, "  range: ");
    if (range->neg)
	fprintf(output, "negative ");
    xmlRegPrintAtomType(output, range->type);
    fprintf(output, "%c - %c\n", range->start, range->end);
}

static void
xmlRegPrintAtom(FILE *output, xmlRegAtomPtr atom) {
    fprintf(output, " atom: ");
    if (atom == NULL) {
	fprintf(output, "NULL\n");
	return;
    }
    if (atom->neg)
        fprintf(output, "not ");
    xmlRegPrintAtomType(output, atom->type);
    xmlRegPrintQuantType(output, atom->quant);
    if (atom->quant == XML_REGEXP_QUANT_RANGE)
	fprintf(output, "%d-%d ", atom->min, atom->max);
    if (atom->type == XML_REGEXP_STRING)
	fprintf(output, "'%s' ", (char *) atom->valuep);
    if (atom->type == XML_REGEXP_CHARVAL)
	fprintf(output, "char %c\n", atom->codepoint);
    else if (atom->type == XML_REGEXP_RANGES) {
	int i;
	fprintf(output, "%d entries\n", atom->nbRanges);
	for (i = 0; i < atom->nbRanges;i++)
	    xmlRegPrintRange(output, atom->ranges[i]);
    } else {
	fprintf(output, "\n");
    }
}

static void
xmlRegPrintAtomCompact(FILE* output, xmlRegexpPtr regexp, int atom)
{
    if (output == NULL || regexp == NULL || atom < 0 || 
        atom >= regexp->nbstrings) {
        return;
    }
    fprintf(output, " atom: ");

    xmlRegPrintAtomType(output, XML_REGEXP_STRING);
    xmlRegPrintQuantType(output, XML_REGEXP_QUANT_ONCE);
    fprintf(output, "'%s' ", (char *) regexp->stringMap[atom]);
    fprintf(output, "\n");
}

static void
xmlRegPrintTrans(FILE *output, xmlRegTransPtr trans) {
    fprintf(output, "  trans: ");
    if (trans == NULL) {
	fprintf(output, "NULL\n");
	return;
    }
    if (trans->to < 0) {
	fprintf(output, "removed\n");
	return;
    }
    if (trans->nd != 0) {
	if (trans->nd == 2)
	    fprintf(output, "last not determinist, ");
	else
	    fprintf(output, "not determinist, ");
    }
    if (trans->counter >= 0) {
	fprintf(output, "counted %d, ", trans->counter);
    }
    if (trans->count == REGEXP_ALL_COUNTER) {
	fprintf(output, "all transition, ");
    } else if (trans->count >= 0) {
	fprintf(output, "count based %d, ", trans->count);
    }
    if (trans->atom == NULL) {
	fprintf(output, "epsilon to %d\n", trans->to);
	return;
    }
    if (trans->atom->type == XML_REGEXP_CHARVAL)
	fprintf(output, "char %c ", trans->atom->codepoint);
    fprintf(output, "atom %d, to %d\n", trans->atom->no, trans->to);
}

static void
xmlRegPrintTransCompact(
    FILE* output,
    xmlRegexpPtr regexp,
    int state,
    int atom
)
{
    int target;
    if (output == NULL || regexp == NULL || regexp->compact == NULL || 
        state < 0 || atom < 0) {
        return;
    }
    target = regexp->compact[state * (regexp->nbstrings + 1) + atom + 1];
    fprintf(output, "  trans: ");

    /* TODO maybe skip 'removed' transitions, because they actually never existed */
    if (target < 0) {
        fprintf(output, "removed\n");
        return;
    }

    /* We will ignore most of the attributes used in xmlRegPrintTrans,
     * since the compact form is much simpler and uses only a part of the 
     * features provided by the libxml2 regexp libary 
     * (no rollbacks, counters etc.) */

    /* Compared to the standard representation, an automata written using the
     * compact form will ALWAYS be deterministic! 
     * From    xmlRegPrintTrans:
         if (trans->nd != 0) {
            ...
      * trans->nd will always be 0! */

    /* In automata represented in compact form, the transitions will not use
     * counters. 
     * From    xmlRegPrintTrans:
         if (trans->counter >= 0) {
            ...
     * regexp->counters == NULL, so trans->counter < 0 */

    /* In compact form, we won't use */

    /* An automata in the compact representation will always use string 
     * atoms. 
     * From    xmlRegPrintTrans:
         if (trans->atom->type == XML_REGEXP_CHARVAL)
             ...
     * trans->atom != NULL && trans->atom->type == XML_REGEXP_STRING */

    fprintf(output, "atom %d, to %d\n", atom, target);
}

static void
xmlRegPrintState(FILE *output, xmlRegStatePtr state) {
    int i;

    fprintf(output, " state: ");
    if (state == NULL) {
	fprintf(output, "NULL\n");
	return;
    }
    if (state->type == XML_REGEXP_START_STATE)
	fprintf(output, "START ");
    if (state->type == XML_REGEXP_FINAL_STATE)
	fprintf(output, "FINAL ");

    fprintf(output, "%d, %d transitions:\n", state->no, state->nbTrans);
    for (i = 0;i < state->nbTrans; i++) {
	xmlRegPrintTrans(output, &(state->trans[i]));
    }
}

static void
xmlRegPrintStateCompact(FILE* output, xmlRegexpPtr regexp, int state)
{
    int nbTrans = 0;
    int i;
    int target;
    xmlRegStateType stateType;

    if (output == NULL || regexp == NULL || regexp->compact == NULL ||
        state < 0) {
        return;
    }
    
    fprintf(output, " state: ");

    stateType = regexp->compact[state * (regexp->nbstrings + 1)];
    if (stateType == XML_REGEXP_START_STATE) {
        fprintf(output, " START ");
    }
    
    if (stateType == XML_REGEXP_FINAL_STATE) {
        fprintf(output, " FINAL ");
    }

    /* Print all atoms. */
    for (i = 0; i < regexp->nbstrings; i++) {
        xmlRegPrintAtomCompact(output, regexp, i);
    }

    /* Count all the transitions from the compact representation. */
    for (i = 0; i < regexp->nbstrings; i++) {
        target = regexp->compact[state * (regexp->nbstrings + 1) + i + 1];
        if (target > 0 && target <= regexp->nbstates && 
            regexp->compact[(target - 1) * (regexp->nbstrings + 1)] == 
            XML_REGEXP_SINK_STATE) {
                nbTrans++;
            }
    }

    fprintf(output, "%d, %d transitions:\n", state, nbTrans);
    
    /* Print all transitions */
    for (i = 0; i < regexp->nbstrings; i++) {
        xmlRegPrintTransCompact(output, regexp, state, i);
    }
}

/*
 * @param output  an output stream
 * @param regexp  the regexp instance
 * 
 * Print the compact representation of a regexp, in the same fashion as the
 * public #xmlRegexpPrint function.
 */
static void
xmlRegPrintCompact(FILE* output, xmlRegexpPtr regexp)
{
    int i;
    if (output == NULL || regexp == NULL || regexp->compact == NULL) {
        return;
    }
    
    fprintf(output, "'%s' ", regexp->string);

    fprintf(output, "%d atoms:\n", regexp->nbstrings);
    fprintf(output, "\n");
    for (i = 0; i < regexp->nbstrings; i++) {
        fprintf(output, " %02d ", i);
        xmlRegPrintAtomCompact(output, regexp, i);
    }

    fprintf(output, "%d states:", regexp->nbstates);
    fprintf(output, "\n");
    for (i = 0; i < regexp->nbstates; i++) {
        xmlRegPrintStateCompact(output, regexp, i);
    }

    fprintf(output, "%d counters:\n", 0);
}

static void
xmlRegexpPrintInternal(FILE *output, xmlRegexpPtr regexp) {
    int i;

    if (output == NULL)
        return;
    fprintf(output, " regexp: ");
    if (regexp == NULL) {
	fprintf(output, "NULL\n");
	return;
    }
	if (regexp->compact) {
		xmlRegPrintCompact(output, regexp);
		return;
	}

    fprintf(output, "'%s' ", regexp->string);
    fprintf(output, "\n");
    fprintf(output, "%d atoms:\n", regexp->nbAtoms);
    for (i = 0;i < regexp->nbAtoms; i++) {
	fprintf(output, " %02d ", i);
	xmlRegPrintAtom(output, regexp->atoms[i]);
    }
    fprintf(output, "%d states:", regexp->nbStates);
    fprintf(output, "\n");
    for (i = 0;i < regexp->nbStates; i++) {
	xmlRegPrintState(output, regexp->states[i]);
    }
    fprintf(output, "%d counters:\n", regexp->nbCounters);
    for (i = 0;i < regexp->nbCounters; i++) {
	fprintf(output, " %d: min %d max %d\n", i, regexp->counters[i].min,
		                                regexp->counters[i].max);
    }
}
#endif /* DEBUG_REGEXP */

/************************************************************************
 *									*
 *		 Finite Automata structures manipulations		*
 *									*
 ************************************************************************/

static xmlRegRangePtr
xmlRegAtomAddRange(xmlRegParserCtxtPtr ctxt, xmlRegAtomPtr atom,
	           int neg, xmlRegAtomType type, int start, int end,
		   xmlChar *blockName) {
    xmlRegRangePtr range;

    if (atom == NULL) {
	ERROR("add range: atom is NULL");
	return(NULL);
    }
    if (atom->type != XML_REGEXP_RANGES) {
	ERROR("add range: atom is not ranges");
	return(NULL);
    }
    if (atom->nbRanges >= atom->maxRanges) {
	xmlRegRangePtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(atom->maxRanges, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
        if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return(NULL);
        }
	tmp = xmlRealloc(atom->ranges, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return(NULL);
	}
	atom->ranges = tmp;
	atom->maxRanges = newSize;
    }
    range = xmlRegNewRange(ctxt, neg, type, start, end);
    if (range == NULL)
	return(NULL);
    range->blockName = blockName;
    atom->ranges[atom->nbRanges++] = range;

    return(range);
}

static int
xmlRegGetCounter(xmlRegParserCtxtPtr ctxt) {
    if (ctxt->nbCounters >= ctxt->maxCounters) {
	xmlRegCounter *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->maxCounters, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
	if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return(-1);
	}
	tmp = xmlRealloc(ctxt->counters, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return(-1);
	}
	ctxt->counters = tmp;
	ctxt->maxCounters = newSize;
    }
    ctxt->counters[ctxt->nbCounters].min = -1;
    ctxt->counters[ctxt->nbCounters].max = -1;
    return(ctxt->nbCounters++);
}

static int
xmlRegAtomPush(xmlRegParserCtxtPtr ctxt, xmlRegAtomPtr atom) {
    if (atom == NULL) {
	ERROR("atom push: atom is NULL");
	return(-1);
    }
    if (ctxt->nbAtoms >= ctxt->maxAtoms) {
	xmlRegAtomPtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->maxAtoms, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
	if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return(-1);
	}
	tmp = xmlRealloc(ctxt->atoms, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return(-1);
	}
	ctxt->atoms = tmp;
        ctxt->maxAtoms = newSize;
    }
    atom->no = ctxt->nbAtoms;
    ctxt->atoms[ctxt->nbAtoms++] = atom;
    return(0);
}

static void
xmlRegStateAddTransTo(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr target,
                      int from) {
    if (target->nbTransTo >= target->maxTransTo) {
	int *tmp;
        int newSize;

        newSize = xmlGrowCapacity(target->maxTransTo, sizeof(tmp[0]),
                                  8, XML_MAX_ITEMS);
	if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return;
	}
	tmp = xmlRealloc(target->transTo, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return;
	}
	target->transTo = tmp;
	target->maxTransTo = newSize;
    }
    target->transTo[target->nbTransTo] = from;
    target->nbTransTo++;
}

static void
xmlRegStateAddTrans(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr state,
	            xmlRegAtomPtr atom, xmlRegStatePtr target,
		    int counter, int count) {

    int nrtrans;

    if (state == NULL) {
	ERROR("add state: state is NULL");
	return;
    }
    if (target == NULL) {
	ERROR("add state: target is NULL");
	return;
    }
    /*
     * Other routines follow the philosophy 'When in doubt, add a transition'
     * so we check here whether such a transition is already present and, if
     * so, silently ignore this request.
     */

    for (nrtrans = state->nbTrans - 1; nrtrans >= 0; nrtrans--) {
	xmlRegTransPtr trans = &(state->trans[nrtrans]);
	if ((trans->atom == atom) &&
	    (trans->to == target->no) &&
	    (trans->counter == counter) &&
	    (trans->count == count)) {
	    return;
	}
    }

    if (state->nbTrans >= state->maxTrans) {
	xmlRegTrans *tmp;
        int newSize;

        newSize = xmlGrowCapacity(state->maxTrans, sizeof(tmp[0]),
                                  8, XML_MAX_ITEMS);
	if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return;
	}
	tmp = xmlRealloc(state->trans, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return;
	}
	state->trans = tmp;
	state->maxTrans = newSize;
    }

    state->trans[state->nbTrans].atom = atom;
    state->trans[state->nbTrans].to = target->no;
    state->trans[state->nbTrans].counter = counter;
    state->trans[state->nbTrans].count = count;
    state->trans[state->nbTrans].nd = 0;
    state->nbTrans++;
    xmlRegStateAddTransTo(ctxt, target, state->no);
}

static xmlRegStatePtr
xmlRegStatePush(xmlRegParserCtxtPtr ctxt) {
    xmlRegStatePtr state;

    if (ctxt->nbStates >= ctxt->maxStates) {
	xmlRegStatePtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->maxStates, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
	if (newSize < 0) {
	    xmlRegexpErrMemory(ctxt);
	    return(NULL);
	}
	tmp = xmlRealloc(ctxt->states, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
	    xmlRegexpErrMemory(ctxt);
	    return(NULL);
	}
	ctxt->states = tmp;
	ctxt->maxStates = newSize;
    }

    state = xmlRegNewState(ctxt);
    if (state == NULL)
        return(NULL);

    state->no = ctxt->nbStates;
    ctxt->states[ctxt->nbStates++] = state;

    return(state);
}

/**
 * @param ctxt  a regexp parser context
 * @param from  the from state
 * @param to  the target state or NULL for building a new one
 * @param lax  
 */
static int
xmlFAGenerateAllTransition(xmlRegParserCtxtPtr ctxt,
			   xmlRegStatePtr from, xmlRegStatePtr to,
			   int lax) {
    if (to == NULL) {
	to = xmlRegStatePush(ctxt);
        if (to == NULL)
            return(-1);
	ctxt->state = to;
    }
    if (lax)
	xmlRegStateAddTrans(ctxt, from, NULL, to, -1, REGEXP_ALL_LAX_COUNTER);
    else
	xmlRegStateAddTrans(ctxt, from, NULL, to, -1, REGEXP_ALL_COUNTER);
    return(0);
}

/**
 * @param ctxt  a regexp parser context
 * @param from  the from state
 * @param to  the target state or NULL for building a new one
 */
static int
xmlFAGenerateEpsilonTransition(xmlRegParserCtxtPtr ctxt,
			       xmlRegStatePtr from, xmlRegStatePtr to) {
    if (to == NULL) {
	to = xmlRegStatePush(ctxt);
        if (to == NULL)
            return(-1);
	ctxt->state = to;
    }
    xmlRegStateAddTrans(ctxt, from, NULL, to, -1, -1);
    return(0);
}

/**
 * @param ctxt  a regexp parser context
 * @param from  the from state
 * @param to  the target state or NULL for building a new one
 * @param counter  the counter for that transition
 */
static int
xmlFAGenerateCountedEpsilonTransition(xmlRegParserCtxtPtr ctxt,
	    xmlRegStatePtr from, xmlRegStatePtr to, int counter) {
    if (to == NULL) {
	to = xmlRegStatePush(ctxt);
        if (to == NULL)
            return(-1);
	ctxt->state = to;
    }
    xmlRegStateAddTrans(ctxt, from, NULL, to, counter, -1);
    return(0);
}

/**
 * @param ctxt  a regexp parser context
 * @param from  the from state
 * @param to  the target state or NULL for building a new one
 * @param counter  the counter for that transition
 */
static int
xmlFAGenerateCountedTransition(xmlRegParserCtxtPtr ctxt,
	    xmlRegStatePtr from, xmlRegStatePtr to, int counter) {
    if (to == NULL) {
	to = xmlRegStatePush(ctxt);
        if (to == NULL)
            return(-1);
	ctxt->state = to;
    }
    xmlRegStateAddTrans(ctxt, from, NULL, to, -1, counter);
    return(0);
}

/**
 * @param ctxt  a regexp parser context
 * @param from  the from state
 * @param to  the target state or NULL for building a new one
 * @param atom  the atom generating the transition
 * @returns 0 if success and -1 in case of error.
 */
static int
xmlFAGenerateTransitions(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr from,
	                 xmlRegStatePtr to, xmlRegAtomPtr atom) {
    xmlRegStatePtr end;
    int nullable = 0;

    if (atom == NULL) {
	ERROR("generate transition: atom == NULL");
	return(-1);
    }
    if (atom->type == XML_REGEXP_SUBREG) {
	/*
	 * this is a subexpression handling one should not need to
	 * create a new node except for XML_REGEXP_QUANT_RANGE.
	 */
	if ((to != NULL) && (atom->stop != to) &&
	    (atom->quant != XML_REGEXP_QUANT_RANGE)) {
	    /*
	     * Generate an epsilon transition to link to the target
	     */
	    xmlFAGenerateEpsilonTransition(ctxt, atom->stop, to);
#ifdef DV
	} else if ((to == NULL) && (atom->quant != XML_REGEXP_QUANT_RANGE) &&
		   (atom->quant != XML_REGEXP_QUANT_ONCE)) {
	    to = xmlRegStatePush(ctxt, to);
            if (to == NULL)
                return(-1);
	    ctxt->state = to;
	    xmlFAGenerateEpsilonTransition(ctxt, atom->stop, to);
#endif
	}
	switch (atom->quant) {
	    case XML_REGEXP_QUANT_OPT:
		atom->quant = XML_REGEXP_QUANT_ONCE;
		/*
		 * transition done to the state after end of atom.
		 *      1. set transition from atom start to new state
		 *      2. set transition from atom end to this state.
		 */
                if (to == NULL) {
                    xmlFAGenerateEpsilonTransition(ctxt, atom->start, 0);
                    xmlFAGenerateEpsilonTransition(ctxt, atom->stop,
                                                   ctxt->state);
                } else {
                    xmlFAGenerateEpsilonTransition(ctxt, atom->start, to);
                }
		break;
	    case XML_REGEXP_QUANT_MULT:
		atom->quant = XML_REGEXP_QUANT_ONCE;
		xmlFAGenerateEpsilonTransition(ctxt, atom->start, atom->stop);
		xmlFAGenerateEpsilonTransition(ctxt, atom->stop, atom->start);
		break;
	    case XML_REGEXP_QUANT_PLUS:
		atom->quant = XML_REGEXP_QUANT_ONCE;
		xmlFAGenerateEpsilonTransition(ctxt, atom->stop, atom->start);
		break;
	    case XML_REGEXP_QUANT_RANGE: {
		int counter;
		xmlRegStatePtr inter, newstate;

		/*
		 * create the final state now if needed
		 */
		if (to != NULL) {
		    newstate = to;
		} else {
		    newstate = xmlRegStatePush(ctxt);
                    if (newstate == NULL)
                        return(-1);
		}

		/*
		 * The principle here is to use counted transition
		 * to avoid explosion in the number of states in the
		 * graph. This is clearly more complex but should not
		 * be exploitable at runtime.
		 */
		if ((atom->min == 0) && (atom->start0 == NULL)) {
		    xmlRegAtomPtr copy;
		    /*
		     * duplicate a transition based on atom to count next
		     * occurrences after 1. We cannot loop to atom->start
		     * directly because we need an epsilon transition to
		     * newstate.
		     */
		     /* ???? For some reason it seems we never reach that
		        case, I suppose this got optimized out before when
			building the automata */
		    copy = xmlRegCopyAtom(ctxt, atom);
		    if (copy == NULL)
		        return(-1);
		    copy->quant = XML_REGEXP_QUANT_ONCE;
		    copy->min = 0;
		    copy->max = 0;

		    if (xmlFAGenerateTransitions(ctxt, atom->start, NULL, copy)
		        < 0) {
                        xmlRegFreeAtom(copy);
			return(-1);
                    }
		    inter = ctxt->state;
		    counter = xmlRegGetCounter(ctxt);
                    if (counter < 0)
                        return(-1);
		    ctxt->counters[counter].min = atom->min - 1;
		    ctxt->counters[counter].max = atom->max - 1;
		    /* count the number of times we see it again */
		    xmlFAGenerateCountedEpsilonTransition(ctxt, inter,
						   atom->stop, counter);
		    /* allow a way out based on the count */
		    xmlFAGenerateCountedTransition(ctxt, inter,
			                           newstate, counter);
		    /* and also allow a direct exit for 0 */
		    xmlFAGenerateEpsilonTransition(ctxt, atom->start,
		                                   newstate);
		} else {
		    /*
		     * either we need the atom at least once or there
		     * is an atom->start0 allowing to easily plug the
		     * epsilon transition.
		     */
		    counter = xmlRegGetCounter(ctxt);
                    if (counter < 0)
                        return(-1);
		    ctxt->counters[counter].min = atom->min - 1;
		    ctxt->counters[counter].max = atom->max - 1;
		    /* allow a way out based on the count */
		    xmlFAGenerateCountedTransition(ctxt, atom->stop,
			                           newstate, counter);
		    /* count the number of times we see it again */
		    xmlFAGenerateCountedEpsilonTransition(ctxt, atom->stop,
						   atom->start, counter);
		    /* and if needed allow a direct exit for 0 */
		    if (atom->min == 0)
			xmlFAGenerateEpsilonTransition(ctxt, atom->start0,
						       newstate);

		}
		atom->min = 0;
		atom->max = 0;
		atom->quant = XML_REGEXP_QUANT_ONCE;
		ctxt->state = newstate;
	    }
	    default:
		break;
	}
        atom->start = NULL;
        atom->start0 = NULL;
        atom->stop = NULL;
	if (xmlRegAtomPush(ctxt, atom) < 0)
	    return(-1);
	return(0);
    }
    if ((atom->min == 0) && (atom->max == 0) &&
               (atom->quant == XML_REGEXP_QUANT_RANGE)) {
        /*
	 * we can discard the atom and generate an epsilon transition instead
	 */
	if (to == NULL) {
	    to = xmlRegStatePush(ctxt);
	    if (to == NULL)
		return(-1);
	}
	xmlFAGenerateEpsilonTransition(ctxt, from, to);
	ctxt->state = to;
	xmlRegFreeAtom(atom);
	return(0);
    }
    if (to == NULL) {
	to = xmlRegStatePush(ctxt);
	if (to == NULL)
	    return(-1);
    }
    end = to;
    if ((atom->quant == XML_REGEXP_QUANT_MULT) ||
        (atom->quant == XML_REGEXP_QUANT_PLUS)) {
	/*
	 * Do not pollute the target state by adding transitions from
	 * it as it is likely to be the shared target of multiple branches.
	 * So isolate with an epsilon transition.
	 */
        xmlRegStatePtr tmp;

	tmp = xmlRegStatePush(ctxt);
        if (tmp == NULL)
	    return(-1);
	xmlFAGenerateEpsilonTransition(ctxt, tmp, to);
	to = tmp;
    }
    if ((atom->quant == XML_REGEXP_QUANT_RANGE) &&
        (atom->min == 0) && (atom->max > 0)) {
	nullable = 1;
	atom->min = 1;
        if (atom->max == 1)
	    atom->quant = XML_REGEXP_QUANT_OPT;
    }
    xmlRegStateAddTrans(ctxt, from, atom, to, -1, -1);
    ctxt->state = end;
    switch (atom->quant) {
	case XML_REGEXP_QUANT_OPT:
	    atom->quant = XML_REGEXP_QUANT_ONCE;
	    xmlFAGenerateEpsilonTransition(ctxt, from, to);
	    break;
	case XML_REGEXP_QUANT_MULT:
	    atom->quant = XML_REGEXP_QUANT_ONCE;
	    xmlFAGenerateEpsilonTransition(ctxt, from, to);
	    xmlRegStateAddTrans(ctxt, to, atom, to, -1, -1);
	    break;
	case XML_REGEXP_QUANT_PLUS:
	    atom->quant = XML_REGEXP_QUANT_ONCE;
	    xmlRegStateAddTrans(ctxt, to, atom, to, -1, -1);
	    break;
	case XML_REGEXP_QUANT_RANGE:
	    if (nullable)
		xmlFAGenerateEpsilonTransition(ctxt, from, to);
	    break;
	default:
	    break;
    }
    if (xmlRegAtomPush(ctxt, atom) < 0)
	return(-1);
    return(0);
}

/**
 * @param ctxt  a regexp parser context
 * @param fromnr  the from state
 * @param tonr  the to state
 * @param counter  should that transition be associated to a counted
 */
static void
xmlFAReduceEpsilonTransitions(xmlRegParserCtxtPtr ctxt, int fromnr,
	                      int tonr, int counter) {
    int transnr;
    xmlRegStatePtr from;
    xmlRegStatePtr to;

    from = ctxt->states[fromnr];
    if (from == NULL)
	return;
    to = ctxt->states[tonr];
    if (to == NULL)
	return;
    if ((to->mark == XML_REGEXP_MARK_START) ||
	(to->mark == XML_REGEXP_MARK_VISITED))
	return;

    to->mark = XML_REGEXP_MARK_VISITED;
    if (to->type == XML_REGEXP_FINAL_STATE) {
	from->type = XML_REGEXP_FINAL_STATE;
    }
    for (transnr = 0;transnr < to->nbTrans;transnr++) {
        xmlRegTransPtr t1 = &to->trans[transnr];
        int tcounter;

        if (t1->to < 0)
	    continue;
        if (t1->counter >= 0) {
            /* assert(counter < 0); */
            tcounter = t1->counter;
        } else {
            tcounter = counter;
        }
	if (t1->atom == NULL) {
	    /*
	     * Don't remove counted transitions
	     * Don't loop either
	     */
	    if (t1->to != fromnr) {
		if (t1->count >= 0) {
		    xmlRegStateAddTrans(ctxt, from, NULL, ctxt->states[t1->to],
					-1, t1->count);
		} else {
                    xmlFAReduceEpsilonTransitions(ctxt, fromnr, t1->to,
                                                  tcounter);
		}
	    }
	} else {
            xmlRegStateAddTrans(ctxt, from, t1->atom,
                                ctxt->states[t1->to], tcounter, -1);
	}
    }
}

/**
 * @param ctxt  a regexp parser context
 * @param tonr  the to state
 */
static void
xmlFAFinishReduceEpsilonTransitions(xmlRegParserCtxtPtr ctxt, int tonr) {
    int transnr;
    xmlRegStatePtr to;

    to = ctxt->states[tonr];
    if (to == NULL)
	return;
    if ((to->mark == XML_REGEXP_MARK_START) ||
	(to->mark == XML_REGEXP_MARK_NORMAL))
	return;

    to->mark = XML_REGEXP_MARK_NORMAL;
    for (transnr = 0;transnr < to->nbTrans;transnr++) {
	xmlRegTransPtr t1 = &to->trans[transnr];
	if ((t1->to >= 0) && (t1->atom == NULL))
            xmlFAFinishReduceEpsilonTransitions(ctxt, t1->to);
    }
}

/**
 * Eliminating general epsilon transitions can get costly in the general
 * algorithm due to the large amount of generated new transitions and
 * associated comparisons. However for simple epsilon transition used just
 * to separate building blocks when generating the automata this can be
 * reduced to state elimination:
 *    - if there exists an epsilon from X to Y
 *    - if there is no other transition from X
 * then X and Y are semantically equivalent and X can be eliminated
 * If X is the start state then make Y the start state, else replace the
 * target of all transitions to X by transitions to Y.
 *
 * If X is a final state, skip it.
 * Otherwise it would be necessary to manipulate counters for this case when
 * eliminating state 2:
 * State 1 has a transition with an atom to state 2.
 * State 2 is final and has an epsilon transition to state 1.
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAEliminateSimpleEpsilonTransitions(xmlRegParserCtxtPtr ctxt) {
    int statenr, i, j, newto;
    xmlRegStatePtr state, tmp;

    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if (state == NULL)
	    continue;
	if (state->nbTrans != 1)
	    continue;
       if (state->type == XML_REGEXP_UNREACH_STATE ||
           state->type == XML_REGEXP_FINAL_STATE)
	    continue;
	/* is the only transition out a basic transition */
	if ((state->trans[0].atom == NULL) &&
	    (state->trans[0].to >= 0) &&
	    (state->trans[0].to != statenr) &&
	    (state->trans[0].counter < 0) &&
	    (state->trans[0].count < 0)) {
	    newto = state->trans[0].to;

            if (state->type == XML_REGEXP_START_STATE) {
            } else {
	        for (i = 0;i < state->nbTransTo;i++) {
		    tmp = ctxt->states[state->transTo[i]];
		    for (j = 0;j < tmp->nbTrans;j++) {
			if (tmp->trans[j].to == statenr) {
			    tmp->trans[j].to = -1;
			    xmlRegStateAddTrans(ctxt, tmp, tmp->trans[j].atom,
						ctxt->states[newto],
					        tmp->trans[j].counter,
						tmp->trans[j].count);
			}
		    }
		}
		if (state->type == XML_REGEXP_FINAL_STATE)
		    ctxt->states[newto]->type = XML_REGEXP_FINAL_STATE;
		/* eliminate the transition completely */
		state->nbTrans = 0;

                state->type = XML_REGEXP_UNREACH_STATE;

	    }

	}
    }
}
/**
 * @param ctxt  a regexp parser context
 */
static void
xmlFAEliminateEpsilonTransitions(xmlRegParserCtxtPtr ctxt) {
    int statenr, transnr;
    xmlRegStatePtr state;
    int has_epsilon;

    if (ctxt->states == NULL) return;

    /*
     * Eliminate simple epsilon transition and the associated unreachable
     * states.
     */
    xmlFAEliminateSimpleEpsilonTransitions(ctxt);
    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if ((state != NULL) && (state->type == XML_REGEXP_UNREACH_STATE)) {
	    xmlRegFreeState(state);
	    ctxt->states[statenr] = NULL;
	}
    }

    has_epsilon = 0;

    /*
     * Build the completed transitions bypassing the epsilons
     * Use a marking algorithm to avoid loops
     * Mark sink states too.
     * Process from the latest states backward to the start when
     * there is long cascading epsilon chains this minimize the
     * recursions and transition compares when adding the new ones
     */
    for (statenr = ctxt->nbStates - 1;statenr >= 0;statenr--) {
	state = ctxt->states[statenr];
	if (state == NULL)
	    continue;
	if ((state->nbTrans == 0) &&
	    (state->type != XML_REGEXP_FINAL_STATE)) {
	    state->type = XML_REGEXP_SINK_STATE;
	}
	for (transnr = 0;transnr < state->nbTrans;transnr++) {
	    if ((state->trans[transnr].atom == NULL) &&
		(state->trans[transnr].to >= 0)) {
		if (state->trans[transnr].to == statenr) {
		    state->trans[transnr].to = -1;
		} else if (state->trans[transnr].count < 0) {
		    int newto = state->trans[transnr].to;

		    has_epsilon = 1;
		    state->trans[transnr].to = -2;
		    state->mark = XML_REGEXP_MARK_START;
		    xmlFAReduceEpsilonTransitions(ctxt, statenr,
				      newto, state->trans[transnr].counter);
		    xmlFAFinishReduceEpsilonTransitions(ctxt, newto);
		    state->mark = XML_REGEXP_MARK_NORMAL;
	        }
	    }
	}
    }
    /*
     * Eliminate the epsilon transitions
     */
    if (has_epsilon) {
	for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	    state = ctxt->states[statenr];
	    if (state == NULL)
		continue;
	    for (transnr = 0;transnr < state->nbTrans;transnr++) {
		xmlRegTransPtr trans = &(state->trans[transnr]);
		if ((trans->atom == NULL) &&
		    (trans->count < 0) &&
		    (trans->to >= 0)) {
		    trans->to = -1;
		}
	    }
	}
    }

    /*
     * Use this pass to detect unreachable states too
     */
    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if (state != NULL)
	    state->reached = XML_REGEXP_MARK_NORMAL;
    }
    state = ctxt->states[0];
    if (state != NULL)
	state->reached = XML_REGEXP_MARK_START;
    while (state != NULL) {
	xmlRegStatePtr target = NULL;
	state->reached = XML_REGEXP_MARK_VISITED;
	/*
	 * Mark all states reachable from the current reachable state
	 */
	for (transnr = 0;transnr < state->nbTrans;transnr++) {
	    if ((state->trans[transnr].to >= 0) &&
		((state->trans[transnr].atom != NULL) ||
		 (state->trans[transnr].count >= 0))) {
		int newto = state->trans[transnr].to;

		if (ctxt->states[newto] == NULL)
		    continue;
		if (ctxt->states[newto]->reached == XML_REGEXP_MARK_NORMAL) {
		    ctxt->states[newto]->reached = XML_REGEXP_MARK_START;
		    target = ctxt->states[newto];
		}
	    }
	}

	/*
	 * find the next accessible state not explored
	 */
	if (target == NULL) {
	    for (statenr = 1;statenr < ctxt->nbStates;statenr++) {
		state = ctxt->states[statenr];
		if ((state != NULL) && (state->reached ==
			XML_REGEXP_MARK_START)) {
		    target = state;
		    break;
		}
	    }
	}
	state = target;
    }
    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if ((state != NULL) && (state->reached == XML_REGEXP_MARK_NORMAL)) {
	    xmlRegFreeState(state);
	    ctxt->states[statenr] = NULL;
	}
    }

}

static int
xmlFACompareRanges(xmlRegRangePtr range1, xmlRegRangePtr range2) {
    int ret = 0;

    if ((range1->type == XML_REGEXP_RANGES) ||
        (range2->type == XML_REGEXP_RANGES) ||
        (range2->type == XML_REGEXP_SUBREG) ||
        (range1->type == XML_REGEXP_SUBREG) ||
        (range1->type == XML_REGEXP_STRING) ||
        (range2->type == XML_REGEXP_STRING))
	return(-1);

    /* put them in order */
    if (range1->type > range2->type) {
        xmlRegRangePtr tmp;

	tmp = range1;
	range1 = range2;
	range2 = tmp;
    }
    if ((range1->type == XML_REGEXP_ANYCHAR) ||
        (range2->type == XML_REGEXP_ANYCHAR)) {
	ret = 1;
    } else if ((range1->type == XML_REGEXP_EPSILON) ||
               (range2->type == XML_REGEXP_EPSILON)) {
	return(0);
    } else if (range1->type == range2->type) {
        if (range1->type != XML_REGEXP_CHARVAL)
            ret = 1;
        else if ((range1->end < range2->start) ||
	         (range2->end < range1->start))
	    ret = 0;
	else
	    ret = 1;
    } else if (range1->type == XML_REGEXP_CHARVAL) {
        int codepoint;
	int neg = 0;

	/*
	 * just check all codepoints in the range for acceptance,
	 * this is usually way cheaper since done only once at
	 * compilation than testing over and over at runtime or
	 * pushing too many states when evaluating.
	 */
	if (((range1->neg == 0) && (range2->neg != 0)) ||
	    ((range1->neg != 0) && (range2->neg == 0)))
	    neg = 1;

	for (codepoint = range1->start;codepoint <= range1->end ;codepoint++) {
	    ret = xmlRegCheckCharacterRange(range2->type, codepoint,
					    0, range2->start, range2->end,
					    range2->blockName);
	    if (ret < 0)
	        return(-1);
	    if (((neg == 1) && (ret == 0)) ||
	        ((neg == 0) && (ret == 1)))
		return(1);
	}
	return(0);
    } else if ((range1->type == XML_REGEXP_BLOCK_NAME) ||
               (range2->type == XML_REGEXP_BLOCK_NAME)) {
	if (range1->type == range2->type) {
	    ret = xmlStrEqual(range1->blockName, range2->blockName);
	} else {
	    /*
	     * comparing a block range with anything else is way
	     * too costly, and maintaining the table is like too much
	     * memory too, so let's force the automata to save state
	     * here.
	     */
	    return(1);
	}
    } else if ((range1->type < XML_REGEXP_LETTER) ||
               (range2->type < XML_REGEXP_LETTER)) {
	if ((range1->type == XML_REGEXP_ANYSPACE) &&
	    (range2->type == XML_REGEXP_NOTSPACE))
	    ret = 0;
	else if ((range1->type == XML_REGEXP_INITNAME) &&
	         (range2->type == XML_REGEXP_NOTINITNAME))
	    ret = 0;
	else if ((range1->type == XML_REGEXP_NAMECHAR) &&
	         (range2->type == XML_REGEXP_NOTNAMECHAR))
	    ret = 0;
	else if ((range1->type == XML_REGEXP_DECIMAL) &&
	         (range2->type == XML_REGEXP_NOTDECIMAL))
	    ret = 0;
	else if ((range1->type == XML_REGEXP_REALCHAR) &&
	         (range2->type == XML_REGEXP_NOTREALCHAR))
	    ret = 0;
	else {
	    /* same thing to limit complexity */
	    return(1);
	}
    } else {
        ret = 0;
        /* range1->type < range2->type here */
        switch (range1->type) {
	    case XML_REGEXP_LETTER:
	         /* all disjoint except in the subgroups */
	         if ((range2->type == XML_REGEXP_LETTER_UPPERCASE) ||
		     (range2->type == XML_REGEXP_LETTER_LOWERCASE) ||
		     (range2->type == XML_REGEXP_LETTER_TITLECASE) ||
		     (range2->type == XML_REGEXP_LETTER_MODIFIER) ||
		     (range2->type == XML_REGEXP_LETTER_OTHERS))
		     ret = 1;
		 break;
	    case XML_REGEXP_MARK:
	         if ((range2->type == XML_REGEXP_MARK_NONSPACING) ||
		     (range2->type == XML_REGEXP_MARK_SPACECOMBINING) ||
		     (range2->type == XML_REGEXP_MARK_ENCLOSING))
		     ret = 1;
		 break;
	    case XML_REGEXP_NUMBER:
	         if ((range2->type == XML_REGEXP_NUMBER_DECIMAL) ||
		     (range2->type == XML_REGEXP_NUMBER_LETTER) ||
		     (range2->type == XML_REGEXP_NUMBER_OTHERS))
		     ret = 1;
		 break;
	    case XML_REGEXP_PUNCT:
	         if ((range2->type == XML_REGEXP_PUNCT_CONNECTOR) ||
		     (range2->type == XML_REGEXP_PUNCT_DASH) ||
		     (range2->type == XML_REGEXP_PUNCT_OPEN) ||
		     (range2->type == XML_REGEXP_PUNCT_CLOSE) ||
		     (range2->type == XML_REGEXP_PUNCT_INITQUOTE) ||
		     (range2->type == XML_REGEXP_PUNCT_FINQUOTE) ||
		     (range2->type == XML_REGEXP_PUNCT_OTHERS))
		     ret = 1;
		 break;
	    case XML_REGEXP_SEPAR:
	         if ((range2->type == XML_REGEXP_SEPAR_SPACE) ||
		     (range2->type == XML_REGEXP_SEPAR_LINE) ||
		     (range2->type == XML_REGEXP_SEPAR_PARA))
		     ret = 1;
		 break;
	    case XML_REGEXP_SYMBOL:
	         if ((range2->type == XML_REGEXP_SYMBOL_MATH) ||
		     (range2->type == XML_REGEXP_SYMBOL_CURRENCY) ||
		     (range2->type == XML_REGEXP_SYMBOL_MODIFIER) ||
		     (range2->type == XML_REGEXP_SYMBOL_OTHERS))
		     ret = 1;
		 break;
	    case XML_REGEXP_OTHER:
	         if ((range2->type == XML_REGEXP_OTHER_CONTROL) ||
		     (range2->type == XML_REGEXP_OTHER_FORMAT) ||
		     (range2->type == XML_REGEXP_OTHER_PRIVATE))
		     ret = 1;
		 break;
            default:
	         if ((range2->type >= XML_REGEXP_LETTER) &&
		     (range2->type < XML_REGEXP_BLOCK_NAME))
		     ret = 0;
		 else {
		     /* safety net ! */
		     return(1);
		 }
	}
    }
    if (((range1->neg == 0) && (range2->neg != 0)) ||
        ((range1->neg != 0) && (range2->neg == 0)))
	ret = !ret;
    return(ret);
}

/**
 * Compares two atoms type to check whether they intersect in some ways,
 * this is used by xmlFACompareAtoms only
 *
 * @param type1  an atom type
 * @param type2  an atom type
 * @returns 1 if they may intersect and 0 otherwise
 */
static int
xmlFACompareAtomTypes(xmlRegAtomType type1, xmlRegAtomType type2) {
    if ((type1 == XML_REGEXP_EPSILON) ||
        (type1 == XML_REGEXP_CHARVAL) ||
	(type1 == XML_REGEXP_RANGES) ||
	(type1 == XML_REGEXP_SUBREG) ||
	(type1 == XML_REGEXP_STRING) ||
	(type1 == XML_REGEXP_ANYCHAR))
	return(1);
    if ((type2 == XML_REGEXP_EPSILON) ||
        (type2 == XML_REGEXP_CHARVAL) ||
	(type2 == XML_REGEXP_RANGES) ||
	(type2 == XML_REGEXP_SUBREG) ||
	(type2 == XML_REGEXP_STRING) ||
	(type2 == XML_REGEXP_ANYCHAR))
	return(1);

    if (type1 == type2) return(1);

    /* simplify subsequent compares by making sure type1 < type2 */
    if (type1 > type2) {
        xmlRegAtomType tmp = type1;
	type1 = type2;
	type2 = tmp;
    }
    switch (type1) {
        case XML_REGEXP_ANYSPACE: /* \s */
	    /* can't be a letter, number, mark, punctuation, symbol */
	    if ((type2 == XML_REGEXP_NOTSPACE) ||
		((type2 >= XML_REGEXP_LETTER) &&
		 (type2 <= XML_REGEXP_LETTER_OTHERS)) ||
	        ((type2 >= XML_REGEXP_NUMBER) &&
		 (type2 <= XML_REGEXP_NUMBER_OTHERS)) ||
	        ((type2 >= XML_REGEXP_MARK) &&
		 (type2 <= XML_REGEXP_MARK_ENCLOSING)) ||
	        ((type2 >= XML_REGEXP_PUNCT) &&
		 (type2 <= XML_REGEXP_PUNCT_OTHERS)) ||
	        ((type2 >= XML_REGEXP_SYMBOL) &&
		 (type2 <= XML_REGEXP_SYMBOL_OTHERS))
	        ) return(0);
	    break;
        case XML_REGEXP_NOTSPACE: /* \S */
	    break;
        case XML_REGEXP_INITNAME: /* \l */
	    /* can't be a number, mark, separator, punctuation, symbol or other */
	    if ((type2 == XML_REGEXP_NOTINITNAME) ||
	        ((type2 >= XML_REGEXP_NUMBER) &&
		 (type2 <= XML_REGEXP_NUMBER_OTHERS)) ||
	        ((type2 >= XML_REGEXP_MARK) &&
		 (type2 <= XML_REGEXP_MARK_ENCLOSING)) ||
	        ((type2 >= XML_REGEXP_SEPAR) &&
		 (type2 <= XML_REGEXP_SEPAR_PARA)) ||
	        ((type2 >= XML_REGEXP_PUNCT) &&
		 (type2 <= XML_REGEXP_PUNCT_OTHERS)) ||
	        ((type2 >= XML_REGEXP_SYMBOL) &&
		 (type2 <= XML_REGEXP_SYMBOL_OTHERS)) ||
	        ((type2 >= XML_REGEXP_OTHER) &&
		 (type2 <= XML_REGEXP_OTHER_NA))
		) return(0);
	    break;
        case XML_REGEXP_NOTINITNAME: /* \L */
	    break;
        case XML_REGEXP_NAMECHAR: /* \c */
	    /* can't be a mark, separator, punctuation, symbol or other */
	    if ((type2 == XML_REGEXP_NOTNAMECHAR) ||
	        ((type2 >= XML_REGEXP_MARK) &&
		 (type2 <= XML_REGEXP_MARK_ENCLOSING)) ||
	        ((type2 >= XML_REGEXP_PUNCT) &&
		 (type2 <= XML_REGEXP_PUNCT_OTHERS)) ||
	        ((type2 >= XML_REGEXP_SEPAR) &&
		 (type2 <= XML_REGEXP_SEPAR_PARA)) ||
	        ((type2 >= XML_REGEXP_SYMBOL) &&
		 (type2 <= XML_REGEXP_SYMBOL_OTHERS)) ||
	        ((type2 >= XML_REGEXP_OTHER) &&
		 (type2 <= XML_REGEXP_OTHER_NA))
		) return(0);
	    break;
        case XML_REGEXP_NOTNAMECHAR: /* \C */
	    break;
        case XML_REGEXP_DECIMAL: /* \d */
	    /* can't be a letter, mark, separator, punctuation, symbol or other */
	    if ((type2 == XML_REGEXP_NOTDECIMAL) ||
	        (type2 == XML_REGEXP_REALCHAR) ||
		((type2 >= XML_REGEXP_LETTER) &&
		 (type2 <= XML_REGEXP_LETTER_OTHERS)) ||
	        ((type2 >= XML_REGEXP_MARK) &&
		 (type2 <= XML_REGEXP_MARK_ENCLOSING)) ||
	        ((type2 >= XML_REGEXP_PUNCT) &&
		 (type2 <= XML_REGEXP_PUNCT_OTHERS)) ||
	        ((type2 >= XML_REGEXP_SEPAR) &&
		 (type2 <= XML_REGEXP_SEPAR_PARA)) ||
	        ((type2 >= XML_REGEXP_SYMBOL) &&
		 (type2 <= XML_REGEXP_SYMBOL_OTHERS)) ||
	        ((type2 >= XML_REGEXP_OTHER) &&
		 (type2 <= XML_REGEXP_OTHER_NA))
		)return(0);
	    break;
        case XML_REGEXP_NOTDECIMAL: /* \D */
	    break;
        case XML_REGEXP_REALCHAR: /* \w */
	    /* can't be a mark, separator, punctuation, symbol or other */
	    if ((type2 == XML_REGEXP_NOTDECIMAL) ||
	        ((type2 >= XML_REGEXP_MARK) &&
		 (type2 <= XML_REGEXP_MARK_ENCLOSING)) ||
	        ((type2 >= XML_REGEXP_PUNCT) &&
		 (type2 <= XML_REGEXP_PUNCT_OTHERS)) ||
	        ((type2 >= XML_REGEXP_SEPAR) &&
		 (type2 <= XML_REGEXP_SEPAR_PARA)) ||
	        ((type2 >= XML_REGEXP_SYMBOL) &&
		 (type2 <= XML_REGEXP_SYMBOL_OTHERS)) ||
	        ((type2 >= XML_REGEXP_OTHER) &&
		 (type2 <= XML_REGEXP_OTHER_NA))
		)return(0);
	    break;
        case XML_REGEXP_NOTREALCHAR: /* \W */
	    break;
	/*
	 * at that point we know both type 1 and type2 are from
	 * character categories are ordered and are different,
	 * it becomes simple because this is a partition
	 */
        case XML_REGEXP_LETTER:
	    if (type2 <= XML_REGEXP_LETTER_OTHERS)
	        return(1);
	    return(0);
        case XML_REGEXP_LETTER_UPPERCASE:
        case XML_REGEXP_LETTER_LOWERCASE:
        case XML_REGEXP_LETTER_TITLECASE:
        case XML_REGEXP_LETTER_MODIFIER:
        case XML_REGEXP_LETTER_OTHERS:
	    return(0);
        case XML_REGEXP_MARK:
	    if (type2 <= XML_REGEXP_MARK_ENCLOSING)
	        return(1);
	    return(0);
        case XML_REGEXP_MARK_NONSPACING:
        case XML_REGEXP_MARK_SPACECOMBINING:
        case XML_REGEXP_MARK_ENCLOSING:
	    return(0);
        case XML_REGEXP_NUMBER:
	    if (type2 <= XML_REGEXP_NUMBER_OTHERS)
	        return(1);
	    return(0);
        case XML_REGEXP_NUMBER_DECIMAL:
        case XML_REGEXP_NUMBER_LETTER:
        case XML_REGEXP_NUMBER_OTHERS:
	    return(0);
        case XML_REGEXP_PUNCT:
	    if (type2 <= XML_REGEXP_PUNCT_OTHERS)
	        return(1);
	    return(0);
        case XML_REGEXP_PUNCT_CONNECTOR:
        case XML_REGEXP_PUNCT_DASH:
        case XML_REGEXP_PUNCT_OPEN:
        case XML_REGEXP_PUNCT_CLOSE:
        case XML_REGEXP_PUNCT_INITQUOTE:
        case XML_REGEXP_PUNCT_FINQUOTE:
        case XML_REGEXP_PUNCT_OTHERS:
	    return(0);
        case XML_REGEXP_SEPAR:
	    if (type2 <= XML_REGEXP_SEPAR_PARA)
	        return(1);
	    return(0);
        case XML_REGEXP_SEPAR_SPACE:
        case XML_REGEXP_SEPAR_LINE:
        case XML_REGEXP_SEPAR_PARA:
	    return(0);
        case XML_REGEXP_SYMBOL:
	    if (type2 <= XML_REGEXP_SYMBOL_OTHERS)
	        return(1);
	    return(0);
        case XML_REGEXP_SYMBOL_MATH:
        case XML_REGEXP_SYMBOL_CURRENCY:
        case XML_REGEXP_SYMBOL_MODIFIER:
        case XML_REGEXP_SYMBOL_OTHERS:
	    return(0);
        case XML_REGEXP_OTHER:
	    if (type2 <= XML_REGEXP_OTHER_NA)
	        return(1);
	    return(0);
        case XML_REGEXP_OTHER_CONTROL:
        case XML_REGEXP_OTHER_FORMAT:
        case XML_REGEXP_OTHER_PRIVATE:
        case XML_REGEXP_OTHER_NA:
	    return(0);
	default:
	    break;
    }
    return(1);
}

/**
 * Compares two atoms to check whether they are the same exactly
 * this is used to remove equivalent transitions
 *
 * @param atom1  an atom
 * @param atom2  an atom
 * @param deep  if not set only compare string pointers
 * @returns 1 if same and 0 otherwise
 */
static int
xmlFAEqualAtoms(xmlRegAtomPtr atom1, xmlRegAtomPtr atom2, int deep) {
    int ret = 0;

    if (atom1 == atom2)
	return(1);
    if ((atom1 == NULL) || (atom2 == NULL))
	return(0);

    if (atom1->type != atom2->type)
        return(0);
    switch (atom1->type) {
        case XML_REGEXP_EPSILON:
	    ret = 0;
	    break;
        case XML_REGEXP_STRING:
            if (!deep)
                ret = (atom1->valuep == atom2->valuep);
            else
                ret = xmlStrEqual((xmlChar *)atom1->valuep,
                                  (xmlChar *)atom2->valuep);
	    break;
        case XML_REGEXP_CHARVAL:
	    ret = (atom1->codepoint == atom2->codepoint);
	    break;
	case XML_REGEXP_RANGES:
	    /* too hard to do in the general case */
	    ret = 0;
	default:
	    break;
    }
    return(ret);
}

/**
 * Compares two atoms to check whether they intersect in some ways,
 * this is used by xmlFAComputesDeterminism and xmlFARecurseDeterminism only
 *
 * @param atom1  an atom
 * @param atom2  an atom
 * @param deep  if not set only compare string pointers
 * @returns 1 if yes and 0 otherwise
 */
static int
xmlFACompareAtoms(xmlRegAtomPtr atom1, xmlRegAtomPtr atom2, int deep) {
    int ret = 1;

    if (atom1 == atom2)
	return(1);
    if ((atom1 == NULL) || (atom2 == NULL))
	return(0);

    if ((atom1->type == XML_REGEXP_ANYCHAR) ||
        (atom2->type == XML_REGEXP_ANYCHAR))
	return(1);

    if (atom1->type > atom2->type) {
	xmlRegAtomPtr tmp;
	tmp = atom1;
	atom1 = atom2;
	atom2 = tmp;
    }
    if (atom1->type != atom2->type) {
        ret = xmlFACompareAtomTypes(atom1->type, atom2->type);
	/* if they can't intersect at the type level break now */
	if (ret == 0)
	    return(0);
    }
    switch (atom1->type) {
        case XML_REGEXP_STRING:
            if (!deep)
                ret = (atom1->valuep != atom2->valuep);
            else {
                xmlChar *val1 = (xmlChar *)atom1->valuep;
                xmlChar *val2 = (xmlChar *)atom2->valuep;
                int compound1 = (xmlStrchr(val1, '|') != NULL);
                int compound2 = (xmlStrchr(val2, '|') != NULL);

                /* Ignore negative match flag for ##other namespaces */
                if (compound1 != compound2)
                    return(0);

                ret = xmlRegStrEqualWildcard(val1, val2);
            }
	    break;
        case XML_REGEXP_EPSILON:
	    goto not_determinist;
        case XML_REGEXP_CHARVAL:
	    if (atom2->type == XML_REGEXP_CHARVAL) {
		ret = (atom1->codepoint == atom2->codepoint);
	    } else {
	        ret = xmlRegCheckCharacter(atom2, atom1->codepoint);
		if (ret < 0)
		    ret = 1;
	    }
	    break;
        case XML_REGEXP_RANGES:
	    if (atom2->type == XML_REGEXP_RANGES) {
	        int i, j, res;
		xmlRegRangePtr r1, r2;

		/*
		 * need to check that none of the ranges eventually matches
		 */
		for (i = 0;i < atom1->nbRanges;i++) {
		    for (j = 0;j < atom2->nbRanges;j++) {
			r1 = atom1->ranges[i];
			r2 = atom2->ranges[j];
			res = xmlFACompareRanges(r1, r2);
			if (res == 1) {
			    ret = 1;
			    goto done;
			}
		    }
		}
		ret = 0;
	    }
	    break;
	default:
	    goto not_determinist;
    }
done:
    if (atom1->neg != atom2->neg) {
        ret = !ret;
    }
    if (ret == 0)
        return(0);
not_determinist:
    return(1);
}

/**
 * Check whether the associated regexp is determinist,
 * should be called after xmlFAEliminateEpsilonTransitions
 *
 * @param ctxt  a regexp parser context
 * @param state  regexp state
 * @param fromnr  the from state
 * @param tonr  the to state
 * @param atom  the atom
 */
static int
xmlFARecurseDeterminism(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr state,
	                int fromnr, int tonr, xmlRegAtomPtr atom) {
    int ret = 1;
    int res;
    int transnr, nbTrans;
    xmlRegTransPtr t1;
    int deep = 1;

    if (state == NULL)
	return(ret);
    if (state->markd == XML_REGEXP_MARK_VISITED)
	return(ret);

    if (ctxt->flags & AM_AUTOMATA_RNG)
        deep = 0;

    /*
     * don't recurse on transitions potentially added in the course of
     * the elimination.
     */
    nbTrans = state->nbTrans;
    for (transnr = 0;transnr < nbTrans;transnr++) {
	t1 = &(state->trans[transnr]);
	/*
	 * check transitions conflicting with the one looked at
	 */
        if ((t1->to < 0) || (t1->to == fromnr))
            continue;
	if (t1->atom == NULL) {
	    state->markd = XML_REGEXP_MARK_VISITED;
	    res = xmlFARecurseDeterminism(ctxt, ctxt->states[t1->to],
		                          fromnr, tonr, atom);
	    if (res == 0) {
	        ret = 0;
		/* t1->nd = 1; */
	    }
	    continue;
	}
	if (xmlFACompareAtoms(t1->atom, atom, deep)) {
            /* Treat equal transitions as deterministic. */
            if ((t1->to != tonr) ||
                (!xmlFAEqualAtoms(t1->atom, atom, deep)))
                ret = 0;
	    /* mark the transition as non-deterministic */
	    t1->nd = 1;
	}
    }
    return(ret);
}

/**
 * Reset flags after checking determinism.
 *
 * @param ctxt  a regexp parser context
 * @param state  regexp state
 */
static void
xmlFAFinishRecurseDeterminism(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr state) {
    int transnr, nbTrans;

    if (state == NULL)
	return;
    if (state->markd != XML_REGEXP_MARK_VISITED)
	return;
    state->markd = 0;

    nbTrans = state->nbTrans;
    for (transnr = 0; transnr < nbTrans; transnr++) {
	xmlRegTransPtr t1 = &state->trans[transnr];
	if ((t1->atom == NULL) && (t1->to >= 0))
	    xmlFAFinishRecurseDeterminism(ctxt, ctxt->states[t1->to]);
    }
}

/**
 * Check whether the associated regexp is determinist,
 * should be called after xmlFAEliminateEpsilonTransitions
 *
 * @param ctxt  a regexp parser context
 */
static int
xmlFAComputesDeterminism(xmlRegParserCtxtPtr ctxt) {
    int statenr, transnr;
    xmlRegStatePtr state;
    xmlRegTransPtr t1, t2, last;
    int i;
    int ret = 1;
    int deep = 1;

    if (ctxt->determinist != -1)
	return(ctxt->determinist);

    if (ctxt->flags & AM_AUTOMATA_RNG)
        deep = 0;

    /*
     * First cleanup the automata removing cancelled transitions
     */
    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if (state == NULL)
	    continue;
	if (state->nbTrans < 2)
	    continue;
	for (transnr = 0;transnr < state->nbTrans;transnr++) {
	    t1 = &(state->trans[transnr]);
	    /*
	     * Determinism checks in case of counted or all transitions
	     * will have to be handled separately
	     */
	    if (t1->atom == NULL) {
		/* t1->nd = 1; */
		continue;
	    }
	    if (t1->to < 0) /* eliminated */
		continue;
	    for (i = 0;i < transnr;i++) {
		t2 = &(state->trans[i]);
		if (t2->to < 0) /* eliminated */
		    continue;
		if (t2->atom != NULL) {
		    if (t1->to == t2->to) {
                        /*
                         * Here we use deep because we want to keep the
                         * transitions which indicate a conflict
                         */
			if (xmlFAEqualAtoms(t1->atom, t2->atom, deep) &&
                            (t1->counter == t2->counter) &&
                            (t1->count == t2->count))
			    t2->to = -1; /* eliminated */
		    }
		}
	    }
	}
    }

    /*
     * Check for all states that there aren't 2 transitions
     * with the same atom and a different target.
     */
    for (statenr = 0;statenr < ctxt->nbStates;statenr++) {
	state = ctxt->states[statenr];
	if (state == NULL)
	    continue;
	if (state->nbTrans < 2)
	    continue;
	last = NULL;
	for (transnr = 0;transnr < state->nbTrans;transnr++) {
	    t1 = &(state->trans[transnr]);
	    /*
	     * Determinism checks in case of counted or all transitions
	     * will have to be handled separately
	     */
	    if (t1->atom == NULL) {
		continue;
	    }
	    if (t1->to < 0) /* eliminated */
		continue;
	    for (i = 0;i < transnr;i++) {
		t2 = &(state->trans[i]);
		if (t2->to < 0) /* eliminated */
		    continue;
		if (t2->atom != NULL) {
                    /*
                     * But here we don't use deep because we want to
                     * find transitions which indicate a conflict
                     */
		    if (xmlFACompareAtoms(t1->atom, t2->atom, 1)) {
                        /*
                         * Treat equal counter transitions that couldn't be
                         * eliminated as deterministic.
                         */
                        if ((t1->to != t2->to) ||
                            (t1->counter == t2->counter) ||
                            (!xmlFAEqualAtoms(t1->atom, t2->atom, deep)))
                            ret = 0;
			/* mark the transitions as non-deterministic ones */
			t1->nd = 1;
			t2->nd = 1;
			last = t1;
		    }
		} else {
                    int res;

		    /*
		     * do the closure in case of remaining specific
		     * epsilon transitions like choices or all
		     */
		    res = xmlFARecurseDeterminism(ctxt, ctxt->states[t2->to],
						  statenr, t1->to, t1->atom);
                    xmlFAFinishRecurseDeterminism(ctxt, ctxt->states[t2->to]);
		    /* don't shortcut the computation so all non deterministic
		       transition get marked down
		    if (ret == 0)
			return(0);
		     */
		    if (res == 0) {
			t1->nd = 1;
			/* t2->nd = 1; */
			last = t1;
                        ret = 0;
		    }
		}
	    }
	    /* don't shortcut the computation so all non deterministic
	       transition get marked down
	    if (ret == 0)
		break; */
	}

	/*
	 * mark specifically the last non-deterministic transition
	 * from a state since there is no need to set-up rollback
	 * from it
	 */
	if (last != NULL) {
	    last->nd = 2;
	}

	/* don't shortcut the computation so all non deterministic
	   transition get marked down
	if (ret == 0)
	    break; */
    }

    ctxt->determinist = ret;
    return(ret);
}

/************************************************************************
 *									*
 *	Routines to check input against transition atoms		*
 *									*
 ************************************************************************/

static int
xmlRegCheckCharacterRange(xmlRegAtomType type, int codepoint, int neg,
	                  int start, int end, const xmlChar *blockName) {
    int ret = 0;

    switch (type) {
        case XML_REGEXP_STRING:
        case XML_REGEXP_SUBREG:
        case XML_REGEXP_RANGES:
        case XML_REGEXP_EPSILON:
	    return(-1);
        case XML_REGEXP_ANYCHAR:
	    ret = ((codepoint != '\n') && (codepoint != '\r'));
	    break;
        case XML_REGEXP_CHARVAL:
	    ret = ((codepoint >= start) && (codepoint <= end));
	    break;
        case XML_REGEXP_NOTSPACE:
	    neg = !neg;
            /* Falls through. */
        case XML_REGEXP_ANYSPACE:
	    ret = ((codepoint == '\n') || (codepoint == '\r') ||
		   (codepoint == '\t') || (codepoint == ' '));
	    break;
        case XML_REGEXP_NOTINITNAME:
	    neg = !neg;
            /* Falls through. */
        case XML_REGEXP_INITNAME:
	    ret = (IS_LETTER(codepoint) ||
		   (codepoint == '_') || (codepoint == ':'));
	    break;
        case XML_REGEXP_NOTNAMECHAR:
	    neg = !neg;
            /* Falls through. */
        case XML_REGEXP_NAMECHAR:
	    ret = (IS_LETTER(codepoint) || IS_DIGIT(codepoint) ||
		   (codepoint == '.') || (codepoint == '-') ||
		   (codepoint == '_') || (codepoint == ':') ||
		   IS_COMBINING(codepoint) || IS_EXTENDER(codepoint));
	    break;
        case XML_REGEXP_NOTDECIMAL:
	    neg = !neg;
            /* Falls through. */
        case XML_REGEXP_DECIMAL:
	    ret = xmlUCSIsCatNd(codepoint);
	    break;
        case XML_REGEXP_REALCHAR:
	    neg = !neg;
            /* Falls through. */
        case XML_REGEXP_NOTREALCHAR:
	    ret = xmlUCSIsCatP(codepoint);
	    if (ret == 0)
		ret = xmlUCSIsCatZ(codepoint);
	    if (ret == 0)
		ret = xmlUCSIsCatC(codepoint);
	    break;
        case XML_REGEXP_LETTER:
	    ret = xmlUCSIsCatL(codepoint);
	    break;
        case XML_REGEXP_LETTER_UPPERCASE:
	    ret = xmlUCSIsCatLu(codepoint);
	    break;
        case XML_REGEXP_LETTER_LOWERCASE:
	    ret = xmlUCSIsCatLl(codepoint);
	    break;
        case XML_REGEXP_LETTER_TITLECASE:
	    ret = xmlUCSIsCatLt(codepoint);
	    break;
        case XML_REGEXP_LETTER_MODIFIER:
	    ret = xmlUCSIsCatLm(codepoint);
	    break;
        case XML_REGEXP_LETTER_OTHERS:
	    ret = xmlUCSIsCatLo(codepoint);
	    break;
        case XML_REGEXP_MARK:
	    ret = xmlUCSIsCatM(codepoint);
	    break;
        case XML_REGEXP_MARK_NONSPACING:
	    ret = xmlUCSIsCatMn(codepoint);
	    break;
        case XML_REGEXP_MARK_SPACECOMBINING:
	    ret = xmlUCSIsCatMc(codepoint);
	    break;
        case XML_REGEXP_MARK_ENCLOSING:
	    ret = xmlUCSIsCatMe(codepoint);
	    break;
        case XML_REGEXP_NUMBER:
	    ret = xmlUCSIsCatN(codepoint);
	    break;
        case XML_REGEXP_NUMBER_DECIMAL:
	    ret = xmlUCSIsCatNd(codepoint);
	    break;
        case XML_REGEXP_NUMBER_LETTER:
	    ret = xmlUCSIsCatNl(codepoint);
	    break;
        case XML_REGEXP_NUMBER_OTHERS:
	    ret = xmlUCSIsCatNo(codepoint);
	    break;
        case XML_REGEXP_PUNCT:
	    ret = xmlUCSIsCatP(codepoint);
	    break;
        case XML_REGEXP_PUNCT_CONNECTOR:
	    ret = xmlUCSIsCatPc(codepoint);
	    break;
        case XML_REGEXP_PUNCT_DASH:
	    ret = xmlUCSIsCatPd(codepoint);
	    break;
        case XML_REGEXP_PUNCT_OPEN:
	    ret = xmlUCSIsCatPs(codepoint);
	    break;
        case XML_REGEXP_PUNCT_CLOSE:
	    ret = xmlUCSIsCatPe(codepoint);
	    break;
        case XML_REGEXP_PUNCT_INITQUOTE:
	    ret = xmlUCSIsCatPi(codepoint);
	    break;
        case XML_REGEXP_PUNCT_FINQUOTE:
	    ret = xmlUCSIsCatPf(codepoint);
	    break;
        case XML_REGEXP_PUNCT_OTHERS:
	    ret = xmlUCSIsCatPo(codepoint);
	    break;
        case XML_REGEXP_SEPAR:
	    ret = xmlUCSIsCatZ(codepoint);
	    break;
        case XML_REGEXP_SEPAR_SPACE:
	    ret = xmlUCSIsCatZs(codepoint);
	    break;
        case XML_REGEXP_SEPAR_LINE:
	    ret = xmlUCSIsCatZl(codepoint);
	    break;
        case XML_REGEXP_SEPAR_PARA:
	    ret = xmlUCSIsCatZp(codepoint);
	    break;
        case XML_REGEXP_SYMBOL:
	    ret = xmlUCSIsCatS(codepoint);
	    break;
        case XML_REGEXP_SYMBOL_MATH:
	    ret = xmlUCSIsCatSm(codepoint);
	    break;
        case XML_REGEXP_SYMBOL_CURRENCY:
	    ret = xmlUCSIsCatSc(codepoint);
	    break;
        case XML_REGEXP_SYMBOL_MODIFIER:
	    ret = xmlUCSIsCatSk(codepoint);
	    break;
        case XML_REGEXP_SYMBOL_OTHERS:
	    ret = xmlUCSIsCatSo(codepoint);
	    break;
        case XML_REGEXP_OTHER:
	    ret = xmlUCSIsCatC(codepoint);
	    break;
        case XML_REGEXP_OTHER_CONTROL:
	    ret = xmlUCSIsCatCc(codepoint);
	    break;
        case XML_REGEXP_OTHER_FORMAT:
	    ret = xmlUCSIsCatCf(codepoint);
	    break;
        case XML_REGEXP_OTHER_PRIVATE:
	    ret = xmlUCSIsCatCo(codepoint);
	    break;
        case XML_REGEXP_OTHER_NA:
	    /* ret = xmlUCSIsCatCn(codepoint); */
	    /* Seems it doesn't exist anymore in recent Unicode releases */
	    ret = 0;
	    break;
        case XML_REGEXP_BLOCK_NAME:
	    ret = xmlUCSIsBlock(codepoint, (const char *) blockName);
	    break;
    }
    if (neg)
	return(!ret);
    return(ret);
}

static int
xmlRegCheckCharacter(xmlRegAtomPtr atom, int codepoint) {
    int i, ret = 0;
    xmlRegRangePtr range;

    if ((atom == NULL) || (!IS_CHAR(codepoint)))
	return(-1);

    switch (atom->type) {
        case XML_REGEXP_SUBREG:
        case XML_REGEXP_EPSILON:
	    return(-1);
        case XML_REGEXP_CHARVAL:
            return(codepoint == atom->codepoint);
        case XML_REGEXP_RANGES: {
	    int accept = 0;

	    for (i = 0;i < atom->nbRanges;i++) {
		range = atom->ranges[i];
		if (range->neg == 2) {
		    ret = xmlRegCheckCharacterRange(range->type, codepoint,
						0, range->start, range->end,
						range->blockName);
		    if (ret != 0)
			return(0); /* excluded char */
		} else if (range->neg) {
		    ret = xmlRegCheckCharacterRange(range->type, codepoint,
						0, range->start, range->end,
						range->blockName);
		    if (ret == 0)
		        accept = 1;
		    else
		        return(0);
		} else {
		    ret = xmlRegCheckCharacterRange(range->type, codepoint,
						0, range->start, range->end,
						range->blockName);
		    if (ret != 0)
			accept = 1; /* might still be excluded */
		}
	    }
	    return(accept);
	}
        case XML_REGEXP_STRING:
	    return(-1);
        case XML_REGEXP_ANYCHAR:
        case XML_REGEXP_ANYSPACE:
        case XML_REGEXP_NOTSPACE:
        case XML_REGEXP_INITNAME:
        case XML_REGEXP_NOTINITNAME:
        case XML_REGEXP_NAMECHAR:
        case XML_REGEXP_NOTNAMECHAR:
        case XML_REGEXP_DECIMAL:
        case XML_REGEXP_NOTDECIMAL:
        case XML_REGEXP_REALCHAR:
        case XML_REGEXP_NOTREALCHAR:
        case XML_REGEXP_LETTER:
        case XML_REGEXP_LETTER_UPPERCASE:
        case XML_REGEXP_LETTER_LOWERCASE:
        case XML_REGEXP_LETTER_TITLECASE:
        case XML_REGEXP_LETTER_MODIFIER:
        case XML_REGEXP_LETTER_OTHERS:
        case XML_REGEXP_MARK:
        case XML_REGEXP_MARK_NONSPACING:
        case XML_REGEXP_MARK_SPACECOMBINING:
        case XML_REGEXP_MARK_ENCLOSING:
        case XML_REGEXP_NUMBER:
        case XML_REGEXP_NUMBER_DECIMAL:
        case XML_REGEXP_NUMBER_LETTER:
        case XML_REGEXP_NUMBER_OTHERS:
        case XML_REGEXP_PUNCT:
        case XML_REGEXP_PUNCT_CONNECTOR:
        case XML_REGEXP_PUNCT_DASH:
        case XML_REGEXP_PUNCT_OPEN:
        case XML_REGEXP_PUNCT_CLOSE:
        case XML_REGEXP_PUNCT_INITQUOTE:
        case XML_REGEXP_PUNCT_FINQUOTE:
        case XML_REGEXP_PUNCT_OTHERS:
        case XML_REGEXP_SEPAR:
        case XML_REGEXP_SEPAR_SPACE:
        case XML_REGEXP_SEPAR_LINE:
        case XML_REGEXP_SEPAR_PARA:
        case XML_REGEXP_SYMBOL:
        case XML_REGEXP_SYMBOL_MATH:
        case XML_REGEXP_SYMBOL_CURRENCY:
        case XML_REGEXP_SYMBOL_MODIFIER:
        case XML_REGEXP_SYMBOL_OTHERS:
        case XML_REGEXP_OTHER:
        case XML_REGEXP_OTHER_CONTROL:
        case XML_REGEXP_OTHER_FORMAT:
        case XML_REGEXP_OTHER_PRIVATE:
        case XML_REGEXP_OTHER_NA:
	case XML_REGEXP_BLOCK_NAME:
	    ret = xmlRegCheckCharacterRange(atom->type, codepoint, 0, 0, 0,
		                            (const xmlChar *)atom->valuep);
	    if (atom->neg)
		ret = !ret;
	    break;
    }
    return(ret);
}

/************************************************************************
 *									*
 *	Saving and restoring state of an execution context		*
 *									*
 ************************************************************************/

static void
xmlFARegExecSave(xmlRegExecCtxtPtr exec) {
#ifdef MAX_PUSH
    if (exec->nbPush > MAX_PUSH) {
        exec->status = XML_REGEXP_INTERNAL_LIMIT;
        return;
    }
    exec->nbPush++;
#endif

    if (exec->nbRollbacks >= exec->maxRollbacks) {
	xmlRegExecRollback *tmp;
        int newSize;
	int len = exec->nbRollbacks;

        newSize = xmlGrowCapacity(exec->maxRollbacks, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
	if (newSize < 0) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
	    return;
	}
	tmp = xmlRealloc(exec->rollbacks, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
	    return;
	}
	exec->rollbacks = tmp;
	exec->maxRollbacks = newSize;
	tmp = &exec->rollbacks[len];
	memset(tmp, 0, (exec->maxRollbacks - len) * sizeof(xmlRegExecRollback));
    }
    exec->rollbacks[exec->nbRollbacks].state = exec->state;
    exec->rollbacks[exec->nbRollbacks].index = exec->index;
    exec->rollbacks[exec->nbRollbacks].nextbranch = exec->transno + 1;
    if (exec->comp->nbCounters > 0) {
	if (exec->rollbacks[exec->nbRollbacks].counts == NULL) {
	    exec->rollbacks[exec->nbRollbacks].counts = (int *)
		xmlMalloc(exec->comp->nbCounters * sizeof(int));
	    if (exec->rollbacks[exec->nbRollbacks].counts == NULL) {
		exec->status = XML_REGEXP_OUT_OF_MEMORY;
		return;
	    }
	}
	memcpy(exec->rollbacks[exec->nbRollbacks].counts, exec->counts,
	       exec->comp->nbCounters * sizeof(int));
    }
    exec->nbRollbacks++;
}

static void
xmlFARegExecRollBack(xmlRegExecCtxtPtr exec) {
    if (exec->status != XML_REGEXP_OK)
        return;
    if (exec->nbRollbacks <= 0) {
	exec->status = XML_REGEXP_NOT_FOUND;
	return;
    }
    exec->nbRollbacks--;
    exec->state = exec->rollbacks[exec->nbRollbacks].state;
    exec->index = exec->rollbacks[exec->nbRollbacks].index;
    exec->transno = exec->rollbacks[exec->nbRollbacks].nextbranch;
    if (exec->comp->nbCounters > 0) {
	if (exec->rollbacks[exec->nbRollbacks].counts == NULL) {
	    exec->status = XML_REGEXP_INTERNAL_ERROR;
	    return;
	}
	if (exec->counts) {
	    memcpy(exec->counts, exec->rollbacks[exec->nbRollbacks].counts,
	       exec->comp->nbCounters * sizeof(int));
	}
    }
}

/************************************************************************
 *									*
 *	Verifier, running an input against a compiled regexp		*
 *									*
 ************************************************************************/

static int
xmlFARegExec(xmlRegexpPtr comp, const xmlChar *content) {
    xmlRegExecCtxt execval;
    xmlRegExecCtxtPtr exec = &execval;
    int ret, codepoint = 0, len, deter;

    exec->inputString = content;
    exec->index = 0;
    exec->nbPush = 0;
    exec->determinist = 1;
    exec->maxRollbacks = 0;
    exec->nbRollbacks = 0;
    exec->rollbacks = NULL;
    exec->status = XML_REGEXP_OK;
    exec->comp = comp;
    exec->state = comp->states[0];
    exec->transno = 0;
    exec->transcount = 0;
    exec->inputStack = NULL;
    exec->inputStackMax = 0;
    if (comp->nbCounters > 0) {
	exec->counts = (int *) xmlMalloc(comp->nbCounters * sizeof(int));
	if (exec->counts == NULL) {
	    return(XML_REGEXP_OUT_OF_MEMORY);
	}
        memset(exec->counts, 0, comp->nbCounters * sizeof(int));
    } else
	exec->counts = NULL;
    while ((exec->status == XML_REGEXP_OK) && (exec->state != NULL) &&
	   ((exec->inputString[exec->index] != 0) ||
	    ((exec->state != NULL) &&
	     (exec->state->type != XML_REGEXP_FINAL_STATE)))) {
	xmlRegTransPtr trans;
	xmlRegAtomPtr atom;

	/*
	 * If end of input on non-terminal state, rollback, however we may
	 * still have epsilon like transition for counted transitions
	 * on counters, in that case don't break too early.  Additionally,
	 * if we are working on a range like "AB{0,2}", where B is not present,
	 * we don't want to break.
	 */
	len = 1;
	if ((exec->inputString[exec->index] == 0) && (exec->counts == NULL)) {
	    /*
	     * if there is a transition, we must check if
	     *  atom allows minOccurs of 0
	     */
	    if (exec->transno < exec->state->nbTrans) {
	        trans = &exec->state->trans[exec->transno];
		if (trans->to >=0) {
		    atom = trans->atom;
		    if (!((atom->min == 0) && (atom->max > 0)))
		        goto rollback;
		}
	    } else
	        goto rollback;
	}

	exec->transcount = 0;
	for (;exec->transno < exec->state->nbTrans;exec->transno++) {
	    trans = &exec->state->trans[exec->transno];
	    if (trans->to < 0)
		continue;
	    atom = trans->atom;
	    ret = 0;
	    deter = 1;
	    if (trans->count >= 0) {
		int count;
		xmlRegCounterPtr counter;

		if (exec->counts == NULL) {
		    exec->status = XML_REGEXP_INTERNAL_ERROR;
		    goto error;
		}
		/*
		 * A counted transition.
		 */

		count = exec->counts[trans->count];
		counter = &exec->comp->counters[trans->count];
		ret = ((count >= counter->min) && (count <= counter->max));
		if ((ret) && (counter->min != counter->max))
		    deter = 0;
	    } else if (atom == NULL) {
		exec->status = XML_REGEXP_INTERNAL_ERROR;
		break;
	    } else if (exec->inputString[exec->index] != 0) {
                len = 4;
                codepoint = xmlGetUTF8Char(&exec->inputString[exec->index],
                                           &len);
                if (codepoint < 0) {
                    exec->status = XML_REGEXP_INVALID_UTF8;
                    goto error;
                }
		ret = xmlRegCheckCharacter(atom, codepoint);
		if ((ret == 1) && (atom->min >= 0) && (atom->max > 0)) {
		    xmlRegStatePtr to = comp->states[trans->to];

		    /*
		     * this is a multiple input sequence
		     * If there is a counter associated increment it now.
		     * do not increment if the counter is already over the
		     * maximum limit in which case get to next transition
		     */
		    if (trans->counter >= 0) {
			xmlRegCounterPtr counter;

			if ((exec->counts == NULL) ||
			    (exec->comp == NULL) ||
			    (exec->comp->counters == NULL)) {
			    exec->status = XML_REGEXP_INTERNAL_ERROR;
			    goto error;
			}
			counter = &exec->comp->counters[trans->counter];
			if (exec->counts[trans->counter] >= counter->max)
			    continue; /* for loop on transitions */
                    }
                    /* Save before incrementing */
		    if (exec->state->nbTrans > exec->transno + 1) {
			xmlFARegExecSave(exec);
                        if (exec->status != XML_REGEXP_OK)
                            goto error;
		    }
		    if (trans->counter >= 0) {
			exec->counts[trans->counter]++;
		    }
		    exec->transcount = 1;
		    do {
			/*
			 * Try to progress as much as possible on the input
			 */
			if (exec->transcount == atom->max) {
			    break;
			}
			exec->index += len;
			/*
			 * End of input: stop here
			 */
			if (exec->inputString[exec->index] == 0) {
			    exec->index -= len;
			    break;
			}
			if (exec->transcount >= atom->min) {
			    int transno = exec->transno;
			    xmlRegStatePtr state = exec->state;

			    /*
			     * The transition is acceptable save it
			     */
			    exec->transno = -1; /* trick */
			    exec->state = to;
			    xmlFARegExecSave(exec);
                            if (exec->status != XML_REGEXP_OK)
                                goto error;
			    exec->transno = transno;
			    exec->state = state;
			}
                        len = 4;
                        codepoint = xmlGetUTF8Char(
                                &exec->inputString[exec->index], &len);
                        if (codepoint < 0) {
                            exec->status = XML_REGEXP_INVALID_UTF8;
                            goto error;
                        }
			ret = xmlRegCheckCharacter(atom, codepoint);
			exec->transcount++;
		    } while (ret == 1);
		    if (exec->transcount < atom->min)
			ret = 0;

		    /*
		     * If the last check failed but one transition was found
		     * possible, rollback
		     */
		    if (ret < 0)
			ret = 0;
		    if (ret == 0) {
			goto rollback;
		    }
		    if (trans->counter >= 0) {
			if (exec->counts == NULL) {
			    exec->status = XML_REGEXP_INTERNAL_ERROR;
			    goto error;
			}
			exec->counts[trans->counter]--;
		    }
		} else if ((ret == 0) && (atom->min == 0) && (atom->max > 0)) {
		    /*
		     * we don't match on the codepoint, but minOccurs of 0
		     * says that's ok.  Setting len to 0 inhibits stepping
		     * over the codepoint.
		     */
		    exec->transcount = 1;
		    len = 0;
		    ret = 1;
		}
	    } else if ((atom->min == 0) && (atom->max > 0)) {
	        /* another spot to match when minOccurs is 0 */
		exec->transcount = 1;
		len = 0;
		ret = 1;
	    }
	    if (ret == 1) {
		if ((trans->nd == 1) ||
		    ((trans->count >= 0) && (deter == 0) &&
		     (exec->state->nbTrans > exec->transno + 1))) {
		    xmlFARegExecSave(exec);
                    if (exec->status != XML_REGEXP_OK)
                        goto error;
		}
		if (trans->counter >= 0) {
		    xmlRegCounterPtr counter;

                    /* make sure we don't go over the counter maximum value */
		    if ((exec->counts == NULL) ||
			(exec->comp == NULL) ||
			(exec->comp->counters == NULL)) {
			exec->status = XML_REGEXP_INTERNAL_ERROR;
			goto error;
		    }
		    counter = &exec->comp->counters[trans->counter];
		    if (exec->counts[trans->counter] >= counter->max)
			continue; /* for loop on transitions */
		    exec->counts[trans->counter]++;
		}
		if ((trans->count >= 0) &&
		    (trans->count < REGEXP_ALL_COUNTER)) {
		    if (exec->counts == NULL) {
		        exec->status = XML_REGEXP_INTERNAL_ERROR;
			goto error;
		    }
		    exec->counts[trans->count] = 0;
		}
		exec->state = comp->states[trans->to];
		exec->transno = 0;
		if (trans->atom != NULL) {
		    exec->index += len;
		}
		goto progress;
	    } else if (ret < 0) {
		exec->status = XML_REGEXP_INTERNAL_ERROR;
		break;
	    }
	}
	if ((exec->transno != 0) || (exec->state->nbTrans == 0)) {
rollback:
	    /*
	     * Failed to find a way out
	     */
	    exec->determinist = 0;
	    xmlFARegExecRollBack(exec);
	}
progress:
	continue;
    }
error:
    if (exec->rollbacks != NULL) {
	if (exec->counts != NULL) {
	    int i;

	    for (i = 0;i < exec->maxRollbacks;i++)
		if (exec->rollbacks[i].counts != NULL)
		    xmlFree(exec->rollbacks[i].counts);
	}
	xmlFree(exec->rollbacks);
    }
    if (exec->state == NULL)
        return(XML_REGEXP_INTERNAL_ERROR);
    if (exec->counts != NULL)
	xmlFree(exec->counts);
    if (exec->status == XML_REGEXP_OK)
	return(1);
    if (exec->status == XML_REGEXP_NOT_FOUND)
	return(0);
    return(exec->status);
}

/************************************************************************
 *									*
 *	Progressive interface to the verifier one atom at a time	*
 *									*
 ************************************************************************/

/**
 * Build a context used for progressive evaluation of a regexp.
 *
 * @deprecated Internal function, don't use.
 *
 * @param comp  a precompiled regular expression
 * @param callback  a callback function used for handling progresses in the
 *            automata matching phase
 * @param data  the context data associated to the callback in this context
 * @returns the new context
 */
xmlRegExecCtxt *
xmlRegNewExecCtxt(xmlRegexp *comp, xmlRegExecCallbacks callback, void *data) {
    xmlRegExecCtxtPtr exec;

    if (comp == NULL)
	return(NULL);
    if ((comp->compact == NULL) && (comp->states == NULL))
        return(NULL);
    exec = (xmlRegExecCtxtPtr) xmlMalloc(sizeof(xmlRegExecCtxt));
    if (exec == NULL)
	return(NULL);
    memset(exec, 0, sizeof(xmlRegExecCtxt));
    exec->inputString = NULL;
    exec->index = 0;
    exec->determinist = 1;
    exec->maxRollbacks = 0;
    exec->nbRollbacks = 0;
    exec->rollbacks = NULL;
    exec->status = XML_REGEXP_OK;
    exec->comp = comp;
    if (comp->compact == NULL)
	exec->state = comp->states[0];
    exec->transno = 0;
    exec->transcount = 0;
    exec->callback = callback;
    exec->data = data;
    if (comp->nbCounters > 0) {
        /*
	 * For error handling, exec->counts is allocated twice the size
	 * the second half is used to store the data in case of rollback
	 */
	exec->counts = (int *) xmlMalloc(comp->nbCounters * sizeof(int)
	                                 * 2);
	if (exec->counts == NULL) {
	    xmlFree(exec);
	    return(NULL);
	}
        memset(exec->counts, 0, comp->nbCounters * sizeof(int) * 2);
	exec->errCounts = &exec->counts[comp->nbCounters];
    } else {
	exec->counts = NULL;
	exec->errCounts = NULL;
    }
    exec->inputStackMax = 0;
    exec->inputStackNr = 0;
    exec->inputStack = NULL;
    exec->errStateNo = -1;
    exec->errString = NULL;
    exec->nbPush = 0;
    return(exec);
}

/**
 * Free the structures associated to a regular expression evaluation context.
 *
 * @deprecated Internal function, don't use.
 *
 * @param exec  a regular expression evaluation context
 */
void
xmlRegFreeExecCtxt(xmlRegExecCtxt *exec) {
    if (exec == NULL)
	return;

    if (exec->rollbacks != NULL) {
	if (exec->counts != NULL) {
	    int i;

	    for (i = 0;i < exec->maxRollbacks;i++)
		if (exec->rollbacks[i].counts != NULL)
		    xmlFree(exec->rollbacks[i].counts);
	}
	xmlFree(exec->rollbacks);
    }
    if (exec->counts != NULL)
	xmlFree(exec->counts);
    if (exec->inputStack != NULL) {
	int i;

	for (i = 0;i < exec->inputStackNr;i++) {
	    if (exec->inputStack[i].value != NULL)
		xmlFree(exec->inputStack[i].value);
	}
	xmlFree(exec->inputStack);
    }
    if (exec->errString != NULL)
        xmlFree(exec->errString);
    xmlFree(exec);
}

static int
xmlRegExecSetErrString(xmlRegExecCtxtPtr exec, const xmlChar *value) {
    if (exec->errString != NULL)
        xmlFree(exec->errString);
    if (value == NULL) {
        exec->errString = NULL;
    } else {
        exec->errString = xmlStrdup(value);
        if (exec->errString == NULL) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
            return(-1);
        }
    }
    return(0);
}

static void
xmlFARegExecSaveInputString(xmlRegExecCtxtPtr exec, const xmlChar *value,
	                    void *data) {
    if (exec->inputStackNr + 1 >= exec->inputStackMax) {
	xmlRegInputTokenPtr tmp;
        int newSize;

        newSize = xmlGrowCapacity(exec->inputStackMax, sizeof(tmp[0]),
                                  4, XML_MAX_ITEMS);
	if (newSize < 0) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
	    return;
	}
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (newSize < 2)
            newSize = 2;
#endif
	tmp = xmlRealloc(exec->inputStack, newSize * sizeof(tmp[0]));
	if (tmp == NULL) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
	    return;
	}
	exec->inputStack = tmp;
	exec->inputStackMax = newSize;
    }
    if (value == NULL) {
        exec->inputStack[exec->inputStackNr].value = NULL;
    } else {
        exec->inputStack[exec->inputStackNr].value = xmlStrdup(value);
        if (exec->inputStack[exec->inputStackNr].value == NULL) {
            exec->status = XML_REGEXP_OUT_OF_MEMORY;
            return;
        }
    }
    exec->inputStack[exec->inputStackNr].data = data;
    exec->inputStackNr++;
    exec->inputStack[exec->inputStackNr].value = NULL;
    exec->inputStack[exec->inputStackNr].data = NULL;
}

/**
 * Checks if both strings are equal or have the same content. "*"
 * can be used as a wildcard in `valStr`; "|" is used as a separator of
 * substrings in both `expStr` and `valStr`.
 *
 * @param expStr  the string to be evaluated
 * @param valStr  the validation string
 * @returns 1 if the comparison is satisfied and the number of substrings
 * is equal, 0 otherwise.
 */

static int
xmlRegStrEqualWildcard(const xmlChar *expStr, const xmlChar *valStr) {
    if (expStr == valStr) return(1);
    if (expStr == NULL) return(0);
    if (valStr == NULL) return(0);
    do {
	/*
	* Eval if we have a wildcard for the current item.
	*/
        if (*expStr != *valStr) {
	    /* if one of them starts with a wildcard make valStr be it */
	    if (*valStr == '*') {
	        const xmlChar *tmp;

		tmp = valStr;
		valStr = expStr;
		expStr = tmp;
	    }
	    if ((*valStr != 0) && (*expStr != 0) && (*expStr++ == '*')) {
		do {
		    if (*valStr == XML_REG_STRING_SEPARATOR)
			break;
		    valStr++;
		} while (*valStr != 0);
		continue;
	    } else
		return(0);
	}
	expStr++;
	valStr++;
    } while (*valStr != 0);
    if (*expStr != 0)
	return (0);
    else
	return (1);
}

/**
 * Push one input token in the execution context
 *
 * @param exec  a regexp execution context
 * @param comp  the precompiled exec with a compact table
 * @param value  a string token input
 * @param data  data associated to the token to reuse in callbacks
 * @returns 1 if the regexp reached a final state, 0 if non-final, and
 *     a negative value in case of error.
 */
static int
xmlRegCompactPushString(xmlRegExecCtxtPtr exec,
	                xmlRegexpPtr comp,
	                const xmlChar *value,
	                void *data) {
    int state = exec->index;
    int i, target;

    if ((comp == NULL) || (comp->compact == NULL) || (comp->stringMap == NULL))
	return(-1);

    if (value == NULL) {
	/*
	 * are we at a final state ?
	 */
	if (comp->compact[state * (comp->nbstrings + 1)] ==
            XML_REGEXP_FINAL_STATE)
	    return(1);
	return(0);
    }

    /*
     * Examine all outside transitions from current state
     */
    for (i = 0;i < comp->nbstrings;i++) {
	target = comp->compact[state * (comp->nbstrings + 1) + i + 1];
	if ((target > 0) && (target <= comp->nbstates)) {
	    target--; /* to avoid 0 */
	    if (xmlRegStrEqualWildcard(comp->stringMap[i], value)) {
		exec->index = target;
		if ((exec->callback != NULL) && (comp->transdata != NULL)) {
		    exec->callback(exec->data, value,
			  comp->transdata[state * comp->nbstrings + i], data);
		}
		if (comp->compact[target * (comp->nbstrings + 1)] ==
		    XML_REGEXP_SINK_STATE)
		    goto error;

		if (comp->compact[target * (comp->nbstrings + 1)] ==
		    XML_REGEXP_FINAL_STATE)
		    return(1);
		return(0);
	    }
	}
    }
    /*
     * Failed to find an exit transition out from current state for the
     * current token
     */
error:
    exec->errStateNo = state;
    exec->status = XML_REGEXP_NOT_FOUND;
    xmlRegExecSetErrString(exec, value);
    return(exec->status);
}

/**
 * Push one input token in the execution context
 *
 * @param exec  a regexp execution context or NULL to indicate the end
 * @param value  a string token input
 * @param data  data associated to the token to reuse in callbacks
 * @param compound  value was assembled from 2 strings
 * @returns 1 if the regexp reached a final state, 0 if non-final, and
 *     a negative value in case of error.
 */
static int
xmlRegExecPushStringInternal(xmlRegExecCtxtPtr exec, const xmlChar *value,
	                     void *data, int compound) {
    xmlRegTransPtr trans;
    xmlRegAtomPtr atom;
    int ret;
    int final = 0;
    int progress = 1;

    if (exec == NULL)
	return(-1);
    if (exec->comp == NULL)
	return(-1);
    if (exec->status != XML_REGEXP_OK)
	return(exec->status);

    if (exec->comp->compact != NULL)
	return(xmlRegCompactPushString(exec, exec->comp, value, data));

    if (value == NULL) {
        if (exec->state->type == XML_REGEXP_FINAL_STATE)
	    return(1);
	final = 1;
    }

    /*
     * If we have an active rollback stack push the new value there
     * and get back to where we were left
     */
    if ((value != NULL) && (exec->inputStackNr > 0)) {
	xmlFARegExecSaveInputString(exec, value, data);
	value = exec->inputStack[exec->index].value;
	data = exec->inputStack[exec->index].data;
    }

    while ((exec->status == XML_REGEXP_OK) &&
	   ((value != NULL) ||
	    ((final == 1) &&
	     (exec->state->type != XML_REGEXP_FINAL_STATE)))) {

	/*
	 * End of input on non-terminal state, rollback, however we may
	 * still have epsilon like transition for counted transitions
	 * on counters, in that case don't break too early.
	 */
	if ((value == NULL) && (exec->counts == NULL))
	    goto rollback;

	exec->transcount = 0;
	for (;exec->transno < exec->state->nbTrans;exec->transno++) {
	    trans = &exec->state->trans[exec->transno];
	    if (trans->to < 0)
		continue;
	    atom = trans->atom;
	    ret = 0;
	    if (trans->count == REGEXP_ALL_LAX_COUNTER) {
		int i;
		int count;
		xmlRegTransPtr t;
		xmlRegCounterPtr counter;

		ret = 0;

		/*
		 * Check all counted transitions from the current state
		 */
		if ((value == NULL) && (final)) {
		    ret = 1;
		} else if (value != NULL) {
		    for (i = 0;i < exec->state->nbTrans;i++) {
			t = &exec->state->trans[i];
			if ((t->counter < 0) || (t == trans))
			    continue;
			counter = &exec->comp->counters[t->counter];
			count = exec->counts[t->counter];
			if ((count < counter->max) &&
		            (t->atom != NULL) &&
			    (xmlStrEqual(value, t->atom->valuep))) {
			    ret = 0;
			    break;
			}
			if ((count >= counter->min) &&
			    (count < counter->max) &&
			    (t->atom != NULL) &&
			    (xmlStrEqual(value, t->atom->valuep))) {
			    ret = 1;
			    break;
			}
		    }
		}
	    } else if (trans->count == REGEXP_ALL_COUNTER) {
		int i;
		int count;
		xmlRegTransPtr t;
		xmlRegCounterPtr counter;

		ret = 1;

		/*
		 * Check all counted transitions from the current state
		 */
		for (i = 0;i < exec->state->nbTrans;i++) {
                    t = &exec->state->trans[i];
		    if ((t->counter < 0) || (t == trans))
			continue;
                    counter = &exec->comp->counters[t->counter];
		    count = exec->counts[t->counter];
		    if ((count < counter->min) || (count > counter->max)) {
			ret = 0;
			break;
		    }
		}
	    } else if (trans->count >= 0) {
		int count;
		xmlRegCounterPtr counter;

		/*
		 * A counted transition.
		 */

		count = exec->counts[trans->count];
		counter = &exec->comp->counters[trans->count];
		ret = ((count >= counter->min) && (count <= counter->max));
	    } else if (atom == NULL) {
		exec->status = XML_REGEXP_INTERNAL_ERROR;
		break;
	    } else if (value != NULL) {
		ret = xmlRegStrEqualWildcard(atom->valuep, value);
		if (atom->neg) {
		    ret = !ret;
		    if (!compound)
		        ret = 0;
		}
		if ((ret == 1) && (trans->counter >= 0)) {
		    xmlRegCounterPtr counter;
		    int count;

		    count = exec->counts[trans->counter];
		    counter = &exec->comp->counters[trans->counter];
		    if (count >= counter->max)
			ret = 0;
		}

		if ((ret == 1) && (atom->min > 0) && (atom->max > 0)) {
		    xmlRegStatePtr to = exec->comp->states[trans->to];

		    /*
		     * this is a multiple input sequence
		     */
		    if (exec->state->nbTrans > exec->transno + 1) {
			if (exec->inputStackNr <= 0) {
			    xmlFARegExecSaveInputString(exec, value, data);
			}
			xmlFARegExecSave(exec);
		    }
		    exec->transcount = 1;
		    do {
			/*
			 * Try to progress as much as possible on the input
			 */
			if (exec->transcount == atom->max) {
			    break;
			}
			exec->index++;
			value = exec->inputStack[exec->index].value;
			data = exec->inputStack[exec->index].data;

			/*
			 * End of input: stop here
			 */
			if (value == NULL) {
			    exec->index --;
			    break;
			}
			if (exec->transcount >= atom->min) {
			    int transno = exec->transno;
			    xmlRegStatePtr state = exec->state;

			    /*
			     * The transition is acceptable save it
			     */
			    exec->transno = -1; /* trick */
			    exec->state = to;
			    if (exec->inputStackNr <= 0) {
				xmlFARegExecSaveInputString(exec, value, data);
			    }
			    xmlFARegExecSave(exec);
			    exec->transno = transno;
			    exec->state = state;
			}
			ret = xmlStrEqual(value, atom->valuep);
			exec->transcount++;
		    } while (ret == 1);
		    if (exec->transcount < atom->min)
			ret = 0;

		    /*
		     * If the last check failed but one transition was found
		     * possible, rollback
		     */
		    if (ret < 0)
			ret = 0;
		    if (ret == 0) {
			goto rollback;
		    }
		}
	    }
	    if (ret == 1) {
		if ((exec->callback != NULL) && (atom != NULL) &&
			(data != NULL)) {
		    exec->callback(exec->data, atom->valuep,
			           atom->data, data);
		}
		if (exec->state->nbTrans > exec->transno + 1) {
		    if (exec->inputStackNr <= 0) {
			xmlFARegExecSaveInputString(exec, value, data);
		    }
		    xmlFARegExecSave(exec);
		}
		if (trans->counter >= 0) {
		    exec->counts[trans->counter]++;
		}
		if ((trans->count >= 0) &&
		    (trans->count < REGEXP_ALL_COUNTER)) {
		    exec->counts[trans->count] = 0;
		}
                if ((exec->comp->states[trans->to] != NULL) &&
		    (exec->comp->states[trans->to]->type ==
		     XML_REGEXP_SINK_STATE)) {
		    /*
		     * entering a sink state, save the current state as error
		     * state.
		     */
                    if (xmlRegExecSetErrString(exec, value) < 0)
                        break;
		    exec->errState = exec->state;
		    memcpy(exec->errCounts, exec->counts,
			   exec->comp->nbCounters * sizeof(int));
		}
		exec->state = exec->comp->states[trans->to];
		exec->transno = 0;
		if (trans->atom != NULL) {
		    if (exec->inputStack != NULL) {
			exec->index++;
			if (exec->index < exec->inputStackNr) {
			    value = exec->inputStack[exec->index].value;
			    data = exec->inputStack[exec->index].data;
			} else {
			    value = NULL;
			    data = NULL;
			}
		    } else {
			value = NULL;
			data = NULL;
		    }
		}
		goto progress;
	    } else if (ret < 0) {
		exec->status = XML_REGEXP_INTERNAL_ERROR;
		break;
	    }
	}
	if ((exec->transno != 0) || (exec->state->nbTrans == 0)) {
rollback:
            /*
	     * if we didn't yet rollback on the current input
	     * store the current state as the error state.
	     */
	    if ((progress) && (exec->state != NULL) &&
	        (exec->state->type != XML_REGEXP_SINK_STATE)) {
	        progress = 0;
                if (xmlRegExecSetErrString(exec, value) < 0)
                    break;
		exec->errState = exec->state;
                if (exec->comp->nbCounters)
                    memcpy(exec->errCounts, exec->counts,
                           exec->comp->nbCounters * sizeof(int));
	    }

	    /*
	     * Failed to find a way out
	     */
	    exec->determinist = 0;
	    xmlFARegExecRollBack(exec);
	    if ((exec->inputStack != NULL ) &&
                (exec->status == XML_REGEXP_OK)) {
		value = exec->inputStack[exec->index].value;
		data = exec->inputStack[exec->index].data;
	    }
	}
	continue;
progress:
        progress = 1;
    }
    if (exec->status == XML_REGEXP_OK) {
        return(exec->state->type == XML_REGEXP_FINAL_STATE);
    }
    return(exec->status);
}

/**
 * Push one input token in the execution context
 *
 * @deprecated Internal function, don't use.
 *
 * @param exec  a regexp execution context or NULL to indicate the end
 * @param value  a string token input
 * @param data  data associated to the token to reuse in callbacks
 * @returns 1 if the regexp reached a final state, 0 if non-final, and
 *     a negative value in case of error.
 */
int
xmlRegExecPushString(xmlRegExecCtxt *exec, const xmlChar *value,
	             void *data) {
    return(xmlRegExecPushStringInternal(exec, value, data, 0));
}

/**
 * Push one input token in the execution context
 *
 * @deprecated Internal function, don't use.
 *
 * @param exec  a regexp execution context or NULL to indicate the end
 * @param value  the first string token input
 * @param value2  the second string token input
 * @param data  data associated to the token to reuse in callbacks
 * @returns 1 if the regexp reached a final state, 0 if non-final, and
 *     a negative value in case of error.
 */
int
xmlRegExecPushString2(xmlRegExecCtxt *exec, const xmlChar *value,
                      const xmlChar *value2, void *data) {
    xmlChar buf[150];
    int lenn, lenp, ret;
    xmlChar *str;

    if (exec == NULL)
	return(-1);
    if (exec->comp == NULL)
	return(-1);
    if (exec->status != XML_REGEXP_OK)
	return(exec->status);

    if (value2 == NULL)
        return(xmlRegExecPushString(exec, value, data));

    lenn = strlen((char *) value2);
    lenp = strlen((char *) value);

    if (150 < lenn + lenp + 2) {
	str = xmlMalloc(lenn + lenp + 2);
	if (str == NULL) {
	    exec->status = XML_REGEXP_OUT_OF_MEMORY;
	    return(-1);
	}
    } else {
	str = buf;
    }
    memcpy(&str[0], value, lenp);
    str[lenp] = XML_REG_STRING_SEPARATOR;
    memcpy(&str[lenp + 1], value2, lenn);
    str[lenn + lenp + 1] = 0;

    if (exec->comp->compact != NULL)
	ret = xmlRegCompactPushString(exec, exec->comp, str, data);
    else
        ret = xmlRegExecPushStringInternal(exec, str, data, 1);

    if (str != buf)
        xmlFree(str);
    return(ret);
}

/**
 * Extract information from the regexp execution. Internal routine to
 * implement #xmlRegExecNextValues and #xmlRegExecErrInfo
 *
 * @param exec  a regexp execution context
 * @param err  error extraction or normal one
 * @param nbval  pointer to the number of accepted values IN/OUT
 * @param nbneg  return number of negative transitions
 * @param values  pointer to the array of acceptable values
 * @param terminal  return value if this was a terminal state
 * @returns 0 in case of success or -1 in case of error.
 */
static int
xmlRegExecGetValues(xmlRegExecCtxtPtr exec, int err,
                    int *nbval, int *nbneg,
		    xmlChar **values, int *terminal) {
    int maxval;
    int nb = 0;

    if ((exec == NULL) || (nbval == NULL) || (nbneg == NULL) ||
        (values == NULL) || (*nbval <= 0))
        return(-1);

    maxval = *nbval;
    *nbval = 0;
    *nbneg = 0;
    if ((exec->comp != NULL) && (exec->comp->compact != NULL)) {
        xmlRegexpPtr comp;
	int target, i, state;

        comp = exec->comp;

	if (err) {
	    if (exec->errStateNo == -1) return(-1);
	    state = exec->errStateNo;
	} else {
	    state = exec->index;
	}
	if (terminal != NULL) {
	    if (comp->compact[state * (comp->nbstrings + 1)] ==
	        XML_REGEXP_FINAL_STATE)
		*terminal = 1;
	    else
		*terminal = 0;
	}
	for (i = 0;(i < comp->nbstrings) && (nb < maxval);i++) {
	    target = comp->compact[state * (comp->nbstrings + 1) + i + 1];
	    if ((target > 0) && (target <= comp->nbstates) &&
	        (comp->compact[(target - 1) * (comp->nbstrings + 1)] !=
		 XML_REGEXP_SINK_STATE)) {
	        values[nb++] = comp->stringMap[i];
		(*nbval)++;
	    }
	}
	for (i = 0;(i < comp->nbstrings) && (nb < maxval);i++) {
	    target = comp->compact[state * (comp->nbstrings + 1) + i + 1];
	    if ((target > 0) && (target <= comp->nbstates) &&
	        (comp->compact[(target - 1) * (comp->nbstrings + 1)] ==
		 XML_REGEXP_SINK_STATE)) {
	        values[nb++] = comp->stringMap[i];
		(*nbneg)++;
	    }
	}
    } else {
        int transno;
	xmlRegTransPtr trans;
	xmlRegAtomPtr atom;
	xmlRegStatePtr state;

	if (terminal != NULL) {
	    if (exec->state->type == XML_REGEXP_FINAL_STATE)
		*terminal = 1;
	    else
		*terminal = 0;
	}

	if (err) {
	    if (exec->errState == NULL) return(-1);
	    state = exec->errState;
	} else {
	    if (exec->state == NULL) return(-1);
	    state = exec->state;
	}
	for (transno = 0;
	     (transno < state->nbTrans) && (nb < maxval);
	     transno++) {
	    trans = &state->trans[transno];
	    if (trans->to < 0)
		continue;
	    atom = trans->atom;
	    if ((atom == NULL) || (atom->valuep == NULL))
		continue;
	    if (trans->count == REGEXP_ALL_LAX_COUNTER) {
	        /* this should not be reached but ... */
	    } else if (trans->count == REGEXP_ALL_COUNTER) {
	        /* this should not be reached but ... */
	    } else if (trans->counter >= 0) {
		xmlRegCounterPtr counter = NULL;
		int count;

		if (err)
		    count = exec->errCounts[trans->counter];
		else
		    count = exec->counts[trans->counter];
		if (exec->comp != NULL)
		    counter = &exec->comp->counters[trans->counter];
		if ((counter == NULL) || (count < counter->max)) {
		    if (atom->neg)
			values[nb++] = (xmlChar *) atom->valuep2;
		    else
			values[nb++] = (xmlChar *) atom->valuep;
		    (*nbval)++;
		}
	    } else {
                if ((exec->comp != NULL) && (exec->comp->states[trans->to] != NULL) &&
		    (exec->comp->states[trans->to]->type !=
		     XML_REGEXP_SINK_STATE)) {
		    if (atom->neg)
			values[nb++] = (xmlChar *) atom->valuep2;
		    else
			values[nb++] = (xmlChar *) atom->valuep;
		    (*nbval)++;
		}
	    }
	}
	for (transno = 0;
	     (transno < state->nbTrans) && (nb < maxval);
	     transno++) {
	    trans = &state->trans[transno];
	    if (trans->to < 0)
		continue;
	    atom = trans->atom;
	    if ((atom == NULL) || (atom->valuep == NULL))
		continue;
	    if (trans->count == REGEXP_ALL_LAX_COUNTER) {
	        continue;
	    } else if (trans->count == REGEXP_ALL_COUNTER) {
	        continue;
	    } else if (trans->counter >= 0) {
	        continue;
	    } else {
                if ((exec->comp->states[trans->to] != NULL) &&
		    (exec->comp->states[trans->to]->type ==
		     XML_REGEXP_SINK_STATE)) {
		    if (atom->neg)
			values[nb++] = (xmlChar *) atom->valuep2;
		    else
			values[nb++] = (xmlChar *) atom->valuep;
		    (*nbneg)++;
		}
	    }
	}
    }
    return(0);
}

/**
 * Extract information from the regexp execution.
 * The parameter `values` must point to an array of `nbval` string pointers
 * on return nbval will contain the number of possible strings in that
 * state and the `values` array will be updated with them. The string values
 * returned will be freed with the `exec` context and don't need to be
 * deallocated.
 *
 * @deprecated Internal function, don't use.
 *
 * @param exec  a regexp execution context
 * @param nbval  pointer to the number of accepted values IN/OUT
 * @param nbneg  return number of negative transitions
 * @param values  pointer to the array of acceptable values
 * @param terminal  return value if this was a terminal state
 * @returns 0 in case of success or -1 in case of error.
 */
int
xmlRegExecNextValues(xmlRegExecCtxt *exec, int *nbval, int *nbneg,
                     xmlChar **values, int *terminal) {
    return(xmlRegExecGetValues(exec, 0, nbval, nbneg, values, terminal));
}

/**
 * Extract error information from the regexp execution. The parameter
 * `string` will be updated with the value pushed and not accepted,
 * the parameter `values` must point to an array of `nbval` string pointers
 * on return nbval will contain the number of possible strings in that
 * state and the `values` array will be updated with them. The string values
 * returned will be freed with the `exec` context and don't need to be
 * deallocated.
 *
 * @deprecated Internal function, don't use.
 *
 * @param exec  a regexp execution context generating an error
 * @param string  return value for the error string
 * @param nbval  pointer to the number of accepted values IN/OUT
 * @param nbneg  return number of negative transitions
 * @param values  pointer to the array of acceptable values
 * @param terminal  return value if this was a terminal state
 * @returns 0 in case of success or -1 in case of error.
 */
int
xmlRegExecErrInfo(xmlRegExecCtxt *exec, const xmlChar **string,
                  int *nbval, int *nbneg, xmlChar **values, int *terminal) {
    if (exec == NULL)
        return(-1);
    if (string != NULL) {
        if (exec->status != XML_REGEXP_OK)
	    *string = exec->errString;
	else
	    *string = NULL;
    }
    return(xmlRegExecGetValues(exec, 1, nbval, nbneg, values, terminal));
}

/**
 * Clear errors in the context, allowing to recover
 * from errors on specific scenarios
 *
 * @param exec  a regexp execution context
 * @remarks it doesn's reset the last internal libxml2 error
 */
void
xmlRegExecClearErrors(xmlRegExecCtxt* exec) {
    exec->status = 0;
    exec->errState = NULL;
    exec->errStateNo = -1;
    xmlFree(exec->errString);
    exec->errString = NULL;
}

/************************************************************************
 *									*
 *	Parser for the Schemas Datatype Regular Expressions		*
 *	http://www.w3.org/TR/2001/REC-xmlschema-2-20010502/#regexs	*
 *									*
 ************************************************************************/

/**
 * [10]   Char   ::=   [^.\?*+()|\#x5B\#x5D]
 *
 * @param ctxt  a regexp parser context
 */
static int
xmlFAIsChar(xmlRegParserCtxtPtr ctxt) {
    int cur;
    int len;

    len = 4;
    cur = xmlGetUTF8Char(ctxt->cur, &len);
    if (cur < 0) {
        ERROR("Invalid UTF-8");
        return(0);
    }
    if ((cur == '.') || (cur == '\\') || (cur == '?') ||
	(cur == '*') || (cur == '+') || (cur == '(') ||
	(cur == ')') || (cur == '|') || (cur == 0x5B) ||
	(cur == 0x5D) || (cur == 0))
	return(-1);
    return(cur);
}

/**
 * [27]   charProp   ::=   IsCategory | IsBlock
 * [28]   IsCategory ::= Letters | Marks | Numbers | Punctuation |
 *                       Separators | Symbols | Others
 * [29]   Letters   ::=   'L' [ultmo]?
 * [30]   Marks   ::=   'M' [nce]?
 * [31]   Numbers   ::=   'N' [dlo]?
 * [32]   Punctuation   ::=   'P' [cdseifo]?
 * [33]   Separators   ::=   'Z' [slp]?
 * [34]   Symbols   ::=   'S' [mcko]?
 * [35]   Others   ::=   'C' [cfon]?
 * [36]   IsBlock   ::=   'Is' [a-zA-Z0-9\#x2D]+
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParseCharProp(xmlRegParserCtxtPtr ctxt) {
    int cur;
    xmlRegAtomType type = (xmlRegAtomType) 0;
    xmlChar *blockName = NULL;

    cur = CUR;
    if (cur == 'L') {
	NEXT;
	cur = CUR;
	if (cur == 'u') {
	    NEXT;
	    type = XML_REGEXP_LETTER_UPPERCASE;
	} else if (cur == 'l') {
	    NEXT;
	    type = XML_REGEXP_LETTER_LOWERCASE;
	} else if (cur == 't') {
	    NEXT;
	    type = XML_REGEXP_LETTER_TITLECASE;
	} else if (cur == 'm') {
	    NEXT;
	    type = XML_REGEXP_LETTER_MODIFIER;
	} else if (cur == 'o') {
	    NEXT;
	    type = XML_REGEXP_LETTER_OTHERS;
	} else {
	    type = XML_REGEXP_LETTER;
	}
    } else if (cur == 'M') {
	NEXT;
	cur = CUR;
	if (cur == 'n') {
	    NEXT;
	    /* nonspacing */
	    type = XML_REGEXP_MARK_NONSPACING;
	} else if (cur == 'c') {
	    NEXT;
	    /* spacing combining */
	    type = XML_REGEXP_MARK_SPACECOMBINING;
	} else if (cur == 'e') {
	    NEXT;
	    /* enclosing */
	    type = XML_REGEXP_MARK_ENCLOSING;
	} else {
	    /* all marks */
	    type = XML_REGEXP_MARK;
	}
    } else if (cur == 'N') {
	NEXT;
	cur = CUR;
	if (cur == 'd') {
	    NEXT;
	    /* digital */
	    type = XML_REGEXP_NUMBER_DECIMAL;
	} else if (cur == 'l') {
	    NEXT;
	    /* letter */
	    type = XML_REGEXP_NUMBER_LETTER;
	} else if (cur == 'o') {
	    NEXT;
	    /* other */
	    type = XML_REGEXP_NUMBER_OTHERS;
	} else {
	    /* all numbers */
	    type = XML_REGEXP_NUMBER;
	}
    } else if (cur == 'P') {
	NEXT;
	cur = CUR;
	if (cur == 'c') {
	    NEXT;
	    /* connector */
	    type = XML_REGEXP_PUNCT_CONNECTOR;
	} else if (cur == 'd') {
	    NEXT;
	    /* dash */
	    type = XML_REGEXP_PUNCT_DASH;
	} else if (cur == 's') {
	    NEXT;
	    /* open */
	    type = XML_REGEXP_PUNCT_OPEN;
	} else if (cur == 'e') {
	    NEXT;
	    /* close */
	    type = XML_REGEXP_PUNCT_CLOSE;
	} else if (cur == 'i') {
	    NEXT;
	    /* initial quote */
	    type = XML_REGEXP_PUNCT_INITQUOTE;
	} else if (cur == 'f') {
	    NEXT;
	    /* final quote */
	    type = XML_REGEXP_PUNCT_FINQUOTE;
	} else if (cur == 'o') {
	    NEXT;
	    /* other */
	    type = XML_REGEXP_PUNCT_OTHERS;
	} else {
	    /* all punctuation */
	    type = XML_REGEXP_PUNCT;
	}
    } else if (cur == 'Z') {
	NEXT;
	cur = CUR;
	if (cur == 's') {
	    NEXT;
	    /* space */
	    type = XML_REGEXP_SEPAR_SPACE;
	} else if (cur == 'l') {
	    NEXT;
	    /* line */
	    type = XML_REGEXP_SEPAR_LINE;
	} else if (cur == 'p') {
	    NEXT;
	    /* paragraph */
	    type = XML_REGEXP_SEPAR_PARA;
	} else {
	    /* all separators */
	    type = XML_REGEXP_SEPAR;
	}
    } else if (cur == 'S') {
	NEXT;
	cur = CUR;
	if (cur == 'm') {
	    NEXT;
	    type = XML_REGEXP_SYMBOL_MATH;
	    /* math */
	} else if (cur == 'c') {
	    NEXT;
	    type = XML_REGEXP_SYMBOL_CURRENCY;
	    /* currency */
	} else if (cur == 'k') {
	    NEXT;
	    type = XML_REGEXP_SYMBOL_MODIFIER;
	    /* modifiers */
	} else if (cur == 'o') {
	    NEXT;
	    type = XML_REGEXP_SYMBOL_OTHERS;
	    /* other */
	} else {
	    /* all symbols */
	    type = XML_REGEXP_SYMBOL;
	}
    } else if (cur == 'C') {
	NEXT;
	cur = CUR;
	if (cur == 'c') {
	    NEXT;
	    /* control */
	    type = XML_REGEXP_OTHER_CONTROL;
	} else if (cur == 'f') {
	    NEXT;
	    /* format */
	    type = XML_REGEXP_OTHER_FORMAT;
	} else if (cur == 'o') {
	    NEXT;
	    /* private use */
	    type = XML_REGEXP_OTHER_PRIVATE;
	} else if (cur == 'n') {
	    NEXT;
	    /* not assigned */
	    type = XML_REGEXP_OTHER_NA;
	} else {
	    /* all others */
	    type = XML_REGEXP_OTHER;
	}
    } else if (cur == 'I') {
	const xmlChar *start;
	NEXT;
	cur = CUR;
	if (cur != 's') {
	    ERROR("IsXXXX expected");
	    return;
	}
	NEXT;
	start = ctxt->cur;
	cur = CUR;
	if (((cur >= 'a') && (cur <= 'z')) ||
	    ((cur >= 'A') && (cur <= 'Z')) ||
	    ((cur >= '0') && (cur <= '9')) ||
	    (cur == 0x2D)) {
	    NEXT;
	    cur = CUR;
	    while (((cur >= 'a') && (cur <= 'z')) ||
		((cur >= 'A') && (cur <= 'Z')) ||
		((cur >= '0') && (cur <= '9')) ||
		(cur == 0x2D)) {
		NEXT;
		cur = CUR;
	    }
	}
	type = XML_REGEXP_BLOCK_NAME;
	blockName = xmlStrndup(start, ctxt->cur - start);
        if (blockName == NULL)
	    xmlRegexpErrMemory(ctxt);
    } else {
	ERROR("Unknown char property");
	return;
    }
    if (ctxt->atom == NULL) {
	ctxt->atom = xmlRegNewAtom(ctxt, type);
        if (ctxt->atom == NULL) {
            xmlFree(blockName);
            return;
        }
	ctxt->atom->valuep = blockName;
    } else if (ctxt->atom->type == XML_REGEXP_RANGES) {
        if (xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
                               type, 0, 0, blockName) == NULL) {
            xmlFree(blockName);
        }
    }
}

static int parse_escaped_codeunit(xmlRegParserCtxtPtr ctxt)
{
    int val = 0, i, cur;
    for (i = 0; i < 4; i++) {
	NEXT;
	val *= 16;
	cur = CUR;
	if (cur >= '0' && cur <= '9') {
	    val += cur - '0';
	} else if (cur >= 'A' && cur <= 'F') {
	    val += cur - 'A' + 10;
	} else if (cur >= 'a' && cur <= 'f') {
	    val += cur - 'a' + 10;
	} else {
	    ERROR("Expecting hex digit");
	    return -1;
	}
    }
    return val;
}

static int parse_escaped_codepoint(xmlRegParserCtxtPtr ctxt)
{
    int val = parse_escaped_codeunit(ctxt);
    if (0xD800 <= val && val <= 0xDBFF) {
	NEXT;
	if (CUR == '\\') {
	    NEXT;
	    if (CUR == 'u') {
		int low = parse_escaped_codeunit(ctxt);
		if (0xDC00 <= low && low <= 0xDFFF) {
		    return (val - 0xD800) * 0x400 + (low - 0xDC00) + 0x10000;
		}
	    }
	}
	ERROR("Invalid low surrogate pair code unit");
	val = -1;
    }
    return val;
}

/**
 * ```
 * [23] charClassEsc ::= ( SingleCharEsc | MultiCharEsc | catEsc | complEsc )
 * [24] SingleCharEsc ::= '\' [nrt\|.?*+(){}\#x2D\#x5B\#x5D\#x5E]
 * [25] catEsc   ::=   '\p{' charProp '}'
 * [26] complEsc ::=   '\P{' charProp '}'
 * [37] MultiCharEsc ::= '.' | ('\' [sSiIcCdDwW])
 * ```
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParseCharClassEsc(xmlRegParserCtxtPtr ctxt) {
    int cur;

    if (CUR == '.') {
	if (ctxt->atom == NULL) {
	    ctxt->atom = xmlRegNewAtom(ctxt, XML_REGEXP_ANYCHAR);
	} else if (ctxt->atom->type == XML_REGEXP_RANGES) {
	    xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
			       XML_REGEXP_ANYCHAR, 0, 0, NULL);
	}
	NEXT;
	return;
    }
    if (CUR != '\\') {
	ERROR("Escaped sequence: expecting \\");
	return;
    }
    NEXT;
    cur = CUR;
    if (cur == 'p') {
	NEXT;
	if (CUR != '{') {
	    ERROR("Expecting '{'");
	    return;
	}
	NEXT;
	xmlFAParseCharProp(ctxt);
	if (CUR != '}') {
	    ERROR("Expecting '}'");
	    return;
	}
	NEXT;
    } else if (cur == 'P') {
	NEXT;
	if (CUR != '{') {
	    ERROR("Expecting '{'");
	    return;
	}
	NEXT;
	xmlFAParseCharProp(ctxt);
        if (ctxt->atom != NULL)
	    ctxt->atom->neg = 1;
	if (CUR != '}') {
	    ERROR("Expecting '}'");
	    return;
	}
	NEXT;
    } else if ((cur == 'n') || (cur == 'r') || (cur == 't') || (cur == '\\') ||
	(cur == '|') || (cur == '.') || (cur == '?') || (cur == '*') ||
	(cur == '+') || (cur == '(') || (cur == ')') || (cur == '{') ||
	(cur == '}') || (cur == 0x2D) || (cur == 0x5B) || (cur == 0x5D) ||
	(cur == 0x5E) ||

	/* Non-standard escape sequences:
	 *                  Java 1.8|.NET Core 3.1|MSXML 6 */
	(cur == '!') ||     /*   +  |     +       |    +   */
	(cur == '"') ||     /*   +  |     +       |    +   */
	(cur == '#') ||     /*   +  |     +       |    +   */
	(cur == '$') ||     /*   +  |     +       |    +   */
	(cur == '%') ||     /*   +  |     +       |    +   */
	(cur == ',') ||     /*   +  |     +       |    +   */
	(cur == '/') ||     /*   +  |     +       |    +   */
	(cur == ':') ||     /*   +  |     +       |    +   */
	(cur == ';') ||     /*   +  |     +       |    +   */
	(cur == '=') ||     /*   +  |     +       |    +   */
	(cur == '>') ||     /*      |     +       |    +   */
	(cur == '@') ||     /*   +  |     +       |    +   */
	(cur == '`') ||     /*   +  |     +       |    +   */
	(cur == '~') ||     /*   +  |     +       |    +   */
	(cur == 'u')) {     /*      |     +       |    +   */
	if (ctxt->atom == NULL) {
	    ctxt->atom = xmlRegNewAtom(ctxt, XML_REGEXP_CHARVAL);
	    if (ctxt->atom != NULL) {
	        switch (cur) {
		    case 'n':
		        ctxt->atom->codepoint = '\n';
			break;
		    case 'r':
		        ctxt->atom->codepoint = '\r';
			break;
		    case 't':
		        ctxt->atom->codepoint = '\t';
			break;
		    case 'u':
			cur = parse_escaped_codepoint(ctxt);
			if (cur < 0) {
			    return;
			}
			ctxt->atom->codepoint = cur;
			break;
		    default:
			ctxt->atom->codepoint = cur;
		}
	    }
	} else if (ctxt->atom->type == XML_REGEXP_RANGES) {
            switch (cur) {
                case 'n':
                    cur = '\n';
                    break;
                case 'r':
                    cur = '\r';
                    break;
                case 't':
                    cur = '\t';
                    break;
            }
	    xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
			       XML_REGEXP_CHARVAL, cur, cur, NULL);
	}
	NEXT;
    } else if ((cur == 's') || (cur == 'S') || (cur == 'i') || (cur == 'I') ||
	(cur == 'c') || (cur == 'C') || (cur == 'd') || (cur == 'D') ||
	(cur == 'w') || (cur == 'W')) {
	xmlRegAtomType type = XML_REGEXP_ANYSPACE;

	switch (cur) {
	    case 's':
		type = XML_REGEXP_ANYSPACE;
		break;
	    case 'S':
		type = XML_REGEXP_NOTSPACE;
		break;
	    case 'i':
		type = XML_REGEXP_INITNAME;
		break;
	    case 'I':
		type = XML_REGEXP_NOTINITNAME;
		break;
	    case 'c':
		type = XML_REGEXP_NAMECHAR;
		break;
	    case 'C':
		type = XML_REGEXP_NOTNAMECHAR;
		break;
	    case 'd':
		type = XML_REGEXP_DECIMAL;
		break;
	    case 'D':
		type = XML_REGEXP_NOTDECIMAL;
		break;
	    case 'w':
		type = XML_REGEXP_REALCHAR;
		break;
	    case 'W':
		type = XML_REGEXP_NOTREALCHAR;
		break;
	}
	NEXT;
	if (ctxt->atom == NULL) {
	    ctxt->atom = xmlRegNewAtom(ctxt, type);
	} else if (ctxt->atom->type == XML_REGEXP_RANGES) {
	    xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
			       type, 0, 0, NULL);
	}
    } else {
	ERROR("Wrong escape sequence, misuse of character '\\'");
    }
}

/**
 * ```
 * [17]   charRange   ::=     seRange | XmlCharRef | XmlCharIncDash
 * [18]   seRange   ::=   charOrEsc '-' charOrEsc
 * [20]   charOrEsc   ::=   XmlChar | SingleCharEsc
 * [21]   XmlChar   ::=   [^\\#x2D\#x5B\#x5D]
 * [22]   XmlCharIncDash   ::=   [^\\#x5B\#x5D]
 * ```
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParseCharRange(xmlRegParserCtxtPtr ctxt) {
    int cur, len;
    int start = -1;
    int end = -1;

    if (CUR == '\0') {
        ERROR("Expecting ']'");
	return;
    }

    cur = CUR;
    if (cur == '\\') {
	NEXT;
	cur = CUR;
	switch (cur) {
	    case 'n': start = 0xA; break;
	    case 'r': start = 0xD; break;
	    case 't': start = 0x9; break;
	    case '\\': case '|': case '.': case '-': case '^': case '?':
	    case '*': case '+': case '{': case '}': case '(': case ')':
	    case '[': case ']':
		start = cur; break;
	    default:
		ERROR("Invalid escape value");
		return;
	}
	end = start;
        len = 1;
    } else if ((cur != 0x5B) && (cur != 0x5D)) {
        len = 4;
        end = start = xmlGetUTF8Char(ctxt->cur, &len);
        if (start < 0) {
            ERROR("Invalid UTF-8");
            return;
        }
    } else {
	ERROR("Expecting a char range");
	return;
    }
    /*
     * Since we are "inside" a range, we can assume ctxt->cur is past
     * the start of ctxt->string, and PREV should be safe
     */
    if ((start == '-') && (NXT(1) != ']') && (PREV != '[') && (PREV != '^')) {
	NEXTL(len);
	return;
    }
    NEXTL(len);
    cur = CUR;
    if ((cur != '-') || (NXT(1) == '[') || (NXT(1) == ']')) {
        xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
		              XML_REGEXP_CHARVAL, start, end, NULL);
	return;
    }
    NEXT;
    cur = CUR;
    if (cur == '\\') {
	NEXT;
	cur = CUR;
	switch (cur) {
	    case 'n': end = 0xA; break;
	    case 'r': end = 0xD; break;
	    case 't': end = 0x9; break;
	    case '\\': case '|': case '.': case '-': case '^': case '?':
	    case '*': case '+': case '{': case '}': case '(': case ')':
	    case '[': case ']':
		end = cur; break;
	    default:
		ERROR("Invalid escape value");
		return;
	}
        len = 1;
    } else if ((cur != '\0') && (cur != 0x5B) && (cur != 0x5D)) {
        len = 4;
        end = xmlGetUTF8Char(ctxt->cur, &len);
        if (end < 0) {
            ERROR("Invalid UTF-8");
            return;
        }
    } else {
	ERROR("Expecting the end of a char range");
	return;
    }

    /* TODO check that the values are acceptable character ranges for XML */
    if (end < start) {
	ERROR("End of range is before start of range");
    } else {
        NEXTL(len);
        xmlRegAtomAddRange(ctxt, ctxt->atom, ctxt->neg,
		           XML_REGEXP_CHARVAL, start, end, NULL);
    }
}

/**
 * [14]   posCharGroup ::= ( charRange | charClassEsc  )+
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParsePosCharGroup(xmlRegParserCtxtPtr ctxt) {
    do {
	if (CUR == '\\') {
	    xmlFAParseCharClassEsc(ctxt);
	} else {
	    xmlFAParseCharRange(ctxt);
	}
    } while ((CUR != ']') && (CUR != '-') &&
             (CUR != 0) && (ctxt->error == 0));
}

/**
 * [13]   charGroup    ::= posCharGroup | negCharGroup | charClassSub
 * [15]   negCharGroup ::= '^' posCharGroup
 * [16]   charClassSub ::= ( posCharGroup | negCharGroup ) '-' charClassExpr
 * [12]   charClassExpr ::= '[' charGroup ']'
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParseCharGroup(xmlRegParserCtxtPtr ctxt) {
    int neg = ctxt->neg;

    if (CUR == '^') {
	NEXT;
	ctxt->neg = !ctxt->neg;
	xmlFAParsePosCharGroup(ctxt);
	ctxt->neg = neg;
    }
    while ((CUR != ']') && (ctxt->error == 0)) {
	if ((CUR == '-') && (NXT(1) == '[')) {
	    NEXT;	/* eat the '-' */
	    NEXT;	/* eat the '[' */
	    ctxt->neg = 2;
	    xmlFAParseCharGroup(ctxt);
	    ctxt->neg = neg;
	    if (CUR == ']') {
		NEXT;
	    } else {
		ERROR("charClassExpr: ']' expected");
	    }
	    break;
	} else {
	    xmlFAParsePosCharGroup(ctxt);
	}
    }
}

/**
 * [11]   charClass   ::=     charClassEsc | charClassExpr
 * [12]   charClassExpr   ::=   '[' charGroup ']'
 *
 * @param ctxt  a regexp parser context
 */
static void
xmlFAParseCharClass(xmlRegParserCtxtPtr ctxt) {
    if (CUR == '[') {
	NEXT;
	ctxt->atom = xmlRegNewAtom(ctxt, XML_REGEXP_RANGES);
	if (ctxt->atom == NULL)
	    return;
	xmlFAParseCharGroup(ctxt);
	if (CUR == ']') {
	    NEXT;
	} else {
	    ERROR("xmlFAParseCharClass: ']' expected");
	}
    } else {
	xmlFAParseCharClassEsc(ctxt);
    }
}

/**
 * [8]   QuantExact   ::=   [0-9]+
 *
 * @param ctxt  a regexp parser context
 * @returns 0 if success or -1 in case of error
 */
static int
xmlFAParseQuantExact(xmlRegParserCtxtPtr ctxt) {
    int ret = 0;
    int ok = 0;
    int overflow = 0;

    while ((CUR >= '0') && (CUR <= '9')) {
        if (ret > INT_MAX / 10) {
            overflow = 1;
        } else {
            int digit = CUR - '0';

            ret *= 10;
            if (ret > INT_MAX - digit)
                overflow = 1;
            else
                ret += digit;
        }
	ok = 1;
	NEXT;
    }
    if ((ok != 1) || (overflow == 1)) {
	return(-1);
    }
    return(ret);
}

/**
 * [4]   quantifier   ::=   [?*+] | ( '{' quantity '}' )
 * [5]   quantity   ::=   quantRange | quantMin | QuantExact
 * [6]   quantRange   ::=   QuantExact ',' QuantExact
 * [7]   quantMin   ::=   QuantExact ','
 * [8]   QuantExact   ::=   [0-9]+
 *
 * @param ctxt  a regexp parser context
 */
static int
xmlFAParseQuantifier(xmlRegParserCtxtPtr ctxt) {
    int cur;

    cur = CUR;
    if ((cur == '?') || (cur == '*') || (cur == '+')) {
	if (ctxt->atom != NULL) {
	    if (cur == '?')
		ctxt->atom->quant = XML_REGEXP_QUANT_OPT;
	    else if (cur == '*')
		ctxt->atom->quant = XML_REGEXP_QUANT_MULT;
	    else if (cur == '+')
		ctxt->atom->quant = XML_REGEXP_QUANT_PLUS;
	}
	NEXT;
	return(1);
    }
    if (cur == '{') {
	int min = 0, max = 0;

	NEXT;
	cur = xmlFAParseQuantExact(ctxt);
	if (cur >= 0)
	    min = cur;
        else {
            ERROR("Improper quantifier");
        }
	if (CUR == ',') {
	    NEXT;
	    if (CUR == '}')
	        max = INT_MAX;
	    else {
	        cur = xmlFAParseQuantExact(ctxt);
	        if (cur >= 0)
		    max = cur;
		else {
		    ERROR("Improper quantifier");
		}
	    }
	}
	if (CUR == '}') {
	    NEXT;
	} else {
	    ERROR("Unterminated quantifier");
	}
	if (max == 0)
	    max = min;
	if (ctxt->atom != NULL) {
	    ctxt->atom->quant = XML_REGEXP_QUANT_RANGE;
	    ctxt->atom->min = min;
	    ctxt->atom->max = max;
	}
	return(1);
    }
    return(0);
}

/**
 * [9]   atom   ::=   Char | charClass | ( '(' regExp ')' )
 *
 * @param ctxt  a regexp parser context
 */
static int
xmlFAParseAtom(xmlRegParserCtxtPtr ctxt) {
    int codepoint, len;

    codepoint = xmlFAIsChar(ctxt);
    if (codepoint > 0) {
	ctxt->atom = xmlRegNewAtom(ctxt, XML_REGEXP_CHARVAL);
	if (ctxt->atom == NULL)
	    return(-1);
        len = 4;
        codepoint = xmlGetUTF8Char(ctxt->cur, &len);
        if (codepoint < 0) {
            ERROR("Invalid UTF-8");
            return(-1);
        }
	ctxt->atom->codepoint = codepoint;
	NEXTL(len);
	return(1);
    } else if (CUR == '|') {
	return(0);
    } else if (CUR == 0) {
	return(0);
    } else if (CUR == ')') {
	return(0);
    } else if (CUR == '(') {
	xmlRegStatePtr start, oldend, start0;

	NEXT;
        if (ctxt->depth >= 50) {
	    ERROR("xmlFAParseAtom: maximum nesting depth exceeded");
            return(-1);
        }
	/*
	 * this extra Epsilon transition is needed if we count with 0 allowed
	 * unfortunately this can't be known at that point
	 */
	xmlFAGenerateEpsilonTransition(ctxt, ctxt->state, NULL);
	start0 = ctxt->state;
	xmlFAGenerateEpsilonTransition(ctxt, ctxt->state, NULL);
	start = ctxt->state;
	oldend = ctxt->end;
	ctxt->end = NULL;
	ctxt->atom = NULL;
        ctxt->depth++;
	xmlFAParseRegExp(ctxt, 0);
        ctxt->depth--;
	if (CUR == ')') {
	    NEXT;
	} else {
	    ERROR("xmlFAParseAtom: expecting ')'");
	}
	ctxt->atom = xmlRegNewAtom(ctxt, XML_REGEXP_SUBREG);
	if (ctxt->atom == NULL)
	    return(-1);
	ctxt->atom->start = start;
	ctxt->atom->start0 = start0;
	ctxt->atom->stop = ctxt->state;
	ctxt->end = oldend;
	return(1);
    } else if ((CUR == '[') || (CUR == '\\') || (CUR == '.')) {
	xmlFAParseCharClass(ctxt);
	return(1);
    }
    return(0);
}

/**
 * [3]   piece   ::=   atom quantifier?
 *
 * @param ctxt  a regexp parser context
 */
static int
xmlFAParsePiece(xmlRegParserCtxtPtr ctxt) {
    int ret;

    ctxt->atom = NULL;
    ret = xmlFAParseAtom(ctxt);
    if (ret == 0)
	return(0);
    if (ctxt->atom == NULL) {
	ERROR("internal: no atom generated");
    }
    xmlFAParseQuantifier(ctxt);
    return(1);
}

/**
 * `to` is used to optimize by removing duplicate path in automata
 * in expressions like (a|b)(c|d)
 *
 * [2]   branch   ::=   piece*
 *
 * @param ctxt  a regexp parser context
 * @param to  optional target to the end of the branch
 */
static int
xmlFAParseBranch(xmlRegParserCtxtPtr ctxt, xmlRegStatePtr to) {
    xmlRegStatePtr previous;
    int ret;

    previous = ctxt->state;
    ret = xmlFAParsePiece(ctxt);
    if (ret == 0) {
        /* Empty branch */
	xmlFAGenerateEpsilonTransition(ctxt, previous, to);
    } else {
	if (xmlFAGenerateTransitions(ctxt, previous,
	        (CUR=='|' || CUR==')' || CUR==0) ? to : NULL,
                ctxt->atom) < 0) {
            xmlRegFreeAtom(ctxt->atom);
            ctxt->atom = NULL;
	    return(-1);
        }
	previous = ctxt->state;
	ctxt->atom = NULL;
    }
    while ((ret != 0) && (ctxt->error == 0)) {
	ret = xmlFAParsePiece(ctxt);
	if (ret != 0) {
	    if (xmlFAGenerateTransitions(ctxt, previous,
	            (CUR=='|' || CUR==')' || CUR==0) ? to : NULL,
                    ctxt->atom) < 0) {
                xmlRegFreeAtom(ctxt->atom);
                ctxt->atom = NULL;
                return(-1);
            }
	    previous = ctxt->state;
	    ctxt->atom = NULL;
	}
    }
    return(0);
}

/**
 * [1]   regExp   ::=     branch  ( '|' branch )*
 *
 * @param ctxt  a regexp parser context
 * @param top  is this the top-level expression ?
 */
static void
xmlFAParseRegExp(xmlRegParserCtxtPtr ctxt, int top) {
    xmlRegStatePtr start, end;

    /* if not top start should have been generated by an epsilon trans */
    start = ctxt->state;
    ctxt->end = NULL;
    xmlFAParseBranch(ctxt, NULL);
    if (top) {
	ctxt->state->type = XML_REGEXP_FINAL_STATE;
    }
    if (CUR != '|') {
	ctxt->end = ctxt->state;
	return;
    }
    end = ctxt->state;
    while ((CUR == '|') && (ctxt->error == 0)) {
	NEXT;
	ctxt->state = start;
	ctxt->end = NULL;
	xmlFAParseBranch(ctxt, end);
    }
    if (!top) {
	ctxt->state = end;
	ctxt->end = end;
    }
}

/************************************************************************
 *									*
 *			The basic API					*
 *									*
 ************************************************************************/

/**
 * No-op since 2.14.0.
 *
 * @deprecated Don't use.
 *
 * @param output  the file for the output debug
 * @param regexp  the compiled regexp
 */
void
xmlRegexpPrint(FILE *output ATTRIBUTE_UNUSED,
               xmlRegexp *regexp ATTRIBUTE_UNUSED) {
}

/**
 * Parses an XML Schemas regular expression.
 *
 * Parses a regular expression conforming to XML Schemas Part 2 Datatype
 * Appendix F and builds an automata suitable for testing strings against
 * that regular expression.
 *
 * @param regexp  a regular expression string
 * @returns the compiled expression or NULL in case of error
 */
xmlRegexp *
xmlRegexpCompile(const xmlChar *regexp) {
    xmlRegexpPtr ret = NULL;
    xmlRegParserCtxtPtr ctxt;

    if (regexp == NULL)
        return(NULL);

    ctxt = xmlRegNewParserCtxt(regexp);
    if (ctxt == NULL)
	return(NULL);

    /* initialize the parser */
    ctxt->state = xmlRegStatePush(ctxt);
    if (ctxt->state == NULL)
        goto error;
    ctxt->start = ctxt->state;
    ctxt->end = NULL;

    /* parse the expression building an automata */
    xmlFAParseRegExp(ctxt, 1);
    if (CUR != 0) {
	ERROR("xmlFAParseRegExp: extra characters");
    }
    if (ctxt->error != 0)
        goto error;
    ctxt->end = ctxt->state;
    ctxt->start->type = XML_REGEXP_START_STATE;
    ctxt->end->type = XML_REGEXP_FINAL_STATE;

    /* remove the Epsilon except for counted transitions */
    xmlFAEliminateEpsilonTransitions(ctxt);


    if (ctxt->error != 0)
        goto error;
    ret = xmlRegEpxFromParse(ctxt);

error:
    xmlRegFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Check if the regular expression matches a string.
 *
 * @param comp  the compiled regular expression
 * @param content  the value to check against the regular expression
 * @returns 1 if it matches, 0 if not and a negative value in case of error
 */
int
xmlRegexpExec(xmlRegexp *comp, const xmlChar *content) {
    if ((comp == NULL) || (content == NULL))
	return(-1);
    return(xmlFARegExec(comp, content));
}

/**
 * Check if the regular expression is deterministic.
 *
 * DTD and XML Schemas require a deterministic content model,
 * so the automaton compiled from the regex must be a DFA.
 *
 * The runtime of this function is quadratic in the number of
 * outgoing edges, causing serious worst-case performance issues.
 *
 * @deprecated: Internal function, don't use.
 *
 * @param comp  the compiled regular expression
 * @returns 1 if it yes, 0 if not and a negative value in case
 * of error
 */
int
xmlRegexpIsDeterminist(xmlRegexp *comp) {
    xmlAutomataPtr am;
    int ret;

    if (comp == NULL)
	return(-1);
    if (comp->determinist != -1)
	return(comp->determinist);

    am = xmlNewAutomata();
    if (am == NULL)
        return(-1);
    if (am->states != NULL) {
	int i;

	for (i = 0;i < am->nbStates;i++)
	    xmlRegFreeState(am->states[i]);
	xmlFree(am->states);
    }
    am->nbAtoms = comp->nbAtoms;
    am->atoms = comp->atoms;
    am->nbStates = comp->nbStates;
    am->states = comp->states;
    am->determinist = -1;
    am->flags = comp->flags;
    ret = xmlFAComputesDeterminism(am);
    am->atoms = NULL;
    am->states = NULL;
    xmlFreeAutomata(am);
    comp->determinist = ret;
    return(ret);
}

/**
 * Free a regexp.
 *
 * @param regexp  the regexp
 */
void
xmlRegFreeRegexp(xmlRegexp *regexp) {
    int i;
    if (regexp == NULL)
	return;

    if (regexp->string != NULL)
	xmlFree(regexp->string);
    if (regexp->states != NULL) {
	for (i = 0;i < regexp->nbStates;i++)
	    xmlRegFreeState(regexp->states[i]);
	xmlFree(regexp->states);
    }
    if (regexp->atoms != NULL) {
	for (i = 0;i < regexp->nbAtoms;i++)
	    xmlRegFreeAtom(regexp->atoms[i]);
	xmlFree(regexp->atoms);
    }
    if (regexp->counters != NULL)
	xmlFree(regexp->counters);
    if (regexp->compact != NULL)
	xmlFree(regexp->compact);
    if (regexp->transdata != NULL)
	xmlFree(regexp->transdata);
    if (regexp->stringMap != NULL) {
	for (i = 0; i < regexp->nbstrings;i++)
	    xmlFree(regexp->stringMap[i]);
	xmlFree(regexp->stringMap);
    }

    xmlFree(regexp);
}

/************************************************************************
 *									*
 *			The Automata interface				*
 *									*
 ************************************************************************/

/**
 * Create a new automata
 *
 * @deprecated Internal function, don't use.
 *
 * @returns the new object or NULL in case of failure
 */
xmlAutomata *
xmlNewAutomata(void) {
    xmlAutomataPtr ctxt;

    ctxt = xmlRegNewParserCtxt(NULL);
    if (ctxt == NULL)
	return(NULL);

    /* initialize the parser */
    ctxt->state = xmlRegStatePush(ctxt);
    if (ctxt->state == NULL) {
	xmlFreeAutomata(ctxt);
	return(NULL);
    }
    ctxt->start = ctxt->state;
    ctxt->end = NULL;

    ctxt->start->type = XML_REGEXP_START_STATE;
    ctxt->flags = 0;

    return(ctxt);
}

/**
 * Free an automata
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 */
void
xmlFreeAutomata(xmlAutomata *am) {
    if (am == NULL)
	return;
    xmlRegFreeParserCtxt(am);
}

/**
 * Set some flags on the automata
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param flags  a set of internal flags
 */
void
xmlAutomataSetFlags(xmlAutomata *am, int flags) {
    if (am == NULL)
	return;
    am->flags |= flags;
}

/**
 * Initial state lookup
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @returns the initial state of the automata
 */
xmlAutomataState *
xmlAutomataGetInitState(xmlAutomata *am) {
    if (am == NULL)
	return(NULL);
    return(am->start);
}

/**
 * Makes that state a final state
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param state  a state in this automata
 * @returns 0 or -1 in case of error
 */
int
xmlAutomataSetFinalState(xmlAutomata *am, xmlAutomataState *state) {
    if ((am == NULL) || (state == NULL))
	return(-1);
    state->type = XML_REGEXP_FINAL_STATE;
    return(0);
}

/**
 * Add a transition.
 *
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by the value of `token`
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the input string associated to that transition
 * @param data  data passed to the callback function if the transition is activated
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewTransition(xmlAutomata *am, xmlAutomataState *from,
			 xmlAutomataState *to, const xmlChar *token,
			 void *data) {
    xmlRegAtomPtr atom;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
        return(NULL);
    atom->data = data;
    atom->valuep = xmlStrdup(token);
    if (atom->valuep == NULL) {
        xmlRegFreeAtom(atom);
        xmlRegexpErrMemory(am);
        return(NULL);
    }

    if (xmlFAGenerateTransitions(am, from, to, atom) < 0) {
        xmlRegFreeAtom(atom);
	return(NULL);
    }
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by the value of `token`
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the first input string associated to that transition
 * @param token2  the second input string associated to that transition
 * @param data  data passed to the callback function if the transition is activated
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewTransition2(xmlAutomata *am, xmlAutomataState *from,
			  xmlAutomataState *to, const xmlChar *token,
			  const xmlChar *token2, void *data) {
    xmlRegAtomPtr atom;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    atom->data = data;
    if ((token2 == NULL) || (*token2 == 0)) {
	atom->valuep = xmlStrdup(token);
    } else {
	int lenn, lenp;
	xmlChar *str;

	lenn = strlen((char *) token2);
	lenp = strlen((char *) token);

	str = xmlMalloc(lenn + lenp + 2);
	if (str == NULL) {
	    xmlRegFreeAtom(atom);
	    return(NULL);
	}
	memcpy(&str[0], token, lenp);
	str[lenp] = '|';
	memcpy(&str[lenp + 1], token2, lenn);
	str[lenn + lenp + 1] = 0;

	atom->valuep = str;
    }

    if (xmlFAGenerateTransitions(am, from, to, atom) < 0) {
        xmlRegFreeAtom(atom);
	return(NULL);
    }
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by any value except (`token`,`token2`)
 * Note that if `token2` is not NULL, then (X, NULL) won't match to follow
 * the semantic of XSD \#\#other
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the first input string associated to that transition
 * @param token2  the second input string associated to that transition
 * @param data  data passed to the callback function if the transition is activated
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewNegTrans(xmlAutomata *am, xmlAutomataState *from,
		       xmlAutomataState *to, const xmlChar *token,
		       const xmlChar *token2, void *data) {
    xmlRegAtomPtr atom;
    xmlChar err_msg[200];

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    atom->data = data;
    atom->neg = 1;
    if ((token2 == NULL) || (*token2 == 0)) {
	atom->valuep = xmlStrdup(token);
    } else {
	int lenn, lenp;
	xmlChar *str;

	lenn = strlen((char *) token2);
	lenp = strlen((char *) token);

	str = xmlMalloc(lenn + lenp + 2);
	if (str == NULL) {
	    xmlRegFreeAtom(atom);
	    return(NULL);
	}
	memcpy(&str[0], token, lenp);
	str[lenp] = '|';
	memcpy(&str[lenp + 1], token2, lenn);
	str[lenn + lenp + 1] = 0;

	atom->valuep = str;
    }
    snprintf((char *) err_msg, 199, "not %s", (const char *) atom->valuep);
    err_msg[199] = 0;
    atom->valuep2 = xmlStrdup(err_msg);

    if (xmlFAGenerateTransitions(am, from, to, atom) < 0) {
        xmlRegFreeAtom(atom);
	return(NULL);
    }
    am->negs++;
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by a succession of input of value `token` and `token2` and
 * whose number is between `min` and `max`
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the input string associated to that transition
 * @param token2  the second input string associated to that transition
 * @param min  the minimum successive occurrences of token
 * @param max  the maximum successive occurrences of token
 * @param data  data associated to the transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewCountTrans2(xmlAutomata *am, xmlAutomataState *from,
			 xmlAutomataState *to, const xmlChar *token,
			 const xmlChar *token2,
			 int min, int max, void *data) {
    xmlRegAtomPtr atom;
    int counter;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    if (min < 0)
	return(NULL);
    if ((max < min) || (max < 1))
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    if ((token2 == NULL) || (*token2 == 0)) {
	atom->valuep = xmlStrdup(token);
        if (atom->valuep == NULL)
            goto error;
    } else {
	int lenn, lenp;
	xmlChar *str;

	lenn = strlen((char *) token2);
	lenp = strlen((char *) token);

	str = xmlMalloc(lenn + lenp + 2);
	if (str == NULL)
	    goto error;
	memcpy(&str[0], token, lenp);
	str[lenp] = '|';
	memcpy(&str[lenp + 1], token2, lenn);
	str[lenn + lenp + 1] = 0;

	atom->valuep = str;
    }
    atom->data = data;
    if (min == 0)
	atom->min = 1;
    else
	atom->min = min;
    atom->max = max;

    /*
     * associate a counter to the transition.
     */
    counter = xmlRegGetCounter(am);
    if (counter < 0)
        goto error;
    am->counters[counter].min = min;
    am->counters[counter].max = max;

    /* xmlFAGenerateTransitions(am, from, to, atom); */
    if (to == NULL) {
	to = xmlRegStatePush(am);
        if (to == NULL)
            goto error;
    }
    xmlRegStateAddTrans(am, from, atom, to, counter, -1);
    if (xmlRegAtomPush(am, atom) < 0)
        goto error;
    am->state = to;

    if (to == NULL)
	to = am->state;
    if (to == NULL)
	return(NULL);
    if (min == 0)
	xmlFAGenerateEpsilonTransition(am, from, to);
    return(to);

error:
    xmlRegFreeAtom(atom);
    return(NULL);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by a succession of input of value `token` and whose number
 * is between `min` and `max`
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the input string associated to that transition
 * @param min  the minimum successive occurrences of token
 * @param max  the maximum successive occurrences of token
 * @param data  data associated to the transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewCountTrans(xmlAutomata *am, xmlAutomataState *from,
			 xmlAutomataState *to, const xmlChar *token,
			 int min, int max, void *data) {
    xmlRegAtomPtr atom;
    int counter;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    if (min < 0)
	return(NULL);
    if ((max < min) || (max < 1))
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    atom->valuep = xmlStrdup(token);
    if (atom->valuep == NULL)
        goto error;
    atom->data = data;
    if (min == 0)
	atom->min = 1;
    else
	atom->min = min;
    atom->max = max;

    /*
     * associate a counter to the transition.
     */
    counter = xmlRegGetCounter(am);
    if (counter < 0)
        goto error;
    am->counters[counter].min = min;
    am->counters[counter].max = max;

    /* xmlFAGenerateTransitions(am, from, to, atom); */
    if (to == NULL) {
	to = xmlRegStatePush(am);
        if (to == NULL)
            goto error;
    }
    xmlRegStateAddTrans(am, from, atom, to, counter, -1);
    if (xmlRegAtomPush(am, atom) < 0)
        goto error;
    am->state = to;

    if (to == NULL)
	to = am->state;
    if (to == NULL)
	return(NULL);
    if (min == 0)
	xmlFAGenerateEpsilonTransition(am, from, to);
    return(to);

error:
    xmlRegFreeAtom(atom);
    return(NULL);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by a succession of input of value `token` and `token2` and whose
 * number is between `min` and `max`, moreover that transition can only be
 * crossed once.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the input string associated to that transition
 * @param token2  the second input string associated to that transition
 * @param min  the minimum successive occurrences of token
 * @param max  the maximum successive occurrences of token
 * @param data  data associated to the transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewOnceTrans2(xmlAutomata *am, xmlAutomataState *from,
			 xmlAutomataState *to, const xmlChar *token,
			 const xmlChar *token2,
			 int min, int max, void *data) {
    xmlRegAtomPtr atom;
    int counter;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    if (min < 1)
	return(NULL);
    if (max < min)
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    if ((token2 == NULL) || (*token2 == 0)) {
	atom->valuep = xmlStrdup(token);
        if (atom->valuep == NULL)
            goto error;
    } else {
	int lenn, lenp;
	xmlChar *str;

	lenn = strlen((char *) token2);
	lenp = strlen((char *) token);

	str = xmlMalloc(lenn + lenp + 2);
	if (str == NULL)
	    goto error;
	memcpy(&str[0], token, lenp);
	str[lenp] = '|';
	memcpy(&str[lenp + 1], token2, lenn);
	str[lenn + lenp + 1] = 0;

	atom->valuep = str;
    }
    atom->data = data;
    atom->quant = XML_REGEXP_QUANT_ONCEONLY;
    atom->min = min;
    atom->max = max;
    /*
     * associate a counter to the transition.
     */
    counter = xmlRegGetCounter(am);
    if (counter < 0)
        goto error;
    am->counters[counter].min = 1;
    am->counters[counter].max = 1;

    /* xmlFAGenerateTransitions(am, from, to, atom); */
    if (to == NULL) {
	to = xmlRegStatePush(am);
        if (to == NULL)
            goto error;
    }
    xmlRegStateAddTrans(am, from, atom, to, counter, -1);
    if (xmlRegAtomPush(am, atom) < 0)
        goto error;
    am->state = to;
    return(to);

error:
    xmlRegFreeAtom(atom);
    return(NULL);
}



/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a transition from the `from` state to the target state
 * activated by a succession of input of value `token` and whose number
 * is between `min` and `max`, moreover that transition can only be crossed
 * once.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param token  the input string associated to that transition
 * @param min  the minimum successive occurrences of token
 * @param max  the maximum successive occurrences of token
 * @param data  data associated to the transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewOnceTrans(xmlAutomata *am, xmlAutomataState *from,
			 xmlAutomataState *to, const xmlChar *token,
			 int min, int max, void *data) {
    xmlRegAtomPtr atom;
    int counter;

    if ((am == NULL) || (from == NULL) || (token == NULL))
	return(NULL);
    if (min < 1)
	return(NULL);
    if (max < min)
	return(NULL);
    atom = xmlRegNewAtom(am, XML_REGEXP_STRING);
    if (atom == NULL)
	return(NULL);
    atom->valuep = xmlStrdup(token);
    atom->data = data;
    atom->quant = XML_REGEXP_QUANT_ONCEONLY;
    atom->min = min;
    atom->max = max;
    /*
     * associate a counter to the transition.
     */
    counter = xmlRegGetCounter(am);
    if (counter < 0)
        goto error;
    am->counters[counter].min = 1;
    am->counters[counter].max = 1;

    /* xmlFAGenerateTransitions(am, from, to, atom); */
    if (to == NULL) {
	to = xmlRegStatePush(am);
        if (to == NULL)
            goto error;
    }
    xmlRegStateAddTrans(am, from, atom, to, counter, -1);
    if (xmlRegAtomPush(am, atom) < 0)
        goto error;
    am->state = to;
    return(to);

error:
    xmlRegFreeAtom(atom);
    return(NULL);
}

/**
 * Create a new disconnected state in the automata
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @returns the new state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewState(xmlAutomata *am) {
    if (am == NULL)
	return(NULL);
    return(xmlRegStatePush(am));
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds an epsilon transition from the `from` state to the
 * target state
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewEpsilon(xmlAutomata *am, xmlAutomataState *from,
		      xmlAutomataState *to) {
    if ((am == NULL) || (from == NULL))
	return(NULL);
    xmlFAGenerateEpsilonTransition(am, from, to);
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds a an ALL transition from the `from` state to the
 * target state. That transition is an epsilon transition allowed only when
 * all transitions from the `from` node have been activated.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param lax  allow to transition if not all all transitions have been activated
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewAllTrans(xmlAutomata *am, xmlAutomataState *from,
		       xmlAutomataState *to, int lax) {
    if ((am == NULL) || (from == NULL))
	return(NULL);
    xmlFAGenerateAllTransition(am, from, to, lax);
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * Create a new counter
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param min  the minimal value on the counter
 * @param max  the maximal value on the counter
 * @returns the counter number or -1 in case of error
 */
int
xmlAutomataNewCounter(xmlAutomata *am, int min, int max) {
    int ret;

    if (am == NULL)
	return(-1);

    ret = xmlRegGetCounter(am);
    if (ret < 0)
	return(-1);
    am->counters[ret].min = min;
    am->counters[ret].max = max;
    return(ret);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds an epsilon transition from the `from` state to the target state
 * which will increment the counter provided
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param counter  the counter associated to that transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewCountedTrans(xmlAutomata *am, xmlAutomataState *from,
		xmlAutomataState *to, int counter) {
    if ((am == NULL) || (from == NULL) || (counter < 0))
	return(NULL);
    xmlFAGenerateCountedEpsilonTransition(am, from, to, counter);
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * If `to` is NULL, this creates first a new target state in the automata
 * and then adds an epsilon transition from the `from` state to the target state
 * which will be allowed only if the counter is within the right range.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @param from  the starting point of the transition
 * @param to  the target point of the transition or NULL
 * @param counter  the counter associated to that transition
 * @returns the target state or NULL in case of error
 */
xmlAutomataState *
xmlAutomataNewCounterTrans(xmlAutomata *am, xmlAutomataState *from,
		xmlAutomataState *to, int counter) {
    if ((am == NULL) || (from == NULL) || (counter < 0))
	return(NULL);
    xmlFAGenerateCountedTransition(am, from, to, counter);
    if (to == NULL)
	return(am->state);
    return(to);
}

/**
 * Compile the automata into a Reg Exp ready for being executed.
 * The automata should be free after this point.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @returns the compiled regexp or NULL in case of error
 */
xmlRegexp *
xmlAutomataCompile(xmlAutomata *am) {
    xmlRegexpPtr ret;

    if ((am == NULL) || (am->error != 0)) return(NULL);
    xmlFAEliminateEpsilonTransitions(am);
    if (am->error != 0)
        return(NULL);
    /* xmlFAComputesDeterminism(am); */
    ret = xmlRegEpxFromParse(am);

    return(ret);
}

/**
 * Checks if an automata is determinist.
 *
 * @deprecated Internal function, don't use.
 *
 * @param am  an automata
 * @returns 1 if true, 0 if not, and -1 in case of error
 */
int
xmlAutomataIsDeterminist(xmlAutomata *am) {
    int ret;

    if (am == NULL)
	return(-1);

    ret = xmlFAComputesDeterminism(am);
    return(ret);
}

#endif /* LIBXML_REGEXP_ENABLED */
