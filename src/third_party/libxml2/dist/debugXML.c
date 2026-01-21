/*
 * debugXML.c : This is a set of routines used for debugging the tree
 *              produced by the XML parser.
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"
#ifdef LIBXML_DEBUG_ENABLED

#include <string.h>
#include <stdlib.h>

#include <libxml/debugXML.h>
#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/HTMLtree.h>
#include <libxml/HTMLparser.h>
#include <libxml/xmlerror.h>

#include "private/error.h"

#define DUMP_TEXT_TYPE 1

typedef struct _xmlDebugCtxt xmlDebugCtxt;
typedef xmlDebugCtxt *xmlDebugCtxtPtr;
struct _xmlDebugCtxt {
    FILE *output;               /* the output file */
    char shift[101];            /* used for indenting */
    int depth;                  /* current depth */
    xmlDocPtr doc;              /* current document */
    xmlNodePtr node;		/* current node */
    xmlDictPtr dict;		/* the doc dictionary */
    int check;                  /* do just checkings */
    int errors;                 /* number of errors found */
    int nodict;			/* if the document has no dictionary */
    int options;		/* options */
};

static void xmlCtxtDumpNodeList(xmlDebugCtxtPtr ctxt, xmlNodePtr node);

static void
xmlCtxtDumpInitCtxt(xmlDebugCtxtPtr ctxt)
{
    int i;

    ctxt->depth = 0;
    ctxt->check = 0;
    ctxt->errors = 0;
    ctxt->output = stdout;
    ctxt->doc = NULL;
    ctxt->node = NULL;
    ctxt->dict = NULL;
    ctxt->nodict = 0;
    ctxt->options = 0;
    for (i = 0; i < 100; i++)
        ctxt->shift[i] = ' ';
    ctxt->shift[100] = 0;
}

static void
xmlCtxtDumpCleanCtxt(xmlDebugCtxtPtr ctxt ATTRIBUTE_UNUSED)
{
 /* remove the ATTRIBUTE_UNUSED when this is added */
}

/**
 * Check that a given namespace is in scope on a node.
 *
 * @param node  the node
 * @param ns  the namespace node
 * @returns 1 if in scope, -1 in case of argument error,
 *         -2 if the namespace is not in scope, and -3 if not on
 *         an ancestor node.
 */
static int
xmlNsCheckScope(xmlNodePtr node, xmlNsPtr ns)
{
    xmlNsPtr cur;

    if ((node == NULL) || (ns == NULL))
        return(-1);

    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE) &&
	(node->type != XML_DOCUMENT_NODE) &&
	(node->type != XML_TEXT_NODE) &&
	(node->type != XML_HTML_DOCUMENT_NODE) &&
	(node->type != XML_XINCLUDE_START))
	return(-2);

    while ((node != NULL) &&
           ((node->type == XML_ELEMENT_NODE) ||
            (node->type == XML_ATTRIBUTE_NODE) ||
            (node->type == XML_TEXT_NODE) ||
	    (node->type == XML_XINCLUDE_START))) {
	if ((node->type == XML_ELEMENT_NODE) ||
	    (node->type == XML_XINCLUDE_START)) {
	    cur = node->nsDef;
	    while (cur != NULL) {
	        if (cur == ns)
		    return(1);
		if (xmlStrEqual(cur->prefix, ns->prefix))
		    return(-2);
		cur = cur->next;
	    }
	}
	node = node->parent;
    }
    /* the xml namespace may be declared on the document node */
    if ((node != NULL) &&
        ((node->type == XML_DOCUMENT_NODE) ||
	 (node->type == XML_HTML_DOCUMENT_NODE))) {
	 xmlNsPtr oldNs = ((xmlDocPtr) node)->oldNs;
	 if (oldNs == ns)
	     return(1);
    }
    return(-3);
}

static void
xmlCtxtDumpSpaces(xmlDebugCtxtPtr ctxt)
{
    if (ctxt->check)
        return;
    if ((ctxt->output != NULL) && (ctxt->depth > 0)) {
        if (ctxt->depth < 50)
            fprintf(ctxt->output, "%s", &ctxt->shift[100 - 2 * ctxt->depth]);
        else
            fprintf(ctxt->output, "%s", ctxt->shift);
    }
}

/**
 * Handle a debug error.
 *
 * @param ctxt  a debug context
 * @param error  the error code
 * @param msg  the error message
 */
static void
xmlDebugErr(xmlDebugCtxtPtr ctxt, int error, const char *msg)
{
    ctxt->errors++;
    fprintf(ctxt->output, "ERROR %d: %s", error, msg);
}
static void LIBXML_ATTR_FORMAT(3,0)
xmlDebugErr2(xmlDebugCtxtPtr ctxt, int error, const char *msg, int extra)
{
    ctxt->errors++;
    fprintf(ctxt->output, "ERROR %d: ", error);
    fprintf(ctxt->output, msg, extra);
}
static void LIBXML_ATTR_FORMAT(3,0)
xmlDebugErr3(xmlDebugCtxtPtr ctxt, int error, const char *msg, const char *extra)
{
    ctxt->errors++;
    fprintf(ctxt->output, "ERROR %d: ", error);
    fprintf(ctxt->output, msg, extra);
}

/**
 * Report if a given namespace is is not in scope.
 *
 * @param ctxt  the debugging context
 * @param node  the node
 * @param ns  the namespace node
 */
static void
xmlCtxtNsCheckScope(xmlDebugCtxtPtr ctxt, xmlNodePtr node, xmlNsPtr ns)
{
    int ret;

    ret = xmlNsCheckScope(node, ns);
    if (ret == -2) {
        if (ns->prefix == NULL)
	    xmlDebugErr(ctxt, XML_CHECK_NS_SCOPE,
			"Reference to default namespace not in scope\n");
	else
	    xmlDebugErr3(ctxt, XML_CHECK_NS_SCOPE,
			 "Reference to namespace '%s' not in scope\n",
			 (char *) ns->prefix);
    }
    if (ret == -3) {
        if (ns->prefix == NULL)
	    xmlDebugErr(ctxt, XML_CHECK_NS_ANCESTOR,
			"Reference to default namespace not on ancestor\n");
	else
	    xmlDebugErr3(ctxt, XML_CHECK_NS_ANCESTOR,
			 "Reference to namespace '%s' not on ancestor\n",
			 (char *) ns->prefix);
    }
}

/**
 * Do debugging on the string, currently it just checks the UTF-8 content
 *
 * @param ctxt  the debug context
 * @param str  the string
 */
static void
xmlCtxtCheckString(xmlDebugCtxtPtr ctxt, const xmlChar * str)
{
    if (str == NULL) return;
    if (ctxt->check) {
        if (!xmlCheckUTF8(str)) {
	    xmlDebugErr3(ctxt, XML_CHECK_NOT_UTF8,
			 "String is not UTF-8 %s", (const char *) str);
	}
    }
}

/**
 * Do debugging on the name, for example the dictionary status and
 * conformance to the Name production.
 *
 * @param ctxt  the debug context
 * @param name  the name
 */
static void
xmlCtxtCheckName(xmlDebugCtxtPtr ctxt, const xmlChar * name)
{
    if (ctxt->check) {
	if (name == NULL) {
	    xmlDebugErr(ctxt, XML_CHECK_NO_NAME, "Name is NULL");
	    return;
	}
        if (xmlValidateName(name, 0)) {
	    xmlDebugErr3(ctxt, XML_CHECK_NOT_NCNAME,
			 "Name is not an NCName '%s'", (const char *) name);
	}
	if ((ctxt->dict != NULL) &&
	    (!xmlDictOwns(ctxt->dict, name)) &&
            ((ctxt->doc == NULL) ||
             ((ctxt->doc->parseFlags & (XML_PARSE_SAX1 | XML_PARSE_NODICT)) == 0))) {
	    xmlDebugErr3(ctxt, XML_CHECK_OUTSIDE_DICT,
			 "Name is not from the document dictionary '%s'",
			 (const char *) name);
	}
    }
}

static void
xmlCtxtGenericNodeCheck(xmlDebugCtxtPtr ctxt, xmlNodePtr node) {
    xmlDocPtr doc;
    xmlDictPtr dict;

    doc = node->doc;

    if (node->parent == NULL)
        xmlDebugErr(ctxt, XML_CHECK_NO_PARENT,
	            "Node has no parent\n");
    if (node->doc == NULL) {
        xmlDebugErr(ctxt, XML_CHECK_NO_DOC,
	            "Node has no doc\n");
        dict = NULL;
    } else {
	dict = doc->dict;
	if ((dict == NULL) && (ctxt->nodict == 0)) {
	    ctxt->nodict = 1;
	}
	if (ctxt->doc == NULL)
	    ctxt->doc = doc;

	if (ctxt->dict == NULL) {
	    ctxt->dict = dict;
	}
    }
    if ((node->parent != NULL) && (node->doc != node->parent->doc) &&
        (!xmlStrEqual(node->name, BAD_CAST "pseudoroot")))
        xmlDebugErr(ctxt, XML_CHECK_WRONG_DOC,
	            "Node doc differs from parent's one\n");
    if (node->prev == NULL) {
        if (node->type == XML_ATTRIBUTE_NODE) {
	    if ((node->parent != NULL) &&
	        (node != (xmlNodePtr) node->parent->properties))
		xmlDebugErr(ctxt, XML_CHECK_NO_PREV,
                    "Attr has no prev and not first of attr list\n");

        } else if ((node->parent != NULL) && (node->parent->children != node))
	    xmlDebugErr(ctxt, XML_CHECK_NO_PREV,
                    "Node has no prev and not first of parent list\n");
    } else {
        if (node->prev->next != node)
	    xmlDebugErr(ctxt, XML_CHECK_WRONG_PREV,
                        "Node prev->next : back link wrong\n");
    }
    if (node->next == NULL) {
	if ((node->parent != NULL) && (node->type != XML_ATTRIBUTE_NODE) &&
	    (node->parent->last != node) &&
	    (node->parent->type == XML_ELEMENT_NODE))
	    xmlDebugErr(ctxt, XML_CHECK_NO_NEXT,
                    "Node has no next and not last of parent list\n");
    } else {
        if (node->next->prev != node)
	    xmlDebugErr(ctxt, XML_CHECK_WRONG_NEXT,
                    "Node next->prev : forward link wrong\n");
        if (node->next->parent != node->parent)
	    xmlDebugErr(ctxt, XML_CHECK_WRONG_PARENT,
                    "Node next->prev : forward link wrong\n");
    }
    if (node->type == XML_ELEMENT_NODE) {
        xmlNsPtr ns;

	ns = node->nsDef;
	while (ns != NULL) {
	    xmlCtxtNsCheckScope(ctxt, node, ns);
	    ns = ns->next;
	}
	if (node->ns != NULL)
	    xmlCtxtNsCheckScope(ctxt, node, node->ns);
    } else if (node->type == XML_ATTRIBUTE_NODE) {
	if (node->ns != NULL)
	    xmlCtxtNsCheckScope(ctxt, node, node->ns);
    }

    if ((node->type != XML_ELEMENT_NODE) &&
	(node->type != XML_ATTRIBUTE_NODE) &&
	(node->type != XML_ELEMENT_DECL) &&
	(node->type != XML_ATTRIBUTE_DECL) &&
	(node->type != XML_DTD_NODE) &&
	(node->type != XML_HTML_DOCUMENT_NODE) &&
	(node->type != XML_DOCUMENT_NODE)) {
	if (node->content != NULL)
	    xmlCtxtCheckString(ctxt, (const xmlChar *) node->content);
    }
    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
	    xmlCtxtCheckName(ctxt, node->name);
	    break;
        case XML_TEXT_NODE:
	    if ((node->name == xmlStringText) ||
	        (node->name == xmlStringTextNoenc))
		break;
	    xmlDebugErr3(ctxt, XML_CHECK_WRONG_NAME,
			 "Text node has wrong name '%s'",
			 (const char *) node->name);
	    break;
        case XML_COMMENT_NODE:
	    if (node->name == xmlStringComment)
		break;
	    xmlDebugErr3(ctxt, XML_CHECK_WRONG_NAME,
			 "Comment node has wrong name '%s'",
			 (const char *) node->name);
	    break;
        case XML_PI_NODE:
	    xmlCtxtCheckName(ctxt, node->name);
	    break;
        case XML_CDATA_SECTION_NODE:
	    if (node->name == NULL)
		break;
	    xmlDebugErr3(ctxt, XML_CHECK_NAME_NOT_NULL,
			 "CData section has non NULL name '%s'",
			 (const char *) node->name);
	    break;
        case XML_ENTITY_REF_NODE:
        case XML_ENTITY_NODE:
        case XML_DOCUMENT_TYPE_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_NOTATION_NODE:
        case XML_DTD_NODE:
        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
        case XML_NAMESPACE_DECL:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
	    break;
    }
}

static void
xmlCtxtDumpString(xmlDebugCtxtPtr ctxt, const xmlChar * str)
{
    int i;

    if (ctxt->check) {
        return;
    }
    /* TODO: check UTF8 content of the string */
    if (str == NULL) {
        fprintf(ctxt->output, "(NULL)");
        return;
    }
    for (i = 0; i < 40; i++)
        if (str[i] == 0)
            return;
        else if (IS_BLANK_CH(str[i]))
            fputc(' ', ctxt->output);
        else if (str[i] >= 0x80)
            fprintf(ctxt->output, "#%X", str[i]);
        else
            fputc(str[i], ctxt->output);
    fprintf(ctxt->output, "...");
}

static void
xmlCtxtDumpDtdNode(xmlDebugCtxtPtr ctxt, xmlDtdPtr dtd)
{
    xmlCtxtDumpSpaces(ctxt);

    if (dtd == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "DTD node is NULL\n");
        return;
    }

    if (dtd->type != XML_DTD_NODE) {
	xmlDebugErr(ctxt, XML_CHECK_NOT_DTD,
	            "Node is not a DTD");
        return;
    }
    if (!ctxt->check) {
        if (dtd->name != NULL)
            fprintf(ctxt->output, "DTD(%s)", (char *) dtd->name);
        else
            fprintf(ctxt->output, "DTD");
        if (dtd->ExternalID != NULL)
            fprintf(ctxt->output, ", PUBLIC %s", (char *) dtd->ExternalID);
        if (dtd->SystemID != NULL)
            fprintf(ctxt->output, ", SYSTEM %s", (char *) dtd->SystemID);
        fprintf(ctxt->output, "\n");
    }
    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, (xmlNodePtr) dtd);
}

static void
xmlCtxtDumpAttrDecl(xmlDebugCtxtPtr ctxt, xmlAttributePtr attr)
{
    xmlCtxtDumpSpaces(ctxt);

    if (attr == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Attribute declaration is NULL\n");
        return;
    }
    if (attr->type != XML_ATTRIBUTE_DECL) {
	xmlDebugErr(ctxt, XML_CHECK_NOT_ATTR_DECL,
	            "Node is not an attribute declaration");
        return;
    }
    if (attr->name != NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "ATTRDECL(%s)", (char *) attr->name);
    } else
	xmlDebugErr(ctxt, XML_CHECK_NO_NAME,
	            "Node attribute declaration has no name");
    if (attr->elem != NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, " for %s", (char *) attr->elem);
    } else
	xmlDebugErr(ctxt, XML_CHECK_NO_ELEM,
	            "Node attribute declaration has no element name");
    if (!ctxt->check) {
        switch (attr->atype) {
            case XML_ATTRIBUTE_CDATA:
                fprintf(ctxt->output, " CDATA");
                break;
            case XML_ATTRIBUTE_ID:
                fprintf(ctxt->output, " ID");
                break;
            case XML_ATTRIBUTE_IDREF:
                fprintf(ctxt->output, " IDREF");
                break;
            case XML_ATTRIBUTE_IDREFS:
                fprintf(ctxt->output, " IDREFS");
                break;
            case XML_ATTRIBUTE_ENTITY:
                fprintf(ctxt->output, " ENTITY");
                break;
            case XML_ATTRIBUTE_ENTITIES:
                fprintf(ctxt->output, " ENTITIES");
                break;
            case XML_ATTRIBUTE_NMTOKEN:
                fprintf(ctxt->output, " NMTOKEN");
                break;
            case XML_ATTRIBUTE_NMTOKENS:
                fprintf(ctxt->output, " NMTOKENS");
                break;
            case XML_ATTRIBUTE_ENUMERATION:
                fprintf(ctxt->output, " ENUMERATION");
                break;
            case XML_ATTRIBUTE_NOTATION:
                fprintf(ctxt->output, " NOTATION ");
                break;
        }
        if (attr->tree != NULL) {
            int indx;
            xmlEnumerationPtr cur = attr->tree;

            for (indx = 0; indx < 5; indx++) {
                if (indx != 0)
                    fprintf(ctxt->output, "|%s", (char *) cur->name);
                else
                    fprintf(ctxt->output, " (%s", (char *) cur->name);
                cur = cur->next;
                if (cur == NULL)
                    break;
            }
            if (cur == NULL)
                fprintf(ctxt->output, ")");
            else
                fprintf(ctxt->output, "...)");
        }
        switch (attr->def) {
            case XML_ATTRIBUTE_NONE:
                break;
            case XML_ATTRIBUTE_REQUIRED:
                fprintf(ctxt->output, " REQUIRED");
                break;
            case XML_ATTRIBUTE_IMPLIED:
                fprintf(ctxt->output, " IMPLIED");
                break;
            case XML_ATTRIBUTE_FIXED:
                fprintf(ctxt->output, " FIXED");
                break;
        }
        if (attr->defaultValue != NULL) {
            fprintf(ctxt->output, "\"");
            xmlCtxtDumpString(ctxt, attr->defaultValue);
            fprintf(ctxt->output, "\"");
        }
        fprintf(ctxt->output, "\n");
    }

    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, (xmlNodePtr) attr);
}

static void
xmlCtxtDumpElemDecl(xmlDebugCtxtPtr ctxt, xmlElementPtr elem)
{
    xmlCtxtDumpSpaces(ctxt);

    if (elem == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Element declaration is NULL\n");
        return;
    }
    if (elem->type != XML_ELEMENT_DECL) {
	xmlDebugErr(ctxt, XML_CHECK_NOT_ELEM_DECL,
	            "Node is not an element declaration");
        return;
    }
    if (elem->name != NULL) {
        if (!ctxt->check) {
            fprintf(ctxt->output, "ELEMDECL(");
            xmlCtxtDumpString(ctxt, elem->name);
            fprintf(ctxt->output, ")");
        }
    } else
	xmlDebugErr(ctxt, XML_CHECK_NO_NAME,
	            "Element declaration has no name");
    if (!ctxt->check) {
        switch (elem->etype) {
            case XML_ELEMENT_TYPE_UNDEFINED:
                fprintf(ctxt->output, ", UNDEFINED");
                break;
            case XML_ELEMENT_TYPE_EMPTY:
                fprintf(ctxt->output, ", EMPTY");
                break;
            case XML_ELEMENT_TYPE_ANY:
                fprintf(ctxt->output, ", ANY");
                break;
            case XML_ELEMENT_TYPE_MIXED:
                fprintf(ctxt->output, ", MIXED ");
                break;
            case XML_ELEMENT_TYPE_ELEMENT:
                fprintf(ctxt->output, ", MIXED ");
                break;
        }
        if ((elem->type != XML_ELEMENT_NODE) && (elem->content != NULL)) {
            char buf[5001];

            buf[0] = 0;
            xmlSnprintfElementContent(buf, 5000, elem->content, 1);
            buf[5000] = 0;
            fprintf(ctxt->output, "%s", buf);
        }
        fprintf(ctxt->output, "\n");
    }

    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, (xmlNodePtr) elem);
}

static void
xmlCtxtDumpEntityDecl(xmlDebugCtxtPtr ctxt, xmlEntityPtr ent)
{
    xmlCtxtDumpSpaces(ctxt);

    if (ent == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Entity declaration is NULL\n");
        return;
    }
    if (ent->type != XML_ENTITY_DECL) {
	xmlDebugErr(ctxt, XML_CHECK_NOT_ENTITY_DECL,
	            "Node is not an entity declaration");
        return;
    }
    if (ent->name != NULL) {
        if (!ctxt->check) {
            fprintf(ctxt->output, "ENTITYDECL(");
            xmlCtxtDumpString(ctxt, ent->name);
            fprintf(ctxt->output, ")");
        }
    } else
	xmlDebugErr(ctxt, XML_CHECK_NO_NAME,
	            "Entity declaration has no name");
    if (!ctxt->check) {
        switch (ent->etype) {
            case XML_INTERNAL_GENERAL_ENTITY:
                fprintf(ctxt->output, ", internal\n");
                break;
            case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
                fprintf(ctxt->output, ", external parsed\n");
                break;
            case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
                fprintf(ctxt->output, ", unparsed\n");
                break;
            case XML_INTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, ", parameter\n");
                break;
            case XML_EXTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, ", external parameter\n");
                break;
            case XML_INTERNAL_PREDEFINED_ENTITY:
                fprintf(ctxt->output, ", predefined\n");
                break;
        }
        if (ent->ExternalID) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, " ExternalID=%s\n",
                    (char *) ent->ExternalID);
        }
        if (ent->SystemID) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, " SystemID=%s\n",
                    (char *) ent->SystemID);
        }
        if (ent->URI != NULL) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, " URI=%s\n", (char *) ent->URI);
        }
        if (ent->content) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, " content=");
            xmlCtxtDumpString(ctxt, ent->content);
            fprintf(ctxt->output, "\n");
        }
    }

    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, (xmlNodePtr) ent);
}

static void
xmlCtxtDumpNamespace(xmlDebugCtxtPtr ctxt, xmlNsPtr ns)
{
    xmlCtxtDumpSpaces(ctxt);

    if (ns == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "namespace node is NULL\n");
        return;
    }
    if (ns->type != XML_NAMESPACE_DECL) {
	xmlDebugErr(ctxt, XML_CHECK_NOT_NS_DECL,
	            "Node is not a namespace declaration");
        return;
    }
    if (ns->href == NULL) {
        if (ns->prefix != NULL)
	    xmlDebugErr3(ctxt, XML_CHECK_NO_HREF,
                    "Incomplete namespace %s href=NULL\n",
                    (char *) ns->prefix);
        else
	    xmlDebugErr(ctxt, XML_CHECK_NO_HREF,
                    "Incomplete default namespace href=NULL\n");
    } else {
        if (!ctxt->check) {
            if (ns->prefix != NULL)
                fprintf(ctxt->output, "namespace %s href=",
                        (char *) ns->prefix);
            else
                fprintf(ctxt->output, "default namespace href=");

            xmlCtxtDumpString(ctxt, ns->href);
            fprintf(ctxt->output, "\n");
        }
    }
}

static void
xmlCtxtDumpNamespaceList(xmlDebugCtxtPtr ctxt, xmlNsPtr ns)
{
    while (ns != NULL) {
        xmlCtxtDumpNamespace(ctxt, ns);
        ns = ns->next;
    }
}

static void
xmlCtxtDumpEntity(xmlDebugCtxtPtr ctxt, xmlEntityPtr ent)
{
    xmlCtxtDumpSpaces(ctxt);

    if (ent == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Entity is NULL\n");
        return;
    }
    if (!ctxt->check) {
        switch (ent->etype) {
            case XML_INTERNAL_GENERAL_ENTITY:
                fprintf(ctxt->output, "INTERNAL_GENERAL_ENTITY ");
                break;
            case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
                fprintf(ctxt->output, "EXTERNAL_GENERAL_PARSED_ENTITY ");
                break;
            case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
                fprintf(ctxt->output, "EXTERNAL_GENERAL_UNPARSED_ENTITY ");
                break;
            case XML_INTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, "INTERNAL_PARAMETER_ENTITY ");
                break;
            case XML_EXTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, "EXTERNAL_PARAMETER_ENTITY ");
                break;
            default:
                fprintf(ctxt->output, "ENTITY_%d ! ", (int) ent->etype);
        }
        fprintf(ctxt->output, "%s\n", ent->name);
        if (ent->ExternalID) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "ExternalID=%s\n",
                    (char *) ent->ExternalID);
        }
        if (ent->SystemID) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "SystemID=%s\n", (char *) ent->SystemID);
        }
        if (ent->URI) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "URI=%s\n", (char *) ent->URI);
        }
        if (ent->content) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "content=");
            xmlCtxtDumpString(ctxt, ent->content);
            fprintf(ctxt->output, "\n");
        }
    }
}

/**
 * Dumps debug information for the attribute
 *
 * @param ctxt  the debug context
 * @param attr  the attribute
 */
static void
xmlCtxtDumpAttr(xmlDebugCtxtPtr ctxt, xmlAttrPtr attr)
{
    xmlCtxtDumpSpaces(ctxt);

    if (attr == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Attr is NULL");
        return;
    }
    if (!ctxt->check) {
        fprintf(ctxt->output, "ATTRIBUTE ");
	xmlCtxtDumpString(ctxt, attr->name);
        fprintf(ctxt->output, "\n");
        if (attr->children != NULL) {
            ctxt->depth++;
            xmlCtxtDumpNodeList(ctxt, attr->children);
            ctxt->depth--;
        }
    }
    if (attr->name == NULL)
	xmlDebugErr(ctxt, XML_CHECK_NO_NAME,
	            "Attribute has no name");

    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, (xmlNodePtr) attr);
}

/**
 * Dumps debug information for the attribute list
 *
 * @param ctxt  the debug context
 * @param attr  the attribute list
 */
static void
xmlCtxtDumpAttrList(xmlDebugCtxtPtr ctxt, xmlAttrPtr attr)
{
    while (attr != NULL) {
        xmlCtxtDumpAttr(ctxt, attr);
        attr = attr->next;
    }
}

/**
 * Dumps debug information for the element node, it is not recursive/
 *
 * @param ctxt  the debug context
 * @param node  the node
 */
static void
xmlCtxtDumpOneNode(xmlDebugCtxtPtr ctxt, xmlNodePtr node)
{
    if (node == NULL) {
        if (!ctxt->check) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "node is NULL\n");
        }
        return;
    }
    ctxt->node = node;

    switch (node->type) {
        case XML_ELEMENT_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "ELEMENT ");
                if ((node->ns != NULL) && (node->ns->prefix != NULL)) {
                    xmlCtxtDumpString(ctxt, node->ns->prefix);
                    fprintf(ctxt->output, ":");
                }
                xmlCtxtDumpString(ctxt, node->name);
                fprintf(ctxt->output, "\n");
            }
            break;
        case XML_ATTRIBUTE_NODE:
            if (!ctxt->check)
                xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "Error, ATTRIBUTE found here\n");
            xmlCtxtGenericNodeCheck(ctxt, node);
            return;
        case XML_TEXT_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                if (node->name == (const xmlChar *) xmlStringTextNoenc)
                    fprintf(ctxt->output, "TEXT no enc");
                else
                    fprintf(ctxt->output, "TEXT");
		if (ctxt->options & DUMP_TEXT_TYPE) {
		    if (node->content == (xmlChar *) &(node->properties))
			fprintf(ctxt->output, " compact\n");
		    else if (xmlDictOwns(ctxt->dict, node->content) == 1)
			fprintf(ctxt->output, " interned\n");
		    else
			fprintf(ctxt->output, "\n");
		} else
		    fprintf(ctxt->output, "\n");
            }
            break;
        case XML_CDATA_SECTION_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "CDATA_SECTION\n");
            }
            break;
        case XML_ENTITY_REF_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "ENTITY_REF(%s)\n",
                        (char *) node->name);
            }
            break;
        case XML_ENTITY_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "ENTITY\n");
            }
            break;
        case XML_PI_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "PI %s\n", (char *) node->name);
            }
            break;
        case XML_COMMENT_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "COMMENT\n");
            }
            break;
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
            }
            fprintf(ctxt->output, "Error, DOCUMENT found here\n");
            xmlCtxtGenericNodeCheck(ctxt, node);
            return;
        case XML_DOCUMENT_TYPE_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "DOCUMENT_TYPE\n");
            }
            break;
        case XML_DOCUMENT_FRAG_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "DOCUMENT_FRAG\n");
            }
            break;
        case XML_NOTATION_NODE:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "NOTATION\n");
            }
            break;
        case XML_DTD_NODE:
            xmlCtxtDumpDtdNode(ctxt, (xmlDtdPtr) node);
            return;
        case XML_ELEMENT_DECL:
            xmlCtxtDumpElemDecl(ctxt, (xmlElementPtr) node);
            return;
        case XML_ATTRIBUTE_DECL:
            xmlCtxtDumpAttrDecl(ctxt, (xmlAttributePtr) node);
            return;
        case XML_ENTITY_DECL:
            xmlCtxtDumpEntityDecl(ctxt, (xmlEntityPtr) node);
            return;
        case XML_NAMESPACE_DECL:
            xmlCtxtDumpNamespace(ctxt, (xmlNsPtr) node);
            return;
        case XML_XINCLUDE_START:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "INCLUDE START\n");
            }
            return;
        case XML_XINCLUDE_END:
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "INCLUDE END\n");
            }
            return;
        default:
            if (!ctxt->check)
                xmlCtxtDumpSpaces(ctxt);
	    xmlDebugErr2(ctxt, XML_CHECK_UNKNOWN_NODE,
	                "Unknown node type %d\n", node->type);
            return;
    }
    if (node->doc == NULL) {
        if (!ctxt->check) {
            xmlCtxtDumpSpaces(ctxt);
        }
        fprintf(ctxt->output, "PBM: doc == NULL !!!\n");
    }
    ctxt->depth++;
    if ((node->type == XML_ELEMENT_NODE) && (node->nsDef != NULL))
        xmlCtxtDumpNamespaceList(ctxt, node->nsDef);
    if ((node->type == XML_ELEMENT_NODE) && (node->properties != NULL))
        xmlCtxtDumpAttrList(ctxt, node->properties);
    if (node->type != XML_ENTITY_REF_NODE) {
        if ((node->type != XML_ELEMENT_NODE) && (node->content != NULL)) {
            if (!ctxt->check) {
                xmlCtxtDumpSpaces(ctxt);
                fprintf(ctxt->output, "content=");
                xmlCtxtDumpString(ctxt, node->content);
                fprintf(ctxt->output, "\n");
            }
        }
    } else {
        xmlEntityPtr ent;

        ent = xmlGetDocEntity(node->doc, node->name);
        if (ent != NULL)
            xmlCtxtDumpEntity(ctxt, ent);
    }
    ctxt->depth--;

    /*
     * Do a bit of checking
     */
    xmlCtxtGenericNodeCheck(ctxt, node);
}

/**
 * Dumps debug information for the element node, it is recursive
 *
 * @param ctxt  the debug context
 * @param node  the node
 */
static void
xmlCtxtDumpNode(xmlDebugCtxtPtr ctxt, xmlNodePtr node)
{
    if (node == NULL) {
        if (!ctxt->check) {
            xmlCtxtDumpSpaces(ctxt);
            fprintf(ctxt->output, "node is NULL\n");
        }
        return;
    }
    xmlCtxtDumpOneNode(ctxt, node);
    if ((node->type != XML_NAMESPACE_DECL) &&
        (node->children != NULL) && (node->type != XML_ENTITY_REF_NODE)) {
        ctxt->depth++;
        xmlCtxtDumpNodeList(ctxt, node->children);
        ctxt->depth--;
    }
}

/**
 * Dumps debug information for the list of element node, it is recursive
 *
 * @param ctxt  the debug context
 * @param node  the node list
 */
static void
xmlCtxtDumpNodeList(xmlDebugCtxtPtr ctxt, xmlNodePtr node)
{
    while (node != NULL) {
        xmlCtxtDumpNode(ctxt, node);
        node = node->next;
    }
}

static void
xmlCtxtDumpDocHead(xmlDebugCtxtPtr ctxt, xmlDocPtr doc)
{
    if (doc == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "DOCUMENT == NULL !\n");
        return;
    }
    ctxt->node = (xmlNodePtr) doc;

    switch (doc->type) {
        case XML_ELEMENT_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_ELEMENT,
	                "Misplaced ELEMENT node\n");
            break;
        case XML_ATTRIBUTE_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_ATTRIBUTE,
	                "Misplaced ATTRIBUTE node\n");
            break;
        case XML_TEXT_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_TEXT,
	                "Misplaced TEXT node\n");
            break;
        case XML_CDATA_SECTION_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_CDATA,
	                "Misplaced CDATA node\n");
            break;
        case XML_ENTITY_REF_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_ENTITYREF,
	                "Misplaced ENTITYREF node\n");
            break;
        case XML_ENTITY_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_ENTITY,
	                "Misplaced ENTITY node\n");
            break;
        case XML_PI_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_PI,
	                "Misplaced PI node\n");
            break;
        case XML_COMMENT_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_COMMENT,
	                "Misplaced COMMENT node\n");
            break;
        case XML_DOCUMENT_NODE:
	    if (!ctxt->check)
		fprintf(ctxt->output, "DOCUMENT\n");
            break;
        case XML_HTML_DOCUMENT_NODE:
	    if (!ctxt->check)
		fprintf(ctxt->output, "HTML DOCUMENT\n");
            break;
        case XML_DOCUMENT_TYPE_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_DOCTYPE,
	                "Misplaced DOCTYPE node\n");
            break;
        case XML_DOCUMENT_FRAG_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_FRAGMENT,
	                "Misplaced FRAGMENT node\n");
            break;
        case XML_NOTATION_NODE:
	    xmlDebugErr(ctxt, XML_CHECK_FOUND_NOTATION,
	                "Misplaced NOTATION node\n");
            break;
        default:
	    xmlDebugErr2(ctxt, XML_CHECK_UNKNOWN_NODE,
	                "Unknown node type %d\n", doc->type);
    }
}

/**
 * Dumps debug information concerning the document, not recursive
 *
 * @param ctxt  the debug context
 * @param doc  the document
 */
static void
xmlCtxtDumpDocumentHead(xmlDebugCtxtPtr ctxt, xmlDocPtr doc)
{
    if (doc == NULL) return;
    xmlCtxtDumpDocHead(ctxt, doc);
    if (!ctxt->check) {
        if (doc->name != NULL) {
            fprintf(ctxt->output, "name=");
            xmlCtxtDumpString(ctxt, BAD_CAST doc->name);
            fprintf(ctxt->output, "\n");
        }
        if (doc->version != NULL) {
            fprintf(ctxt->output, "version=");
            xmlCtxtDumpString(ctxt, doc->version);
            fprintf(ctxt->output, "\n");
        }
        if (doc->encoding != NULL) {
            fprintf(ctxt->output, "encoding=");
            xmlCtxtDumpString(ctxt, doc->encoding);
            fprintf(ctxt->output, "\n");
        }
        if (doc->URL != NULL) {
            fprintf(ctxt->output, "URL=");
            xmlCtxtDumpString(ctxt, doc->URL);
            fprintf(ctxt->output, "\n");
        }
        if (doc->standalone)
            fprintf(ctxt->output, "standalone=true\n");
    }
    if (doc->oldNs != NULL)
        xmlCtxtDumpNamespaceList(ctxt, doc->oldNs);
}

/**
 * Dumps debug information for the document, it's recursive
 *
 * @param ctxt  the debug context
 * @param doc  the document
 */
static void
xmlCtxtDumpDocument(xmlDebugCtxtPtr ctxt, xmlDocPtr doc)
{
    if (doc == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "DOCUMENT == NULL !\n");
        return;
    }
    xmlCtxtDumpDocumentHead(ctxt, doc);
    if (((doc->type == XML_DOCUMENT_NODE) ||
         (doc->type == XML_HTML_DOCUMENT_NODE))
        && (doc->children != NULL)) {
        ctxt->depth++;
        xmlCtxtDumpNodeList(ctxt, doc->children);
        ctxt->depth--;
    }
}

static void
xmlCtxtDumpEntityCallback(void *payload, void *data,
                          const xmlChar *name ATTRIBUTE_UNUSED)
{
    xmlEntityPtr cur = (xmlEntityPtr) payload;
    xmlDebugCtxtPtr ctxt = (xmlDebugCtxtPtr) data;
    if (cur == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "Entity is NULL");
        return;
    }
    if (!ctxt->check) {
        fprintf(ctxt->output, "%s : ", (char *) cur->name);
        switch (cur->etype) {
            case XML_INTERNAL_GENERAL_ENTITY:
                fprintf(ctxt->output, "INTERNAL GENERAL, ");
                break;
            case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
                fprintf(ctxt->output, "EXTERNAL PARSED, ");
                break;
            case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
                fprintf(ctxt->output, "EXTERNAL UNPARSED, ");
                break;
            case XML_INTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, "INTERNAL PARAMETER, ");
                break;
            case XML_EXTERNAL_PARAMETER_ENTITY:
                fprintf(ctxt->output, "EXTERNAL PARAMETER, ");
                break;
            default:
		xmlDebugErr2(ctxt, XML_CHECK_ENTITY_TYPE,
			     "Unknown entity type %d\n", cur->etype);
        }
        if (cur->ExternalID != NULL)
            fprintf(ctxt->output, "ID \"%s\"", (char *) cur->ExternalID);
        if (cur->SystemID != NULL)
            fprintf(ctxt->output, "SYSTEM \"%s\"", (char *) cur->SystemID);
        if (cur->orig != NULL)
            fprintf(ctxt->output, "\n orig \"%s\"", (char *) cur->orig);
        if ((cur->type != XML_ELEMENT_NODE) && (cur->content != NULL))
            fprintf(ctxt->output, "\n content \"%s\"",
                    (char *) cur->content);
        fprintf(ctxt->output, "\n");
    }
}

/**
 * Dumps debug information for all the entities in use by the document
 *
 * @param ctxt  the debug context
 * @param doc  the document
 */
static void
xmlCtxtDumpEntities(xmlDebugCtxtPtr ctxt, xmlDocPtr doc)
{
    if (doc == NULL) return;
    xmlCtxtDumpDocHead(ctxt, doc);
    if ((doc->intSubset != NULL) && (doc->intSubset->entities != NULL)) {
        xmlEntitiesTablePtr table = (xmlEntitiesTablePtr)
            doc->intSubset->entities;

        if (!ctxt->check)
            fprintf(ctxt->output, "Entities in internal subset\n");
        xmlHashScan(table, xmlCtxtDumpEntityCallback, ctxt);
    } else
        fprintf(ctxt->output, "No entities in internal subset\n");
    if ((doc->extSubset != NULL) && (doc->extSubset->entities != NULL)) {
        xmlEntitiesTablePtr table = (xmlEntitiesTablePtr)
            doc->extSubset->entities;

        if (!ctxt->check)
            fprintf(ctxt->output, "Entities in external subset\n");
        xmlHashScan(table, xmlCtxtDumpEntityCallback, ctxt);
    } else if (!ctxt->check)
        fprintf(ctxt->output, "No entities in external subset\n");
}

/**
 * Dumps debug information for the DTD
 *
 * @param ctxt  the debug context
 * @param dtd  the DTD
 */
static void
xmlCtxtDumpDTD(xmlDebugCtxtPtr ctxt, xmlDtdPtr dtd)
{
    if (dtd == NULL) {
        if (!ctxt->check)
            fprintf(ctxt->output, "DTD is NULL\n");
        return;
    }
    xmlCtxtDumpDtdNode(ctxt, dtd);
    if (dtd->children == NULL)
        fprintf(ctxt->output, "    DTD is empty\n");
    else {
        ctxt->depth++;
        xmlCtxtDumpNodeList(ctxt, dtd->children);
        ctxt->depth--;
    }
}

/************************************************************************
 *									*
 *			Public entry points for dump			*
 *									*
 ************************************************************************/

/**
 * Dumps information about the string, shorten it if necessary
 *
 * @param output  the FILE * for the output
 * @param str  the string
 */
void
xmlDebugDumpString(FILE * output, const xmlChar * str)
{
    int i;

    if (output == NULL)
	output = stdout;
    if (str == NULL) {
        fprintf(output, "(NULL)");
        return;
    }
    for (i = 0; i < 40; i++)
        if (str[i] == 0)
            return;
        else if (IS_BLANK_CH(str[i]))
            fputc(' ', output);
        else if (str[i] >= 0x80)
            fprintf(output, "#%X", str[i]);
        else
            fputc(str[i], output);
    fprintf(output, "...");
}

/**
 * Dumps debug information for the attribute
 *
 * @param output  the FILE * for the output
 * @param attr  the attribute
 * @param depth  the indentation level.
 */
void
xmlDebugDumpAttr(FILE *output, xmlAttr *attr, int depth) {
    xmlDebugCtxt ctxt;

    if (output == NULL) return;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.depth = depth;
    xmlCtxtDumpAttr(&ctxt, attr);
    xmlCtxtDumpCleanCtxt(&ctxt);
}


/**
 * Dumps debug information for all the entities in use by the document
 *
 * @param output  the FILE * for the output
 * @param doc  the document
 */
void
xmlDebugDumpEntities(FILE * output, xmlDoc *doc)
{
    xmlDebugCtxt ctxt;

    if (output == NULL) return;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    xmlCtxtDumpEntities(&ctxt, doc);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the attribute list
 *
 * @param output  the FILE * for the output
 * @param attr  the attribute list
 * @param depth  the indentation level.
 */
void
xmlDebugDumpAttrList(FILE * output, xmlAttr *attr, int depth)
{
    xmlDebugCtxt ctxt;

    if (output == NULL) return;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.depth = depth;
    xmlCtxtDumpAttrList(&ctxt, attr);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the element node, it is not recursive
 *
 * @param output  the FILE * for the output
 * @param node  the node
 * @param depth  the indentation level.
 */
void
xmlDebugDumpOneNode(FILE * output, xmlNode *node, int depth)
{
    xmlDebugCtxt ctxt;

    if (output == NULL) return;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.depth = depth;
    xmlCtxtDumpOneNode(&ctxt, node);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the element node, it is recursive
 *
 * @param output  the FILE * for the output
 * @param node  the node
 * @param depth  the indentation level.
 */
void
xmlDebugDumpNode(FILE * output, xmlNode *node, int depth)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.depth = depth;
    xmlCtxtDumpNode(&ctxt, node);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the list of element node, it is recursive
 *
 * @param output  the FILE * for the output
 * @param node  the node list
 * @param depth  the indentation level.
 */
void
xmlDebugDumpNodeList(FILE * output, xmlNode *node, int depth)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.depth = depth;
    xmlCtxtDumpNodeList(&ctxt, node);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information concerning the document, not recursive
 *
 * @param output  the FILE * for the output
 * @param doc  the document
 */
void
xmlDebugDumpDocumentHead(FILE * output, xmlDoc *doc)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.options |= DUMP_TEXT_TYPE;
    ctxt.output = output;
    xmlCtxtDumpDocumentHead(&ctxt, doc);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the document, it's recursive
 *
 * @param output  the FILE * for the output
 * @param doc  the document
 */
void
xmlDebugDumpDocument(FILE * output, xmlDoc *doc)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.options |= DUMP_TEXT_TYPE;
    ctxt.output = output;
    xmlCtxtDumpDocument(&ctxt, doc);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/**
 * Dumps debug information for the DTD
 *
 * @param output  the FILE * for the output
 * @param dtd  the DTD
 */
void
xmlDebugDumpDTD(FILE * output, xmlDtd *dtd)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.options |= DUMP_TEXT_TYPE;
    ctxt.output = output;
    xmlCtxtDumpDTD(&ctxt, dtd);
    xmlCtxtDumpCleanCtxt(&ctxt);
}

/************************************************************************
 *									*
 *			Public entry points for checkings		*
 *									*
 ************************************************************************/

/**
 * Check the document for potential content problems, and output
 * the errors to `output`
 *
 * @param output  the FILE * for the output
 * @param doc  the document
 * @returns the number of errors found
 */
int
xmlDebugCheckDocument(FILE * output, xmlDoc *doc)
{
    xmlDebugCtxt ctxt;

    if (output == NULL)
	output = stdout;
    xmlCtxtDumpInitCtxt(&ctxt);
    ctxt.output = output;
    ctxt.check = 1;
    xmlCtxtDumpDocument(&ctxt, doc);
    xmlCtxtDumpCleanCtxt(&ctxt);
    return(ctxt.errors);
}

#endif /* LIBXML_DEBUG_ENABLED */
