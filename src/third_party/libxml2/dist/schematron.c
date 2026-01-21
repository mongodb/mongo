/*
 * schematron.c : implementation of the Schematron schema validity checking
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

/*
 * TODO:
 * + double check the semantic, especially
 *        - multiple rules applying in a single pattern/node
 *        - the semantic of libxml2 patterns vs. XSLT production referenced
 *          by the spec.
 * + export of results in SVRL
 * + full parsing and coverage of the spec, conformance of the input to the
 *   spec
 * + divergences between the draft and the ISO proposed standard :-(
 * + hook and test include
 * + try and compare with the XSLT version
 */

#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_SCHEMATRON_ENABLED

#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/pattern.h>
#include <libxml/schematron.h>

#include "private/error.h"

#define SCHEMATRON_PARSE_OPTIONS XML_PARSE_NOENT

#define SCT_OLD_NS BAD_CAST "http://www.ascc.net/xml/schematron"

#define XML_SCHEMATRON_NS BAD_CAST "http://purl.oclc.org/dsdl/schematron"


static const xmlChar *xmlSchematronNs = XML_SCHEMATRON_NS;
static const xmlChar *xmlOldSchematronNs = SCT_OLD_NS;

#define IS_SCHEMATRON(node, elem)                                       \
   ((node != NULL) && (node->type == XML_ELEMENT_NODE ) &&              \
    (node->ns != NULL) &&                                               \
    (xmlStrEqual(node->name, (const xmlChar *) elem)) &&                \
    ((xmlStrEqual(node->ns->href, xmlSchematronNs)) ||                  \
     (xmlStrEqual(node->ns->href, xmlOldSchematronNs))))

#define NEXT_SCHEMATRON(node)                                           \
   while (node != NULL) {                                               \
       if ((node->type == XML_ELEMENT_NODE ) && (node->ns != NULL) &&   \
           ((xmlStrEqual(node->ns->href, xmlSchematronNs)) ||           \
            (xmlStrEqual(node->ns->href, xmlOldSchematronNs))))         \
           break;                                                       \
       node = node->next;                                               \
   }

typedef enum {
    XML_SCHEMATRON_ASSERT=1,
    XML_SCHEMATRON_REPORT=2
} xmlSchematronTestType;

/**
 * A Schematron let variable
 */
typedef struct _xmlSchematronLet xmlSchematronLet;
typedef xmlSchematronLet *xmlSchematronLetPtr;
struct _xmlSchematronLet {
    xmlSchematronLetPtr next; /* the next let variable in the list */
    xmlChar *name;            /* the name of the variable */
    xmlXPathCompExprPtr comp; /* the compiled expression */
};

/**
 * A Schematrons test, either an assert or a report
 */
typedef struct _xmlSchematronTest xmlSchematronTest;
typedef xmlSchematronTest *xmlSchematronTestPtr;
struct _xmlSchematronTest {
    xmlSchematronTestPtr next;  /* the next test in the list */
    xmlSchematronTestType type; /* the test type */
    xmlNodePtr node;            /* the node in the tree */
    xmlChar *test;              /* the expression to test */
    xmlXPathCompExprPtr comp;   /* the compiled expression */
    xmlChar *report;            /* the message to report */
};

/**
 * A Schematrons rule
 */
typedef struct _xmlSchematronRule xmlSchematronRule;
typedef xmlSchematronRule *xmlSchematronRulePtr;
struct _xmlSchematronRule {
    xmlSchematronRulePtr next;  /* the next rule in the list */
    xmlSchematronRulePtr patnext;/* the next rule in the pattern list */
    xmlNodePtr node;            /* the node in the tree */
    xmlChar *context;           /* the context evaluation rule */
    xmlSchematronTestPtr tests; /* the list of tests */
    xmlPatternPtr pattern;      /* the compiled pattern associated */
    xmlChar *report;            /* the message to report */
    xmlSchematronLetPtr lets;   /* the list of let variables */
};

/**
 * A Schematrons pattern
 */
typedef struct _xmlSchematronPattern xmlSchematronPattern;
typedef xmlSchematronPattern *xmlSchematronPatternPtr;
struct _xmlSchematronPattern {
    xmlSchematronPatternPtr next;/* the next pattern in the list */
    xmlSchematronRulePtr rules; /* the list of rules */
    xmlChar *name;              /* the name of the pattern */
};

/**
 * A Schematrons definition
 */
struct _xmlSchematron {
    const xmlChar *name;        /* schema name */
    int preserve;               /* was the document passed by the user */
    xmlDocPtr doc;              /* pointer to the parsed document */
    int flags;                  /* specific to this schematron */

    void *_private;             /* unused by the library */
    xmlDictPtr dict;            /* the dictionary used internally */

    const xmlChar *title;       /* the title if any */

    int nbNs;                   /* the number of namespaces */

    int nbPattern;              /* the number of patterns */
    xmlSchematronPatternPtr patterns;/* the patterns found */
    xmlSchematronRulePtr rules; /* the rules gathered */
    int nbNamespaces;           /* number of namespaces in the array */
    int maxNamespaces;          /* size of the array */
    const xmlChar **namespaces; /* the array of namespaces */
};

/**
 * A Schematrons validation context
 */
struct _xmlSchematronValidCtxt {
    int type;
    int flags;                  /* an or of xmlSchematronValidOptions */

    xmlDictPtr dict;
    int nberrors;
    int err;

    xmlSchematronPtr schema;
    xmlXPathContextPtr xctxt;

    FILE *outputFile;           /* if using XML_SCHEMATRON_OUT_FILE */
    xmlBufferPtr outputBuffer;  /* if using XML_SCHEMATRON_OUT_BUFFER */
#ifdef LIBXML_OUTPUT_ENABLED
    xmlOutputWriteCallback iowrite; /* if using XML_SCHEMATRON_OUT_IO */
    xmlOutputCloseCallback  ioclose;
#endif
    void *ioctx;

    /* error reporting data */
    void *userData;                      /* user specific data block */
    xmlSchematronValidityErrorFunc error;/* the callback in case of errors */
    xmlSchematronValidityWarningFunc warning;/* callback in case of warning */
    xmlStructuredErrorFunc serror;       /* the structured function */
};

struct _xmlSchematronParserCtxt {
    int type;
    const xmlChar *URL;
    xmlDocPtr doc;
    int preserve;               /* Whether the doc should be freed  */
    const char *buffer;
    int size;

    xmlDictPtr dict;            /* dictionary for interned string names */

    int nberrors;
    int err;
    xmlXPathContextPtr xctxt;   /* the XPath context used for compilation */
    xmlSchematronPtr schema;

    int nbNamespaces;           /* number of namespaces in the array */
    int maxNamespaces;          /* size of the array */
    const xmlChar **namespaces; /* the array of namespaces */

    int nbIncludes;             /* number of includes in the array */
    int maxIncludes;            /* size of the array */
    xmlNodePtr *includes;       /* the array of includes */

    /* error reporting data */
    void *userData;                      /* user specific data block */
    xmlSchematronValidityErrorFunc error;/* the callback in case of errors */
    xmlSchematronValidityWarningFunc warning;/* callback in case of warning */
    xmlStructuredErrorFunc serror;       /* the structured function */
};

#define XML_STRON_CTXT_PARSER 1
#define XML_STRON_CTXT_VALIDATOR 2

/************************************************************************
 *                                                                      *
 *                      Error reporting                                 *
 *                                                                      *
 ************************************************************************/

/**
 * Handle an out of memory condition
 *
 * @param ctxt  parser context
 */
static void
xmlSchematronPErrMemory(xmlSchematronParserCtxtPtr ctxt)
{
    if (ctxt != NULL)
        ctxt->nberrors++;
    xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_SCHEMASP, NULL);
}

/**
 * Handle a parser error
 *
 * @param ctxt  the parsing context
 * @param node  the context node
 * @param error  the error code
 * @param msg  the error message
 * @param str1  extra data
 * @param str2  extra data
 */
static void LIBXML_ATTR_FORMAT(4,0)
xmlSchematronPErr(xmlSchematronParserCtxtPtr ctxt, xmlNodePtr node, int error,
              const char *msg, const xmlChar * str1, const xmlChar * str2)
{
    xmlGenericErrorFunc channel = NULL;
    xmlStructuredErrorFunc schannel = NULL;
    void *data = NULL;
    int res;

    if (ctxt != NULL) {
        ctxt->nberrors++;
        channel = ctxt->error;
        data = ctxt->userData;
        schannel = ctxt->serror;
    }

    if ((channel == NULL) && (schannel == NULL)) {
        channel = xmlGenericError;
        data = xmlGenericErrorContext;
    }

    res = xmlRaiseError(schannel, channel, data, ctxt, node,
                        XML_FROM_SCHEMASP, error, XML_ERR_ERROR, NULL, 0,
                        (const char *) str1, (const char *) str2, NULL, 0, 0,
                        msg, str1, str2);
    if (res < 0)
        xmlSchematronPErrMemory(ctxt);
}

/**
 * Handle an out of memory condition
 *
 * @param ctxt  validation context
 */
static void
xmlSchematronVErrMemory(xmlSchematronValidCtxtPtr ctxt)
{
    if (ctxt != NULL) {
        ctxt->nberrors++;
        ctxt->err = XML_SCHEMAV_INTERNAL;
    }
    xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_SCHEMASV, NULL);
}

/**
 * Handle a validation error
 *
 * @param ctxt  validation context
 * @param error  the error code
 * @param msg  the error message
 * @param str1  extra data
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlSchematronVErr(xmlSchematronValidCtxtPtr ctxt, int error,
                  const char *msg, const xmlChar * str1)
{
    xmlGenericErrorFunc channel = NULL;
    xmlStructuredErrorFunc schannel = NULL;
    void *data = NULL;
    int res;

    if (ctxt != NULL) {
        ctxt->nberrors++;
        channel = ctxt->error;
        data = ctxt->userData;
        schannel = ctxt->serror;
    }

    if ((channel == NULL) && (schannel == NULL)) {
        channel = xmlGenericError;
        data = xmlGenericErrorContext;
    }

    res = xmlRaiseError(schannel, channel, data, ctxt, NULL,
                        XML_FROM_SCHEMASV, error, XML_ERR_ERROR, NULL, 0,
                        (const char *) str1, NULL, NULL, 0, 0,
                        msg, str1);
    if (res < 0)
        xmlSchematronVErrMemory(ctxt);
}

/************************************************************************
 *                                                                      *
 *              Parsing and compilation of the Schematrontrons          *
 *                                                                      *
 ************************************************************************/

/**
 * Add a test to a schematron
 *
 * @param ctxt  the schema parsing context
 * @param type  the type of test
 * @param rule  the parent rule
 * @param node  the node hosting the test
 * @param test  the associated test
 * @param report  the associated report string
 * @returns the new pointer or NULL in case of error
 */
static xmlSchematronTestPtr
xmlSchematronAddTest(xmlSchematronParserCtxtPtr ctxt,
                     xmlSchematronTestType type,
                     xmlSchematronRulePtr rule,
                     xmlNodePtr node, xmlChar *test, xmlChar *report)
{
    xmlSchematronTestPtr ret;
    xmlXPathCompExprPtr comp;

    if ((ctxt == NULL) || (rule == NULL) || (node == NULL) ||
        (test == NULL))
        return(NULL);

    /*
     * try first to compile the test expression
     */
    comp = xmlXPathCtxtCompile(ctxt->xctxt, test);
    if (comp == NULL) {
        xmlSchematronPErr(ctxt, node,
            XML_SCHEMAP_NOROOT,
            "Failed to compile test expression %s",
            test, NULL);
        return(NULL);
    }

    ret = (xmlSchematronTestPtr) xmlMalloc(sizeof(xmlSchematronTest));
    if (ret == NULL) {
        xmlSchematronPErrMemory(ctxt);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronTest));
    ret->type = type;
    ret->node = node;
    ret->test = test;
    ret->comp = comp;
    ret->report = report;
    ret->next = NULL;
    if (rule->tests == NULL) {
        rule->tests = ret;
    } else {
        xmlSchematronTestPtr prev = rule->tests;

        while (prev->next != NULL)
             prev = prev->next;
        prev->next = ret;
    }
    return (ret);
}

/**
 * Free a list of tests.
 *
 * @param tests  a list of tests
 */
static void
xmlSchematronFreeTests(xmlSchematronTestPtr tests) {
    xmlSchematronTestPtr next;

    while (tests != NULL) {
        next = tests->next;
        if (tests->test != NULL)
            xmlFree(tests->test);
        if (tests->comp != NULL)
            xmlXPathFreeCompExpr(tests->comp);
        if (tests->report != NULL)
            xmlFree(tests->report);
        xmlFree(tests);
        tests = next;
    }
}

/**
 * Free a list of let variables.
 *
 * @param lets  a list of let variables
 */
static void
xmlSchematronFreeLets(xmlSchematronLetPtr lets) {
    xmlSchematronLetPtr next;

    while (lets != NULL) {
        next = lets->next;
        if (lets->name != NULL)
            xmlFree(lets->name);
        if (lets->comp != NULL)
            xmlXPathFreeCompExpr(lets->comp);
        xmlFree(lets);
        lets = next;
    }
}

/**
 * Add a rule to a schematron
 *
 * @param ctxt  the schema parsing context
 * @param schema  a schema structure
 * @param pat  a pattern
 * @param node  the node hosting the rule
 * @param context  the associated context string
 * @param report  the associated report string
 * @returns the new pointer or NULL in case of error
 */
static xmlSchematronRulePtr
xmlSchematronAddRule(xmlSchematronParserCtxtPtr ctxt, xmlSchematronPtr schema,
                     xmlSchematronPatternPtr pat, xmlNodePtr node,
                     xmlChar *context, xmlChar *report)
{
    xmlSchematronRulePtr ret;
    xmlPatternPtr pattern;

    if ((ctxt == NULL) || (schema == NULL) || (node == NULL) ||
        (context == NULL))
        return(NULL);

    /*
     * Try first to compile the pattern
     */
    pattern = xmlPatterncompile(context, ctxt->dict, XML_PATTERN_XPATH,
                                ctxt->namespaces);
    if (pattern == NULL) {
        xmlSchematronPErr(ctxt, node,
            XML_SCHEMAP_NOROOT,
            "Failed to compile context expression %s",
            context, NULL);
    }

    ret = (xmlSchematronRulePtr) xmlMalloc(sizeof(xmlSchematronRule));
    if (ret == NULL) {
        xmlSchematronPErrMemory(ctxt);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronRule));
    ret->node = node;
    ret->context = context;
    ret->pattern = pattern;
    ret->report = report;
    ret->next = NULL;
    ret->lets = NULL;
    if (schema->rules == NULL) {
        schema->rules = ret;
    } else {
        xmlSchematronRulePtr prev = schema->rules;

        while (prev->next != NULL)
             prev = prev->next;
        prev->next = ret;
    }
    ret->patnext = NULL;
    if (pat->rules == NULL) {
        pat->rules = ret;
    } else {
        xmlSchematronRulePtr prev = pat->rules;

        while (prev->patnext != NULL)
             prev = prev->patnext;
        prev->patnext = ret;
    }
    return (ret);
}

/**
 * Free a list of rules.
 *
 * @param rules  a list of rules
 */
static void
xmlSchematronFreeRules(xmlSchematronRulePtr rules) {
    xmlSchematronRulePtr next;

    while (rules != NULL) {
        next = rules->next;
        if (rules->tests)
            xmlSchematronFreeTests(rules->tests);
        if (rules->context != NULL)
            xmlFree(rules->context);
        if (rules->pattern)
            xmlFreePattern(rules->pattern);
        if (rules->report != NULL)
            xmlFree(rules->report);
        if (rules->lets != NULL)
            xmlSchematronFreeLets(rules->lets);
        xmlFree(rules);
        rules = next;
    }
}

/**
 * Add a pattern to a schematron
 *
 * @param ctxt  the schema parsing context
 * @param schema  a schema structure
 * @param node  the node hosting the pattern
 * @param name  the name of the pattern
 * @returns the new pointer or NULL in case of error
 */
static xmlSchematronPatternPtr
xmlSchematronAddPattern(xmlSchematronParserCtxtPtr ctxt,
                     xmlSchematronPtr schema, xmlNodePtr node, xmlChar *name)
{
    xmlSchematronPatternPtr ret;

    if ((ctxt == NULL) || (schema == NULL) || (node == NULL) || (name == NULL))
        return(NULL);

    ret = (xmlSchematronPatternPtr) xmlMalloc(sizeof(xmlSchematronPattern));
    if (ret == NULL) {
        xmlSchematronPErrMemory(ctxt);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronPattern));
    ret->name = name;
    ret->next = NULL;
    if (schema->patterns == NULL) {
        schema->patterns = ret;
    } else {
        xmlSchematronPatternPtr prev = schema->patterns;

        while (prev->next != NULL)
             prev = prev->next;
        prev->next = ret;
    }
    return (ret);
}

/**
 * Free a list of patterns.
 *
 * @param patterns  a list of patterns
 */
static void
xmlSchematronFreePatterns(xmlSchematronPatternPtr patterns) {
    xmlSchematronPatternPtr next;

    while (patterns != NULL) {
        next = patterns->next;
        if (patterns->name != NULL)
            xmlFree(patterns->name);
        xmlFree(patterns);
        patterns = next;
    }
}

/**
 * Allocate a new Schematron structure.
 *
 * @param ctxt  a schema validation context
 * @returns the newly allocated structure or NULL in case or error
 */
static xmlSchematronPtr
xmlSchematronNewSchematron(xmlSchematronParserCtxtPtr ctxt)
{
    xmlSchematronPtr ret;

    ret = (xmlSchematronPtr) xmlMalloc(sizeof(xmlSchematron));
    if (ret == NULL) {
        xmlSchematronPErrMemory(ctxt);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematron));
    ret->dict = ctxt->dict;
    xmlDictReference(ret->dict);

    return (ret);
}

/**
 * Deallocate a Schematron structure.
 *
 * @param schema  a schema structure
 */
void
xmlSchematronFree(xmlSchematron *schema)
{
    if (schema == NULL)
        return;

    if ((schema->doc != NULL) && (!(schema->preserve)))
        xmlFreeDoc(schema->doc);

    if (schema->namespaces != NULL)
        xmlFree((char **) schema->namespaces);

    xmlSchematronFreeRules(schema->rules);
    xmlSchematronFreePatterns(schema->patterns);
    xmlDictFree(schema->dict);
    xmlFree(schema);
}

/**
 * Create an XML Schematrons parse context for that file/resource expected
 * to contain an XML Schematrons file.
 *
 * @param URL  the location of the schema
 * @returns the parser context or NULL in case of error
 */
xmlSchematronParserCtxt *
xmlSchematronNewParserCtxt(const char *URL)
{
    xmlSchematronParserCtxtPtr ret;

    if (URL == NULL)
        return (NULL);

    ret =
        (xmlSchematronParserCtxtPtr)
        xmlMalloc(sizeof(xmlSchematronParserCtxt));
    if (ret == NULL) {
        xmlSchematronPErrMemory(NULL);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronParserCtxt));
    ret->type = XML_STRON_CTXT_PARSER;
    ret->dict = xmlDictCreate();
    ret->URL = xmlDictLookup(ret->dict, (const xmlChar *) URL, -1);
    ret->includes = NULL;
    ret->xctxt = xmlXPathNewContext(NULL);
    if (ret->xctxt == NULL) {
        xmlSchematronPErrMemory(NULL);
        xmlSchematronFreeParserCtxt(ret);
        return (NULL);
    }
    ret->xctxt->flags = XML_XPATH_CHECKNS;
    return (ret);
}

/**
 * Create an XML Schematrons parse context for that memory buffer expected
 * to contain an XML Schematrons file.
 *
 * @param buffer  a pointer to a char array containing the schemas
 * @param size  the size of the array
 * @returns the parser context or NULL in case of error
 */
xmlSchematronParserCtxt *
xmlSchematronNewMemParserCtxt(const char *buffer, int size)
{
    xmlSchematronParserCtxtPtr ret;

    if ((buffer == NULL) || (size <= 0))
        return (NULL);

    ret =
        (xmlSchematronParserCtxtPtr)
        xmlMalloc(sizeof(xmlSchematronParserCtxt));
    if (ret == NULL) {
        xmlSchematronPErrMemory(NULL);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronParserCtxt));
    ret->buffer = buffer;
    ret->size = size;
    ret->dict = xmlDictCreate();
    ret->xctxt = xmlXPathNewContext(NULL);
    if (ret->xctxt == NULL) {
        xmlSchematronPErrMemory(NULL);
        xmlSchematronFreeParserCtxt(ret);
        return (NULL);
    }
    return (ret);
}

/**
 * Create an XML Schematrons parse context for that document.
 * NB. The document may be modified during the parsing process.
 *
 * @param doc  a preparsed document tree
 * @returns the parser context or NULL in case of error
 */
xmlSchematronParserCtxt *
xmlSchematronNewDocParserCtxt(xmlDoc *doc)
{
    xmlSchematronParserCtxtPtr ret;

    if (doc == NULL)
        return (NULL);

    ret =
        (xmlSchematronParserCtxtPtr)
        xmlMalloc(sizeof(xmlSchematronParserCtxt));
    if (ret == NULL) {
        xmlSchematronPErrMemory(NULL);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronParserCtxt));
    ret->doc = doc;
    ret->dict = xmlDictCreate();
    /* The application has responsibility for the document */
    ret->preserve = 1;
    ret->xctxt = xmlXPathNewContext(doc);
    if (ret->xctxt == NULL) {
        xmlSchematronPErrMemory(NULL);
        xmlSchematronFreeParserCtxt(ret);
        return (NULL);
    }

    return (ret);
}

/**
 * Free the resources associated to the schema parser context
 *
 * @param ctxt  the schema parser context
 */
void
xmlSchematronFreeParserCtxt(xmlSchematronParserCtxt *ctxt)
{
    if (ctxt == NULL)
        return;
    if (ctxt->doc != NULL && !ctxt->preserve)
        xmlFreeDoc(ctxt->doc);
    if (ctxt->xctxt != NULL) {
        xmlXPathFreeContext(ctxt->xctxt);
    }
    if (ctxt->namespaces != NULL)
        xmlFree((char **) ctxt->namespaces);
    xmlDictFree(ctxt->dict);
    xmlFree(ctxt);
}

#if 0
/**
 * Add an included document
 *
 * @param ctxt  the schema parser context
 * @param doc  the included document
 * @param cur  the current include node
 */
static void
xmlSchematronPushInclude(xmlSchematronParserCtxtPtr ctxt,
                        xmlDocPtr doc, xmlNodePtr cur)
{
    if (ctxt->includes == NULL) {
        ctxt->maxIncludes = 10;
        ctxt->includes = (xmlNodePtr *)
            xmlMalloc(ctxt->maxIncludes * 2 * sizeof(xmlNodePtr));
        if (ctxt->includes == NULL) {
            xmlSchematronPErrMemory(NULL);
            return;
        }
        ctxt->nbIncludes = 0;
    } else if (ctxt->nbIncludes + 2 >= ctxt->maxIncludes) {
        xmlNodePtr *tmp;

        tmp = (xmlNodePtr *)
            xmlRealloc(ctxt->includes, ctxt->maxIncludes * 4 *
                       sizeof(xmlNodePtr));
        if (tmp == NULL) {
            xmlSchematronPErrMemory(NULL);
            return;
        }
        ctxt->includes = tmp;
        ctxt->maxIncludes *= 2;
    }
    ctxt->includes[2 * ctxt->nbIncludes] = cur;
    ctxt->includes[2 * ctxt->nbIncludes + 1] = (xmlNodePtr) doc;
    ctxt->nbIncludes++;
}

/**
 * Pop an include level. The included document is being freed
 *
 * @param ctxt  the schema parser context
 * @returns the node immediately following the include or NULL if the
 *         include list was empty.
 */
static xmlNodePtr
xmlSchematronPopInclude(xmlSchematronParserCtxtPtr ctxt)
{
    xmlDocPtr doc;
    xmlNodePtr ret;

    if (ctxt->nbIncludes <= 0)
        return(NULL);
    ctxt->nbIncludes--;
    doc = (xmlDocPtr) ctxt->includes[2 * ctxt->nbIncludes + 1];
    ret = ctxt->includes[2 * ctxt->nbIncludes];
    xmlFreeDoc(doc);
    if (ret != NULL)
        ret = ret->next;
    if (ret == NULL)
        return(xmlSchematronPopInclude(ctxt));
    return(ret);
}
#endif

/**
 * Add a namespace definition in the context
 *
 * @param ctxt  the schema parser context
 * @param prefix  the namespace prefix
 * @param ns  the namespace name
 */
static void
xmlSchematronAddNamespace(xmlSchematronParserCtxtPtr ctxt,
                          const xmlChar *prefix, const xmlChar *ns)
{
    if (ctxt->namespaces == NULL) {
        ctxt->maxNamespaces = 10;
        ctxt->namespaces = (const xmlChar **)
            xmlMalloc(ctxt->maxNamespaces * 2 * sizeof(const xmlChar *));
        if (ctxt->namespaces == NULL) {
            xmlSchematronPErrMemory(NULL);
            return;
        }
        ctxt->nbNamespaces = 0;
    } else if (ctxt->nbNamespaces + 2 >= ctxt->maxNamespaces) {
        const xmlChar **tmp;

        tmp = (const xmlChar **)
            xmlRealloc((xmlChar **) ctxt->namespaces, ctxt->maxNamespaces * 4 *
                       sizeof(const xmlChar *));
        if (tmp == NULL) {
            xmlSchematronPErrMemory(NULL);
            return;
        }
        ctxt->namespaces = tmp;
        ctxt->maxNamespaces *= 2;
    }
    ctxt->namespaces[2 * ctxt->nbNamespaces] =
        xmlDictLookup(ctxt->dict, ns, -1);
    ctxt->namespaces[2 * ctxt->nbNamespaces + 1] =
        xmlDictLookup(ctxt->dict, prefix, -1);
    ctxt->nbNamespaces++;
    ctxt->namespaces[2 * ctxt->nbNamespaces] = NULL;
    ctxt->namespaces[2 * ctxt->nbNamespaces + 1] = NULL;

}

/**
 * Format the message content of the assert or report test
 *
 * @param ctxt  the schema parser context
 * @param con  the assert or report node
 */
static void
xmlSchematronParseTestReportMsg(xmlSchematronParserCtxtPtr ctxt, xmlNodePtr con)
{
    xmlNodePtr child;
    xmlXPathCompExprPtr comp;

    child = con->children;
    while (child != NULL) {
        if ((child->type == XML_TEXT_NODE) ||
            (child->type == XML_CDATA_SECTION_NODE))
            /* Do Nothing */
            {}
        else if (IS_SCHEMATRON(child, "name")) {
            /* Do Nothing */
        } else if (IS_SCHEMATRON(child, "value-of")) {
            xmlChar *select;

            select = xmlGetNoNsProp(child, BAD_CAST "select");

            if (select == NULL) {
                xmlSchematronPErr(ctxt, child,
                                  XML_SCHEMAV_ATTRINVALID,
                                  "value-of has no select attribute",
                                  NULL, NULL);
            } else {
                /*
                 * try first to compile the test expression
                 */
                comp = xmlXPathCtxtCompile(ctxt->xctxt, select);
                if (comp == NULL) {
                    xmlSchematronPErr(ctxt, child,
                                      XML_SCHEMAV_ATTRINVALID,
                                      "Failed to compile select expression %s",
                                      select, NULL);
                }
                xmlXPathFreeCompExpr(comp);
            }
            xmlFree(select);
        }
        child = child->next;
    }
}

/**
 * parse a rule element
 *
 * @param ctxt  a schema validation context
 * @param pattern  a pattern
 * @param rule  the rule node
 */
static void
xmlSchematronParseRule(xmlSchematronParserCtxtPtr ctxt,
                       xmlSchematronPatternPtr pattern,
                       xmlNodePtr rule)
{
    xmlNodePtr cur;
    int nbChecks = 0;
    xmlChar *test;
    xmlChar *context;
    xmlChar *report;
    xmlChar *name;
    xmlChar *value;
    xmlSchematronRulePtr ruleptr;
    xmlSchematronTestPtr testptr;

    if ((ctxt == NULL) || (rule == NULL)) return;

    context = xmlGetNoNsProp(rule, BAD_CAST "context");
    if (context == NULL) {
        xmlSchematronPErr(ctxt, rule,
            XML_SCHEMAP_NOROOT,
            "rule has no context attribute",
            NULL, NULL);
        return;
    } else if (context[0] == 0) {
        xmlSchematronPErr(ctxt, rule,
            XML_SCHEMAP_NOROOT,
            "rule has an empty context attribute",
            NULL, NULL);
        xmlFree(context);
        return;
    } else {
        ruleptr = xmlSchematronAddRule(ctxt, ctxt->schema, pattern,
                                       rule, context, NULL);
        if (ruleptr == NULL) {
            xmlFree(context);
            return;
        }
    }

    cur = rule->children;
    NEXT_SCHEMATRON(cur);
    while (cur != NULL) {
        if (IS_SCHEMATRON(cur, "let")) {
            xmlXPathCompExprPtr var_comp;
            xmlSchematronLetPtr let;

            name = xmlGetNoNsProp(cur, BAD_CAST "name");
            if (name == NULL) {
                xmlSchematronPErr(ctxt, cur,
                                  XML_SCHEMAP_NOROOT,
                                  "let has no name attribute",
                                  NULL, NULL);
                return;
            } else if (name[0] == 0) {
                xmlSchematronPErr(ctxt, cur,
                                  XML_SCHEMAP_NOROOT,
                                  "let has an empty name attribute",
                                  NULL, NULL);
                xmlFree(name);
                return;
            }
            value = xmlGetNoNsProp(cur, BAD_CAST "value");
            if (value == NULL) {
                xmlSchematronPErr(ctxt, cur,
                                  XML_SCHEMAP_NOROOT,
                                  "let has no value attribute",
                                  NULL, NULL);
                return;
            } else if (value[0] == 0) {
                xmlSchematronPErr(ctxt, cur,
                                  XML_SCHEMAP_NOROOT,
                                  "let has an empty value attribute",
                                  NULL, NULL);
                xmlFree(value);
                return;
            }

            var_comp = xmlXPathCtxtCompile(ctxt->xctxt, value);
            if (var_comp == NULL) {
                xmlSchematronPErr(ctxt, cur,
                                  XML_SCHEMAP_NOROOT,
                                  "Failed to compile let expression %s",
                                  value, NULL);
                return;
            }

            let = (xmlSchematronLetPtr) xmlMalloc(sizeof(xmlSchematronLet));
            let->name = name;
            let->comp = var_comp;
            let->next = NULL;

            /* add new let variable to the beginning of the list */
            if (ruleptr->lets != NULL) {
                let->next = ruleptr->lets;
            }
            ruleptr->lets = let;

            xmlFree(value);
        } else if (IS_SCHEMATRON(cur, "assert")) {
            nbChecks++;
            test = xmlGetNoNsProp(cur, BAD_CAST "test");
            if (test == NULL) {
                xmlSchematronPErr(ctxt, cur,
                    XML_SCHEMAP_NOROOT,
                    "assert has no test attribute",
                    NULL, NULL);
            } else if (test[0] == 0) {
                xmlSchematronPErr(ctxt, cur,
                    XML_SCHEMAP_NOROOT,
                    "assert has an empty test attribute",
                    NULL, NULL);
                xmlFree(test);
            } else {
                xmlSchematronParseTestReportMsg(ctxt, cur);
                report = xmlNodeGetContent(cur);

                testptr = xmlSchematronAddTest(ctxt, XML_SCHEMATRON_ASSERT,
                                               ruleptr, cur, test, report);
                if (testptr == NULL)
                    xmlFree(test);
            }
        } else if (IS_SCHEMATRON(cur, "report")) {
            nbChecks++;
            test = xmlGetNoNsProp(cur, BAD_CAST "test");
            if (test == NULL) {
                xmlSchematronPErr(ctxt, cur,
                    XML_SCHEMAP_NOROOT,
                    "assert has no test attribute",
                    NULL, NULL);
            } else if (test[0] == 0) {
                xmlSchematronPErr(ctxt, cur,
                    XML_SCHEMAP_NOROOT,
                    "assert has an empty test attribute",
                    NULL, NULL);
                xmlFree(test);
            } else {
                xmlSchematronParseTestReportMsg(ctxt, cur);
                report = xmlNodeGetContent(cur);

                testptr = xmlSchematronAddTest(ctxt, XML_SCHEMATRON_REPORT,
                                               ruleptr, cur, test, report);
                if (testptr == NULL)
                    xmlFree(test);
            }
        } else {
            xmlSchematronPErr(ctxt, cur,
                XML_SCHEMAP_NOROOT,
                "Expecting an assert or a report element instead of %s",
                cur->name, NULL);
        }
        cur = cur->next;
        NEXT_SCHEMATRON(cur);
    }
    if (nbChecks == 0) {
        xmlSchematronPErr(ctxt, rule,
            XML_SCHEMAP_NOROOT,
            "rule has no assert nor report element", NULL, NULL);
    }
}

/**
 * parse a pattern element
 *
 * @param ctxt  a schema validation context
 * @param pat  the pattern node
 */
static void
xmlSchematronParsePattern(xmlSchematronParserCtxtPtr ctxt, xmlNodePtr pat)
{
    xmlNodePtr cur;
    xmlSchematronPatternPtr pattern;
    int nbRules = 0;
    xmlChar *id;

    if ((ctxt == NULL) || (pat == NULL)) return;

    id = xmlGetNoNsProp(pat, BAD_CAST "id");
    if (id == NULL) {
        id = xmlGetNoNsProp(pat, BAD_CAST "name");
    }
    pattern = xmlSchematronAddPattern(ctxt, ctxt->schema, pat, id);
    if (pattern == NULL) {
        if (id != NULL)
            xmlFree(id);
        return;
    }
    cur = pat->children;
    NEXT_SCHEMATRON(cur);
    while (cur != NULL) {
        if (IS_SCHEMATRON(cur, "rule")) {
            xmlSchematronParseRule(ctxt, pattern, cur);
            nbRules++;
        } else {
            xmlSchematronPErr(ctxt, cur,
                XML_SCHEMAP_NOROOT,
                "Expecting a rule element instead of %s", cur->name, NULL);
        }
        cur = cur->next;
        NEXT_SCHEMATRON(cur);
    }
    if (nbRules == 0) {
        xmlSchematronPErr(ctxt, pat,
            XML_SCHEMAP_NOROOT,
            "Pattern has no rule element", NULL, NULL);
    }
}

#if 0
/**
 * Load the include document, Push the current pointer
 *
 * @param ctxt  a schema validation context
 * @param cur  the include element
 * @returns the updated node pointer
 */
static xmlNodePtr
xmlSchematronLoadInclude(xmlSchematronParserCtxtPtr ctxt, xmlNodePtr cur)
{
    xmlNodePtr ret = NULL;
    xmlDocPtr doc = NULL;
    xmlChar *href = NULL;
    xmlChar *base = NULL;
    xmlChar *URI = NULL;

    if ((ctxt == NULL) || (cur == NULL))
        return(NULL);

    href = xmlGetNoNsProp(cur, BAD_CAST "href");
    if (href == NULL) {
        xmlSchematronPErr(ctxt, cur,
            XML_SCHEMAP_NOROOT,
            "Include has no href attribute", NULL, NULL);
        return(cur->next);
    }

    /* do the URI base composition, load and find the root */
    base = xmlNodeGetBase(cur->doc, cur);
    URI = xmlBuildURI(href, base);
    doc = xmlReadFile((const char *) URI, NULL, SCHEMATRON_PARSE_OPTIONS);
    if (doc == NULL) {
        xmlSchematronPErr(ctxt, cur,
                      XML_SCHEMAP_FAILED_LOAD,
                      "could not load include '%s'.\n",
                      URI, NULL);
        goto done;
    }
    ret = xmlDocGetRootElement(doc);
    if (ret == NULL) {
        xmlSchematronPErr(ctxt, cur,
                      XML_SCHEMAP_FAILED_LOAD,
                      "could not find root from include '%s'.\n",
                      URI, NULL);
        goto done;
    }

    /* Success, push the include for rollback on exit */
    xmlSchematronPushInclude(ctxt, doc, cur);

done:
    if (ret == NULL) {
        if (doc != NULL)
            xmlFreeDoc(doc);
    }
    xmlFree(href);
    if (base != NULL)
        xmlFree(base);
    if (URI != NULL)
        xmlFree(URI);
    return(ret);
}
#endif

/**
 * parse a schema definition resource and build an internal
 * XML Schema structure which can be used to validate instances.
 *
 * @param ctxt  a schema validation context
 * @returns the internal XML Schematron structure built from the resource or
 *         NULL in case of error
 */
xmlSchematron *
xmlSchematronParse(xmlSchematronParserCtxt *ctxt)
{
    xmlSchematronPtr ret = NULL;
    xmlDocPtr doc;
    xmlNodePtr root, cur;
    int preserve = 0;

    if (ctxt == NULL)
        return (NULL);

    ctxt->nberrors = 0;

    /*
     * First step is to parse the input document into an DOM/Infoset
     */
    if (ctxt->URL != NULL) {
        doc = xmlReadFile((const char *) ctxt->URL, NULL,
                          SCHEMATRON_PARSE_OPTIONS);
        if (doc == NULL) {
            xmlSchematronPErr(ctxt, NULL,
                          XML_SCHEMAP_FAILED_LOAD,
                          "xmlSchematronParse: could not load '%s'.\n",
                          ctxt->URL, NULL);
            return (NULL);
        }
        ctxt->preserve = 0;
    } else if (ctxt->buffer != NULL) {
        doc = xmlReadMemory(ctxt->buffer, ctxt->size, NULL, NULL,
                            SCHEMATRON_PARSE_OPTIONS);
        if (doc == NULL) {
            xmlSchematronPErr(ctxt, NULL,
                          XML_SCHEMAP_FAILED_PARSE,
                          "xmlSchematronParse: could not parse.\n",
                          NULL, NULL);
            return (NULL);
        }
        doc->URL = xmlStrdup(BAD_CAST "in_memory_buffer");
        ctxt->URL = xmlDictLookup(ctxt->dict, BAD_CAST "in_memory_buffer", -1);
        ctxt->preserve = 0;
    } else if (ctxt->doc != NULL) {
        doc = ctxt->doc;
        preserve = 1;
        ctxt->preserve = 1;
    } else {
        xmlSchematronPErr(ctxt, NULL,
                      XML_SCHEMAP_NOTHING_TO_PARSE,
                      "xmlSchematronParse: could not parse.\n",
                      NULL, NULL);
        return (NULL);
    }

    /*
     * Then extract the root and Schematron parse it
     */
    root = xmlDocGetRootElement(doc);
    if (root == NULL) {
        xmlSchematronPErr(ctxt, (xmlNodePtr) doc,
                      XML_SCHEMAP_NOROOT,
                      "The schema has no document element.\n", NULL, NULL);
        if (!preserve) {
            xmlFreeDoc(doc);
        }
        return (NULL);
    }

    if (!IS_SCHEMATRON(root, "schema")) {
        xmlSchematronPErr(ctxt, root,
            XML_SCHEMAP_NOROOT,
            "The XML document '%s' is not a XML schematron document",
            ctxt->URL, NULL);
        goto exit;
    }
    ret = xmlSchematronNewSchematron(ctxt);
    if (ret == NULL)
        goto exit;
    ctxt->schema = ret;

    /*
     * scan the schema elements
     */
    cur = root->children;
    NEXT_SCHEMATRON(cur);
    if (IS_SCHEMATRON(cur, "title")) {
        xmlChar *title = xmlNodeGetContent(cur);
        if (title != NULL) {
            ret->title = xmlDictLookup(ret->dict, title, -1);
            xmlFree(title);
        }
        cur = cur->next;
        NEXT_SCHEMATRON(cur);
    }
    while (IS_SCHEMATRON(cur, "ns")) {
        xmlChar *prefix = xmlGetNoNsProp(cur, BAD_CAST "prefix");
        xmlChar *uri = xmlGetNoNsProp(cur, BAD_CAST "uri");
        if ((uri == NULL) || (uri[0] == 0)) {
            xmlSchematronPErr(ctxt, cur,
                XML_SCHEMAP_NOROOT,
                "ns element has no uri", NULL, NULL);
        }
        if ((prefix == NULL) || (prefix[0] == 0)) {
            xmlSchematronPErr(ctxt, cur,
                XML_SCHEMAP_NOROOT,
                "ns element has no prefix", NULL, NULL);
        }
        if ((prefix) && (uri)) {
            xmlXPathRegisterNs(ctxt->xctxt, prefix, uri);
            xmlSchematronAddNamespace(ctxt, prefix, uri);
            ret->nbNs++;
        }
        if (uri)
            xmlFree(uri);
        if (prefix)
            xmlFree(prefix);
        cur = cur->next;
        NEXT_SCHEMATRON(cur);
    }
    while (cur != NULL) {
        if (IS_SCHEMATRON(cur, "pattern")) {
            xmlSchematronParsePattern(ctxt, cur);
            ret->nbPattern++;
        } else {
            xmlSchematronPErr(ctxt, cur,
                XML_SCHEMAP_NOROOT,
                "Expecting a pattern element instead of %s", cur->name, NULL);
        }
        cur = cur->next;
        NEXT_SCHEMATRON(cur);
    }
    if (ret->nbPattern == 0) {
        xmlSchematronPErr(ctxt, root,
            XML_SCHEMAP_NOROOT,
            "The schematron document '%s' has no pattern",
            ctxt->URL, NULL);
        goto exit;
    }
    /* the original document must be kept for reporting */
    ret->doc = doc;
    if (preserve) {
            ret->preserve = 1;
    }
    preserve = 1;

exit:
    if (!preserve) {
        xmlFreeDoc(doc);
    }
    if (ret != NULL) {
        if (ctxt->nberrors != 0) {
            xmlSchematronFree(ret);
            ret = NULL;
        } else {
            ret->namespaces = ctxt->namespaces;
            ret->nbNamespaces = ctxt->nbNamespaces;
            ctxt->namespaces = NULL;
        }
    }
    return (ret);
}

/************************************************************************
 *                                                                      *
 *              Schematrontron Reports handler                          *
 *                                                                      *
 ************************************************************************/

static xmlXPathObjectPtr
xmlSchematronGetNode(xmlSchematronValidCtxtPtr ctxt,
                     xmlNodePtr cur, const xmlChar *xpath) {
    if ((ctxt == NULL) || (cur == NULL) || (xpath == NULL))
        return(NULL);

    ctxt->xctxt->doc = cur->doc;
    ctxt->xctxt->node = cur;
    return(xmlXPathEval(xpath, ctxt->xctxt));
}

/**
 * Output part of the report to whatever channel the user selected
 *
 * @param ctxt  the validation context
 * @param cur  the current node tested
 * @param msg  the message output
 */
static void
xmlSchematronReportOutput(xmlSchematronValidCtxtPtr ctxt ATTRIBUTE_UNUSED,
                          xmlNodePtr cur ATTRIBUTE_UNUSED,
                          const char *msg) {
    /* TODO */
    xmlPrintErrorMessage("%s", msg);
}

/**
 * Build the string being reported to the user.
 *
 * @param ctxt  the validation context
 * @param test  the test node
 * @param cur  the current node tested
 * @returns a report string or NULL in case of error. The string needs
 *         to be deallocated by the caller
 */
static xmlChar *
xmlSchematronFormatReport(xmlSchematronValidCtxtPtr ctxt,
                          xmlNodePtr test, xmlNodePtr cur) {
    xmlChar *ret = NULL;
    xmlNodePtr child, node;
    xmlXPathCompExprPtr comp;

    if ((test == NULL) || (cur == NULL))
        return(ret);

    child = test->children;
    while (child != NULL) {
        if ((child->type == XML_TEXT_NODE) ||
            (child->type == XML_CDATA_SECTION_NODE))
            ret = xmlStrcat(ret, child->content);
        else if (IS_SCHEMATRON(child, "name")) {
            xmlXPathObject *obj = NULL;
            xmlChar *path;

            path = xmlGetNoNsProp(child, BAD_CAST "path");

            node = cur;
            if (path != NULL) {
                obj = xmlSchematronGetNode(ctxt, cur, path);
                if ((obj != NULL) &&
                    (obj->type == XPATH_NODESET) &&
                    (obj->nodesetval != NULL) &&
                    (obj->nodesetval->nodeNr > 0))
                    node = obj->nodesetval->nodeTab[0];
                xmlFree(path);
            }

            switch (node->type) {
                case XML_ELEMENT_NODE:
                case XML_ATTRIBUTE_NODE:
                    if ((node->ns == NULL) || (node->ns->prefix == NULL))
                        ret = xmlStrcat(ret, node->name);
                    else {
                        ret = xmlStrcat(ret, node->ns->prefix);
                        ret = xmlStrcat(ret, BAD_CAST ":");
                        ret = xmlStrcat(ret, node->name);
                    }
                    break;

                /* TODO: handle other node types */
                default:
                    break;
            }

            xmlXPathFreeObject(obj);
        } else if (IS_SCHEMATRON(child, "value-of")) {
            xmlChar *select;
            xmlXPathObjectPtr eval;

            select = xmlGetNoNsProp(child, BAD_CAST "select");
            comp = xmlXPathCtxtCompile(ctxt->xctxt, select);
            eval = xmlXPathCompiledEval(comp, ctxt->xctxt);
            if (eval == NULL) {
                xmlXPathFreeCompExpr(comp);
                xmlFree(select);
                return ret;
            }

            switch (eval->type) {
            case XPATH_NODESET: {
                int indx;
                xmlChar *spacer = BAD_CAST " ";

                if (eval->nodesetval) {
                    for (indx = 0; indx < eval->nodesetval->nodeNr; indx++) {
                        if (indx > 0)
                            ret = xmlStrcat(ret, spacer);
                        ret = xmlStrcat(ret, eval->nodesetval->nodeTab[indx]->name);
                    }
                }
                break;
            }
            case XPATH_BOOLEAN: {
                const char *str = eval->boolval ? "True" : "False";
                ret = xmlStrcat(ret, BAD_CAST str);
                break;
            }
            case XPATH_NUMBER: {
                xmlChar *buf;
                int size;

                size = snprintf(NULL, 0, "%0g", eval->floatval);
                buf = (xmlChar *) xmlMalloc(size + 1);
                if (buf != NULL) {
                    snprintf((char *) buf, size + 1, "%0g", eval->floatval);
                    ret = xmlStrcat(ret, buf);
                    xmlFree(buf);
                }
                break;
            }
            case XPATH_STRING:
                ret = xmlStrcat(ret, eval->stringval);
                break;
            default:
                xmlSchematronVErr(ctxt, XML_ERR_INTERNAL_ERROR,
                                  "Unsupported XPATH Type\n", NULL);
            }
            xmlXPathFreeObject(eval);
            xmlXPathFreeCompExpr(comp);
            xmlFree(select);
        } else {
            child = child->next;
            continue;
        }

        /*
         * remove superfluous \n
         */
        if (ret != NULL) {
            int len = xmlStrlen(ret);
            xmlChar c;

            if (len > 0) {
                c = ret[len - 1];
                if ((c == ' ') || (c == '\n') || (c == '\r') || (c == '\t')) {
                    while ((c == ' ') || (c == '\n') ||
                           (c == '\r') || (c == '\t')) {
                        len--;
                        if (len == 0)
                            break;
                        c = ret[len - 1];
                    }
                    ret[len] = ' ';
                    ret[len + 1] = 0;
                }
            }
        }

        child = child->next;
    }
    return(ret);
}

/**
 * called from the validation engine when an assert or report test have
 * been done.
 *
 * @param ctxt  the validation context
 * @param test  the compiled test
 * @param cur  the current node tested
 * @param pattern  a pattern
 * @param success  boolean value for the result
 */
static void
xmlSchematronReportSuccess(xmlSchematronValidCtxtPtr ctxt,
                   xmlSchematronTestPtr test, xmlNodePtr cur, xmlSchematronPatternPtr pattern, int success) {
    if ((ctxt == NULL) || (cur == NULL) || (test == NULL))
        return;
    /* if quiet and not SVRL report only failures */
    if ((ctxt->flags & XML_SCHEMATRON_OUT_QUIET) &&
        ((ctxt->flags & XML_SCHEMATRON_OUT_XML) == 0) &&
        (test->type == XML_SCHEMATRON_REPORT))
        return;
    if (ctxt->flags & XML_SCHEMATRON_OUT_XML) {
        /* TODO */
    } else {
        xmlChar *path;
        char msg[1000];
        long line;
        const xmlChar *report = NULL;

        if (((test->type == XML_SCHEMATRON_REPORT) && (!success)) ||
            ((test->type == XML_SCHEMATRON_ASSERT) && (success)))
            return;
        line = xmlGetLineNo(cur);
        path = xmlGetNodePath(cur);
        if (path == NULL)
            path = (xmlChar *) cur->name;
#if 0
        if ((test->report != NULL) && (test->report[0] != 0))
            report = test->report;
#endif
        if (test->node != NULL)
            report = xmlSchematronFormatReport(ctxt, test->node, cur);
        if (report == NULL) {
            if (test->type == XML_SCHEMATRON_ASSERT) {
            report = xmlStrdup((const xmlChar *) "node failed assert");
            } else {
            report = xmlStrdup((const xmlChar *) "node failed report");
            }
            }
            snprintf(msg, 999, "%s line %ld: %s\n", (const char *) path,
                     line, (const char *) report);

    if (ctxt->flags & XML_SCHEMATRON_OUT_ERROR) {
        xmlStructuredErrorFunc schannel;
        xmlGenericErrorFunc channel;
        void *data;
        int res;

        schannel = ctxt->serror;
        channel = ctxt->error;
        data = ctxt->userData;

        if ((channel == NULL) && (schannel == NULL)) {
            channel = xmlGenericError;
            data = xmlGenericErrorContext;
        }

        res = xmlRaiseError(schannel, channel, data, NULL, cur,
                            XML_FROM_SCHEMATRONV,
                            (test->type == XML_SCHEMATRON_ASSERT) ?
                                XML_SCHEMATRONV_ASSERT :
                                XML_SCHEMATRONV_REPORT,
                            XML_ERR_ERROR, NULL, line,
                            (pattern == NULL) ?
                                NULL :
                                (const char *) pattern->name,
                            (const char *) path, (const char *) report, 0, 0,
                            "%s", msg);
        if (res < 0)
            xmlSchematronVErrMemory(ctxt);
    } else {
        xmlSchematronReportOutput(ctxt, cur, &msg[0]);
    }

    xmlFree((char *) report);

        if ((path != NULL) && (path != (xmlChar *) cur->name))
            xmlFree(path);
    }
}

/**
 * called from the validation engine when starting to check a pattern
 *
 * @param ctxt  the validation context
 * @param pattern  the current pattern
 */
static void
xmlSchematronReportPattern(xmlSchematronValidCtxtPtr ctxt,
                           xmlSchematronPatternPtr pattern) {
    if ((ctxt == NULL) || (pattern == NULL))
        return;
    if ((ctxt->flags & XML_SCHEMATRON_OUT_QUIET) || (ctxt->flags & XML_SCHEMATRON_OUT_ERROR)) /* Error gives pattern name as part of error */
        return;
    if (ctxt->flags & XML_SCHEMATRON_OUT_XML) {
        /* TODO */
    } else {
        char msg[1000];

        if (pattern->name == NULL)
            return;
        snprintf(msg, 999, "Pattern: %s\n", (const char *) pattern->name);
        xmlSchematronReportOutput(ctxt, NULL, &msg[0]);
    }
}


/************************************************************************
 *                                                                      *
 *              Validation against a Schematrontron                             *
 *                                                                      *
 ************************************************************************/

/**
 * Set the structured error callback
 *
 * @param ctxt  a Schematron validation context
 * @param serror  the structured error function
 * @param ctx  the functions context
 */
void
xmlSchematronSetValidStructuredErrors(xmlSchematronValidCtxt *ctxt,
                                      xmlStructuredErrorFunc serror, void *ctx)
{
    if (ctxt == NULL)
        return;
    ctxt->serror = serror;
    ctxt->error = NULL;
    ctxt->warning = NULL;
    ctxt->userData = ctx;
}

/**
 * Create an XML Schematrons validation context based on the given schema.
 *
 * @param schema  a precompiled XML Schematrons
 * @param options  a set of xmlSchematronValidOptions
 * @returns the validation context or NULL in case of error
 */
xmlSchematronValidCtxt *
xmlSchematronNewValidCtxt(xmlSchematron *schema, int options)
{
    int i;
    xmlSchematronValidCtxtPtr ret;

    if (schema == NULL)
        return(NULL);

    ret = (xmlSchematronValidCtxtPtr) xmlMalloc(sizeof(xmlSchematronValidCtxt));
    if (ret == NULL) {
        xmlSchematronVErrMemory(NULL);
        return (NULL);
    }
    memset(ret, 0, sizeof(xmlSchematronValidCtxt));
    ret->type = XML_STRON_CTXT_VALIDATOR;
    ret->schema = schema;
    ret->xctxt = xmlXPathNewContext(NULL);
    ret->flags = options;
    if (ret->xctxt == NULL) {
        xmlSchematronPErrMemory(NULL);
        xmlSchematronFreeValidCtxt(ret);
        return (NULL);
    }
    for (i = 0;i < schema->nbNamespaces;i++) {
        if ((schema->namespaces[2 * i] == NULL) ||
            (schema->namespaces[2 * i + 1] == NULL))
            break;
        xmlXPathRegisterNs(ret->xctxt, schema->namespaces[2 * i + 1],
                           schema->namespaces[2 * i]);
    }
    return (ret);
}

/**
 * Free the resources associated to the schema validation context
 *
 * @param ctxt  the schema validation context
 */
void
xmlSchematronFreeValidCtxt(xmlSchematronValidCtxt *ctxt)
{
    if (ctxt == NULL)
        return;
    if (ctxt->xctxt != NULL)
        xmlXPathFreeContext(ctxt->xctxt);
    if (ctxt->dict != NULL)
        xmlDictFree(ctxt->dict);
    xmlFree(ctxt);
}

static xmlNodePtr
xmlSchematronNextNode(xmlNodePtr cur) {
    if (cur->children != NULL) {
        /*
         * Do not descend on entities declarations
         */
        if (cur->children->type != XML_ENTITY_DECL) {
            cur = cur->children;
            /*
             * Skip DTDs
             */
            if (cur->type != XML_DTD_NODE)
                return(cur);
        }
    }

    while (cur->next != NULL) {
        cur = cur->next;
        if ((cur->type != XML_ENTITY_DECL) &&
            (cur->type != XML_DTD_NODE))
            return(cur);
    }

    do {
        cur = cur->parent;
        if (cur == NULL) break;
        if (cur->type == XML_DOCUMENT_NODE) return(NULL);
        if (cur->next != NULL) {
            cur = cur->next;
            return(cur);
        }
    } while (cur != NULL);
    return(cur);
}

/**
 * Validate a rule against a tree instance at a given position
 *
 * @param ctxt  the schema validation context
 * @param test  the current test
 * @param instance  the document instance tree
 * @param cur  the current node in the instance
 * @param pattern  a pattern
 * @returns 1 in case of success, 0 if error and -1 in case of internal error
 */
static int
xmlSchematronRunTest(xmlSchematronValidCtxtPtr ctxt,
     xmlSchematronTestPtr test, xmlDocPtr instance, xmlNodePtr cur, xmlSchematronPatternPtr pattern)
{
    xmlXPathObjectPtr ret;
    int failed;

    failed = 0;
    ctxt->xctxt->doc = instance;
    ctxt->xctxt->node = cur;
    ret = xmlXPathCompiledEval(test->comp, ctxt->xctxt);
    if (ret == NULL) {
        failed = 1;
    } else {
        switch (ret->type) {
            case XPATH_XSLT_TREE:
            case XPATH_NODESET:
                if ((ret->nodesetval == NULL) ||
                    (ret->nodesetval->nodeNr == 0))
                    failed = 1;
                break;
            case XPATH_BOOLEAN:
                failed = !ret->boolval;
                break;
            case XPATH_NUMBER:
                if ((xmlXPathIsNaN(ret->floatval)) ||
                    (ret->floatval == 0.0))
                    failed = 1;
                break;
            case XPATH_STRING:
                if ((ret->stringval == NULL) ||
                    (ret->stringval[0] == 0))
                    failed = 1;
                break;
            case XPATH_UNDEFINED:
            case XPATH_USERS:
                failed = 1;
                break;
        }
        xmlXPathFreeObject(ret);
    }
    if ((failed) && (test->type == XML_SCHEMATRON_ASSERT))
        ctxt->nberrors++;
    else if ((!failed) && (test->type == XML_SCHEMATRON_REPORT))
        ctxt->nberrors++;

    xmlSchematronReportSuccess(ctxt, test, cur, pattern, !failed);

    return(!failed);
}

/**
 * Registers a list of let variables to the current context of `cur`
 *
 * @param vctxt  the schema validation context
 * @param ctxt  an XPath context
 * @param let  the list of let variables
 * @param instance  the document instance tree
 * @param cur  the current node
 * @returns -1 in case of errors, otherwise 0
 */
static int
xmlSchematronRegisterVariables(xmlSchematronValidCtxtPtr vctxt,
                               xmlXPathContextPtr ctxt,
                               xmlSchematronLetPtr let,
                               xmlDocPtr instance, xmlNodePtr cur)
{
    xmlXPathObjectPtr let_eval;

    ctxt->doc = instance;
    ctxt->node = cur;
    while (let != NULL) {
        let_eval = xmlXPathCompiledEval(let->comp, ctxt);
        if (let_eval == NULL) {
            xmlSchematronVErr(vctxt, XML_ERR_INTERNAL_ERROR,
                              "Evaluation of compiled expression failed\n",
                              NULL);
            return -1;
        }
        if(xmlXPathRegisterVariableNS(ctxt, let->name, NULL, let_eval)) {
            xmlSchematronVErr(vctxt, XML_ERR_INTERNAL_ERROR,
                              "Registering a let variable failed\n", NULL);
            return -1;
        }
        let = let->next;
    }
    return 0;
}

/**
 * Unregisters a list of let variables from the context
 *
 * @param vctxt  the schema validation context
 * @param ctxt  an XPath context
 * @param let  the list of let variables
 * @returns -1 in case of errors, otherwise 0
 */
static int
xmlSchematronUnregisterVariables(xmlSchematronValidCtxtPtr vctxt,
                                 xmlXPathContextPtr ctxt,
                                 xmlSchematronLetPtr let)
{
    while (let != NULL) {
        if (xmlXPathRegisterVariableNS(ctxt, let->name, NULL, NULL)) {
            xmlSchematronVErr(vctxt, XML_ERR_INTERNAL_ERROR,
                              "Unregistering a let variable failed\n", NULL);
            return -1;
        }
        let = let->next;
    }
    return 0;
}

/**
 * Validate a tree instance against the schematron
 *
 * @param ctxt  the schema validation context
 * @param instance  the document instance tree
 * @returns 0 in case of success, -1 in case of internal error
 *         and an error count otherwise.
 */
int
xmlSchematronValidateDoc(xmlSchematronValidCtxt *ctxt, xmlDoc *instance)
{
    xmlNodePtr cur, root;
    xmlSchematronPatternPtr pattern;
    xmlSchematronRulePtr rule;
    xmlSchematronTestPtr test;

    if ((ctxt == NULL) || (ctxt->schema == NULL) ||
        (ctxt->schema->rules == NULL) || (instance == NULL))
        return(-1);
    ctxt->nberrors = 0;
    root = xmlDocGetRootElement(instance);
    if (root == NULL) {
        /* TODO */
        ctxt->nberrors++;
        return(1);
    }
    if ((ctxt->flags & XML_SCHEMATRON_OUT_QUIET) ||
        (ctxt->flags == 0)) {
        /*
         * we are just trying to assert the validity of the document,
         * speed primes over the output, run in a single pass
         */
        cur = root;
        while (cur != NULL) {
            rule = ctxt->schema->rules;
            while (rule != NULL) {
                if (xmlPatternMatch(rule->pattern, cur) == 1) {
                    test = rule->tests;

                    if (xmlSchematronRegisterVariables(ctxt, ctxt->xctxt,
                                rule->lets, instance, cur))
                        return -1;

                    while (test != NULL) {
                        xmlSchematronRunTest(ctxt, test, instance, cur, (xmlSchematronPatternPtr)rule->pattern);
                        test = test->next;
                    }

                    if (xmlSchematronUnregisterVariables(ctxt, ctxt->xctxt,
                                rule->lets))
                        return -1;

                }
                rule = rule->next;
            }

            cur = xmlSchematronNextNode(cur);
        }
    } else {
        /*
         * Process all contexts one at a time
         */
        pattern = ctxt->schema->patterns;

        while (pattern != NULL) {
            xmlSchematronReportPattern(ctxt, pattern);

            /*
             * TODO convert the pattern rule to a direct XPath and
             * compute directly instead of using the pattern matching
             * over the full document...
             * Check the exact semantic
             */
            cur = root;
            while (cur != NULL) {
                rule = pattern->rules;
                while (rule != NULL) {
                    if (xmlPatternMatch(rule->pattern, cur) == 1) {
                        test = rule->tests;
                        xmlSchematronRegisterVariables(ctxt, ctxt->xctxt,
                                rule->lets, instance, cur);

                        while (test != NULL) {
                            xmlSchematronRunTest(ctxt, test, instance, cur, pattern);
                            test = test->next;
                        }

                        xmlSchematronUnregisterVariables(ctxt, ctxt->xctxt,
                                rule->lets);
                    }
                    rule = rule->patnext;
                }

                cur = xmlSchematronNextNode(cur);
            }
            pattern = pattern->next;
        }
    }
    return(ctxt->nberrors);
}

#endif /* LIBXML_SCHEMATRON_ENABLED */
