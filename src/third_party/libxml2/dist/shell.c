/*
 * shell.c: The xmllint shell
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#include "libxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#ifdef HAVE_LIBHISTORY
#include <readline/history.h>
#endif
#endif

#include <libxml/debugXML.h>
#include <libxml/HTMLtree.h>
#include <libxml/parser.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#ifdef LIBXML_RELAXNG_ENABLED
#include <libxml/relaxng.h>
#endif

#include "private/lint.h"

#ifndef STDIN_FILENO
  #define STDIN_FILENO 0
#endif

/*
 * TODO: Improvement/cleanups for the XML shell
 *     - allow to shell out an editor on a subpart
 *     - cleanup function registrations (with help) and calling
 *     - provide registration routines
 */

typedef struct _xmllintShellCtxt xmllintShellCtxt;
typedef xmllintShellCtxt *xmllintShellCtxtPtr;
struct _xmllintShellCtxt {
    char *filename;
    xmlDocPtr doc;
    xmlNodePtr node;
#ifdef LIBXML_XPATH_ENABLED
    xmlXPathContextPtr pctxt;
#endif
    int loaded;
    FILE *output;
};

/**
 * Count the children of `node`.
 *
 * @param node  the node to count
 * @returns the number of children of `node`.
 */
static int
xmllintLsCountNode(xmlNodePtr node) {
    int ret = 0;
    xmlNodePtr list = NULL;

    if (node == NULL)
	return(0);

    switch (node->type) {
	case XML_ELEMENT_NODE:
	    list = node->children;
	    break;
	case XML_DOCUMENT_NODE:
	case XML_HTML_DOCUMENT_NODE:
	    list = ((xmlDocPtr) node)->children;
	    break;
	case XML_ATTRIBUTE_NODE:
	    list = ((xmlAttrPtr) node)->children;
	    break;
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_PI_NODE:
	case XML_COMMENT_NODE:
	    if (node->content != NULL) {
		ret = xmlStrlen(node->content);
            }
	    break;
	case XML_ENTITY_REF_NODE:
	case XML_DOCUMENT_TYPE_NODE:
	case XML_ENTITY_NODE:
	case XML_DOCUMENT_FRAG_NODE:
	case XML_NOTATION_NODE:
	case XML_DTD_NODE:
        case XML_ELEMENT_DECL:
        case XML_ATTRIBUTE_DECL:
        case XML_ENTITY_DECL:
	case XML_NAMESPACE_DECL:
	case XML_XINCLUDE_START:
	case XML_XINCLUDE_END:
	    ret = 1;
	    break;
    }
    for (;list != NULL;ret++)
        list = list->next;
    return(ret);
}

/**
 * Dump to `output` the type and name of `node`.
 *
 * @param output  the FILE * for the output
 * @param node  the node to dump
 */
static void
xmllintLsOneNode(FILE *output, xmlNodePtr node) {
    if (output == NULL) return;
    if (node == NULL) {
	fprintf(output, "NULL\n");
	return;
    }
    switch (node->type) {
	case XML_ELEMENT_NODE:
	    fprintf(output, "-");
	    break;
	case XML_ATTRIBUTE_NODE:
	    fprintf(output, "a");
	    break;
	case XML_TEXT_NODE:
	    fprintf(output, "t");
	    break;
	case XML_CDATA_SECTION_NODE:
	    fprintf(output, "C");
	    break;
	case XML_ENTITY_REF_NODE:
	    fprintf(output, "e");
	    break;
	case XML_ENTITY_NODE:
	    fprintf(output, "E");
	    break;
	case XML_PI_NODE:
	    fprintf(output, "p");
	    break;
	case XML_COMMENT_NODE:
	    fprintf(output, "c");
	    break;
	case XML_DOCUMENT_NODE:
	    fprintf(output, "d");
	    break;
	case XML_HTML_DOCUMENT_NODE:
	    fprintf(output, "h");
	    break;
	case XML_DOCUMENT_TYPE_NODE:
	    fprintf(output, "T");
	    break;
	case XML_DOCUMENT_FRAG_NODE:
	    fprintf(output, "F");
	    break;
	case XML_NOTATION_NODE:
	    fprintf(output, "N");
	    break;
	case XML_NAMESPACE_DECL:
	    fprintf(output, "n");
	    break;
	default:
	    fprintf(output, "?");
    }
    if (node->type != XML_NAMESPACE_DECL) {
	if (node->properties != NULL)
	    fprintf(output, "a");
	else
	    fprintf(output, "-");
	if (node->nsDef != NULL)
	    fprintf(output, "n");
	else
	    fprintf(output, "-");
    }

    fprintf(output, " %8d ", xmllintLsCountNode(node));

    switch (node->type) {
	case XML_ELEMENT_NODE:
	    if (node->name != NULL) {
                if ((node->ns != NULL) && (node->ns->prefix != NULL))
                    fprintf(output, "%s:", node->ns->prefix);
		fprintf(output, "%s", (const char *) node->name);
            }
	    break;
	case XML_ATTRIBUTE_NODE:
	    if (node->name != NULL)
		fprintf(output, "%s", (const char *) node->name);
	    break;
	case XML_TEXT_NODE:
#ifdef LIBXML_DEBUG_ENABLED
	    if (node->content != NULL) {
		xmlDebugDumpString(output, node->content);
            }
#endif
	    break;
	case XML_CDATA_SECTION_NODE:
	    break;
	case XML_ENTITY_REF_NODE:
	    if (node->name != NULL)
		fprintf(output, "%s", (const char *) node->name);
	    break;
	case XML_ENTITY_NODE:
	    if (node->name != NULL)
		fprintf(output, "%s", (const char *) node->name);
	    break;
	case XML_PI_NODE:
	    if (node->name != NULL)
		fprintf(output, "%s", (const char *) node->name);
	    break;
	case XML_COMMENT_NODE:
	    break;
	case XML_DOCUMENT_NODE:
	    break;
	case XML_HTML_DOCUMENT_NODE:
	    break;
	case XML_DOCUMENT_TYPE_NODE:
	    break;
	case XML_DOCUMENT_FRAG_NODE:
	    break;
	case XML_NOTATION_NODE:
	    break;
	case XML_NAMESPACE_DECL: {
	    xmlNsPtr ns = (xmlNsPtr) node;

	    if (ns->prefix == NULL)
		fprintf(output, "default -> %s", (char *)ns->href);
	    else
		fprintf(output, "%s -> %s", (char *)ns->prefix,
			(char *)ns->href);
	    break;
	}
	default:
	    if (node->name != NULL)
		fprintf(output, "%s", (const char *) node->name);
    }
    fprintf(output, "\n");
}

/**
 * Implements the XML shell function "ls"
 * Does an Unix like listing of the given node (like a directory)
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellList(xmllintShellCtxtPtr ctxt,
             char *arg ATTRIBUTE_UNUSED, xmlNodePtr node,
             xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlNodePtr cur;
    if (!ctxt)
        return (0);
    if (node == NULL) {
	fprintf(ctxt->output, "NULL\n");
	return (0);
    }
    if ((node->type == XML_DOCUMENT_NODE) ||
        (node->type == XML_HTML_DOCUMENT_NODE)) {
        cur = ((xmlDocPtr) node)->children;
    } else if (node->type == XML_NAMESPACE_DECL) {
        xmllintLsOneNode(ctxt->output, node);
        return (0);
    } else if (node->children != NULL) {
        cur = node->children;
    } else {
        xmllintLsOneNode(ctxt->output, node);
        return (0);
    }
    while (cur != NULL) {
        xmllintLsOneNode(ctxt->output, cur);
        cur = cur->next;
    }
    return (0);
}

/**
 * Implements the XML shell function "base"
 * dumps the current XML base of the node
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellBase(xmllintShellCtxtPtr ctxt,
             char *arg ATTRIBUTE_UNUSED, xmlNodePtr node,
             xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlChar *base;
    if (!ctxt)
        return 0;
    if (node == NULL) {
	fprintf(ctxt->output, "NULL\n");
	return (0);
    }

    base = xmlNodeGetBase(node->doc, node);

    if (base == NULL) {
        fprintf(ctxt->output, " No base found !!!\n");
    } else {
        fprintf(ctxt->output, "%s\n", base);
        xmlFree(base);
    }
    return (0);
}

/**
 * Implements the XML shell function "setbase"
 * change the current XML base of the node
 *
 * @param ctxt  the shell context
 * @param arg  the new base
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellSetBase(xmllintShellCtxtPtr ctxt ATTRIBUTE_UNUSED,
             char *arg ATTRIBUTE_UNUSED, xmlNodePtr node,
             xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlNodeSetBase(node, (xmlChar*) arg);
    return (0);
}

#ifdef LIBXML_XPATH_ENABLED
/**
 * Implements the XML shell function "setns"
 * register/unregister a prefix=namespace pair
 * on the XPath context
 *
 * @param ctxt  the shell context
 * @param arg  a string in prefix=nsuri format
 * @param node  unused
 * @param node2  unused
 * @returns 0 on success and a negative value otherwise.
 */
static int
xmllintShellRegisterNamespace(xmllintShellCtxtPtr ctxt, char *arg,
      xmlNodePtr node ATTRIBUTE_UNUSED, xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlChar* nsListDup;
    xmlChar* prefix;
    xmlChar* href;
    xmlChar* next;

    nsListDup = xmlStrdup((xmlChar *) arg);
    next = nsListDup;
    while(next != NULL) {
	/* skip spaces */
	/*while((*next) == ' ') next++;*/
	if((*next) == '\0') break;

	/* find prefix */
	prefix = next;
	next = (xmlChar*)xmlStrchr(next, '=');
	if(next == NULL) {
	    fprintf(ctxt->output, "setns: prefix=[nsuri] required\n");
	    xmlFree(nsListDup);
	    return(-1);
	}
	*(next++) = '\0';

	/* find href */
	href = next;
	next = (xmlChar*)xmlStrchr(next, ' ');
	if(next != NULL) {
	    *(next++) = '\0';
	}

	/* do register namespace */
	if(xmlXPathRegisterNs(ctxt->pctxt, prefix, href) != 0) {
	    fprintf(ctxt->output,"Error: unable to register NS with prefix=\"%s\" and href=\"%s\"\n", prefix, href);
	    xmlFree(nsListDup);
	    return(-1);
	}
    }

    xmlFree(nsListDup);
    return(0);
}
/**
 * Implements the XML shell function "setrootns"
 * which registers all namespaces declarations found on the root element.
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param root  the root element
 * @param node2  unused
 * @returns 0 on success and a negative value otherwise.
 */
static int
xmllintShellRegisterRootNamespaces(xmllintShellCtxtPtr ctxt, char *arg ATTRIBUTE_UNUSED,
      xmlNodePtr root, xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlNsPtr ns;

    if ((root == NULL) || (root->type != XML_ELEMENT_NODE) ||
        (root->nsDef == NULL) || (ctxt == NULL) || (ctxt->pctxt == NULL))
	return(-1);
    ns = root->nsDef;
    while (ns != NULL) {
        if (ns->prefix == NULL)
	    xmlXPathRegisterNs(ctxt->pctxt, BAD_CAST "defaultns", ns->href);
	else
	    xmlXPathRegisterNs(ctxt->pctxt, ns->prefix, ns->href);
        ns = ns->next;
    }
    return(0);
}
#endif

/**
 * Implements the XML shell function "grep"
 * dumps information about the node (namespace, attributes, content).
 *
 * @param ctxt  the shell context
 * @param arg  the string or regular expression to find
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellGrep(xmllintShellCtxtPtr ctxt ATTRIBUTE_UNUSED,
            char *arg, xmlNodePtr node, xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    if (!ctxt)
        return (0);
    if (node == NULL)
	return (0);
    if (arg == NULL)
	return (0);
#ifdef LIBXML_REGEXP_ENABLED
    if ((xmlStrchr((xmlChar *) arg, '?')) ||
	(xmlStrchr((xmlChar *) arg, '*')) ||
	(xmlStrchr((xmlChar *) arg, '.')) ||
	(xmlStrchr((xmlChar *) arg, '['))) {
    }
#endif
    while (node != NULL) {
        if (node->type == XML_COMMENT_NODE) {
	    if (xmlStrstr(node->content, (xmlChar *) arg)) {
		fprintf(ctxt->output, "%s : ", xmlGetNodePath(node));
                xmllintShellList(ctxt, NULL, node, NULL);
	    }
        } else if (node->type == XML_TEXT_NODE) {
	    if (xmlStrstr(node->content, (xmlChar *) arg)) {
		fprintf(ctxt->output, "%s : ", xmlGetNodePath(node->parent));
                xmllintShellList(ctxt, NULL, node->parent, NULL);
	    }
        }

        /*
         * Browse the full subtree, deep first
         */

        if ((node->type == XML_DOCUMENT_NODE) ||
            (node->type == XML_HTML_DOCUMENT_NODE)) {
            node = ((xmlDocPtr) node)->children;
        } else if ((node->children != NULL)
                   && (node->type != XML_ENTITY_REF_NODE)) {
            /* deep first */
            node = node->children;
        } else if (node->next != NULL) {
            /* then siblings */
            node = node->next;
        } else {
            /* go up to parents->next if needed */
            while (node != NULL) {
                if (node->parent != NULL) {
                    node = node->parent;
                }
                if (node->next != NULL) {
                    node = node->next;
                    break;
                }
                if (node->parent == NULL) {
                    node = NULL;
                    break;
                }
            }
	}
    }
    return (0);
}

/**
 * Implements the XML shell function "dir"
 * dumps information about the node (namespace, attributes, content).
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellDir(xmllintShellCtxtPtr ctxt ATTRIBUTE_UNUSED,
            char *arg ATTRIBUTE_UNUSED, xmlNodePtr node,
            xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    if (!ctxt)
        return (0);
    if (node == NULL) {
	fprintf(ctxt->output, "NULL\n");
	return (0);
    }
#ifdef LIBXML_DEBUG_ENABLED
    if ((node->type == XML_DOCUMENT_NODE) ||
        (node->type == XML_HTML_DOCUMENT_NODE)) {
        xmlDebugDumpDocumentHead(ctxt->output, (xmlDocPtr) node);
    } else if (node->type == XML_ATTRIBUTE_NODE) {
        xmlDebugDumpAttr(ctxt->output, (xmlAttrPtr) node, 0);
    } else {
        xmlDebugDumpOneNode(ctxt->output, node, 0);
    }
#endif
    return (0);
}

/**
 * Implements the XML shell function "dir"
 * dumps information about the node (namespace, attributes, content).
 *
 * @param ctxt  the shell context
 * @param value  the content as a string
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellSetContent(xmllintShellCtxtPtr ctxt ATTRIBUTE_UNUSED,
            char *value, xmlNodePtr node,
            xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlNodePtr results;
    xmlParserErrors ret;

    if (!ctxt)
        return (0);
    if (node == NULL) {
	fprintf(ctxt->output, "NULL\n");
	return (0);
    }
    if (value == NULL) {
        fprintf(ctxt->output, "NULL\n");
	return (0);
    }

    ret = xmlParseInNodeContext(node, value, strlen(value), 0, &results);
    if (ret == XML_ERR_OK) {
	if (node->children != NULL) {
	    xmlFreeNodeList(node->children);
	    node->children = NULL;
	    node->last = NULL;
	}
	xmlAddChildList(node, results);
    } else {
        fprintf(ctxt->output, "failed to parse content\n");
    }
    return (0);
}

#if defined(LIBXML_VALID_ENABLED) || defined(LIBXML_RELAXNG_ENABLED)
static void
xmllintShellPrintf(void *ctx, const char *msg, ...) {
    xmllintShellCtxtPtr sctxt = ctx;
    va_list ap;

    va_start(ap, msg);
    vfprintf(sctxt->output, msg, ap);
    va_end(ap);
}
#endif /* defined(LIBXML_VALID_ENABLED) || defined(LIBXML_RELAXNG_ENABLED) */

#ifdef LIBXML_RELAXNG_ENABLED
/**
 * Implements the XML shell function "relaxng"
 * validating the instance against a Relax-NG schemas
 *
 * @param sctxt  the shell context
 * @param schemas  the path to the Relax-NG schemas
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellRNGValidate(xmllintShellCtxtPtr sctxt, char *schemas,
            xmlNodePtr node ATTRIBUTE_UNUSED,
	    xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlRelaxNGPtr relaxngschemas;
    xmlRelaxNGParserCtxtPtr ctxt;
    xmlRelaxNGValidCtxtPtr vctxt;
    int ret;

    ctxt = xmlRelaxNGNewParserCtxt(schemas);
    xmlRelaxNGSetParserErrors(ctxt, xmllintShellPrintf, xmllintShellPrintf, sctxt);
    relaxngschemas = xmlRelaxNGParse(ctxt);
    xmlRelaxNGFreeParserCtxt(ctxt);
    if (relaxngschemas == NULL) {
	fprintf(sctxt->output,
		"Relax-NG schema %s failed to compile\n", schemas);
	return(-1);
    }
    vctxt = xmlRelaxNGNewValidCtxt(relaxngschemas);
    xmlRelaxNGSetValidErrors(vctxt, xmllintShellPrintf, xmllintShellPrintf, sctxt);
    ret = xmlRelaxNGValidateDoc(vctxt, sctxt->doc);
    if (ret == 0) {
	fprintf(sctxt->output, "%s validates\n", sctxt->filename);
    } else if (ret > 0) {
	fprintf(sctxt->output, "%s fails to validate\n", sctxt->filename);
    } else {
	fprintf(sctxt->output, "%s validation generated an internal error\n",
	       sctxt->filename);
    }
    xmlRelaxNGFreeValidCtxt(vctxt);
    if (relaxngschemas != NULL)
	xmlRelaxNGFree(relaxngschemas);
    return(0);
}
#endif

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Implements the XML shell function "cat"
 * dumps the serialization node content (XML or HTML).
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param node  a node
 * @param node2  unused
 * @returns 0
 */
static int
xmllintShellCat(xmllintShellCtxtPtr ctxt, char *arg ATTRIBUTE_UNUSED,
            xmlNodePtr node, xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    if (!ctxt)
        return (0);
    if (node == NULL) {
	fprintf(ctxt->output, "NULL\n");
	return (0);
    }
    if (ctxt->doc->type == XML_HTML_DOCUMENT_NODE) {
#ifdef LIBXML_HTML_ENABLED
        if (node->type == XML_HTML_DOCUMENT_NODE)
            htmlDocDump(ctxt->output, (htmlDocPtr) node);
        else
            htmlNodeDumpFile(ctxt->output, ctxt->doc, node);
#else
        if (node->type == XML_DOCUMENT_NODE)
            xmlDocDump(ctxt->output, (xmlDocPtr) node);
        else
            xmlElemDump(ctxt->output, ctxt->doc, node);
#endif /* LIBXML_HTML_ENABLED */
    } else {
        if (node->type == XML_DOCUMENT_NODE)
            xmlDocDump(ctxt->output, (xmlDocPtr) node);
        else
            xmlElemDump(ctxt->output, ctxt->doc, node);
    }
    fprintf(ctxt->output, "\n");
    return (0);
}
#endif /* LIBXML_OUTPUT_ENABLED */

/**
 * Implements the XML shell function "load"
 * loads a new document specified by the filename
 *
 * @param ctxt  the shell context
 * @param filename  the file name
 * @param node  unused
 * @param node2  unused
 * @returns 0 or -1 if loading failed
 */
static int
xmllintShellLoad(xmllintShellCtxtPtr ctxt, char *filename,
             xmlNodePtr node ATTRIBUTE_UNUSED,
             xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlDocPtr doc;
    int html = 0;

    if ((ctxt == NULL) || (filename == NULL)) return(-1);
    if (ctxt->doc != NULL)
        html = (ctxt->doc->type == XML_HTML_DOCUMENT_NODE);

    if (html) {
#ifdef LIBXML_HTML_ENABLED
        doc = htmlParseFile(filename, NULL);
#else
        fprintf(ctxt->output, "HTML support not compiled in\n");
        doc = NULL;
#endif /* LIBXML_HTML_ENABLED */
    } else {
        doc = xmlReadFile(filename,NULL,0);
    }
    if (doc != NULL) {
        if (ctxt->loaded == 1) {
            xmlFreeDoc(ctxt->doc);
        }
        ctxt->loaded = 1;
#ifdef LIBXML_XPATH_ENABLED
        xmlXPathFreeContext(ctxt->pctxt);
#endif /* LIBXML_XPATH_ENABLED */
        xmlFree(ctxt->filename);
        ctxt->doc = doc;
        ctxt->node = (xmlNodePtr) doc;
#ifdef LIBXML_XPATH_ENABLED
        ctxt->pctxt = xmlXPathNewContext(doc);
#endif /* LIBXML_XPATH_ENABLED */
        ctxt->filename = (char *) xmlCanonicPath((xmlChar *) filename);
    } else
        return (-1);
    return (0);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Implements the XML shell function "write"
 * Write the current node to the filename, it saves the serialization
 * of the subtree under the `node` specified
 *
 * @param ctxt  the shell context
 * @param filename  the file name
 * @param node  a node in the tree
 * @param node2  unused
 * @returns 0 or -1 in case of error
 */
static int
xmllintShellWrite(xmllintShellCtxtPtr ctxt, char *filename, xmlNodePtr node,
              xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    if (node == NULL)
        return (-1);
    if ((filename == NULL) || (filename[0] == 0)) {
        return (-1);
    }
    switch (node->type) {
        case XML_DOCUMENT_NODE:
            if (xmlSaveFile((char *) filename, ctxt->doc) < -1) {
                fprintf(ctxt->output,
                                "Failed to write to %s\n", filename);
                return (-1);
            }
            break;
        case XML_HTML_DOCUMENT_NODE:
#ifdef LIBXML_HTML_ENABLED
            if (htmlSaveFile((char *) filename, ctxt->doc) < 0) {
                fprintf(ctxt->output,
                                "Failed to write to %s\n", filename);
                return (-1);
            }
#else
            if (xmlSaveFile((char *) filename, ctxt->doc) < -1) {
                fprintf(ctxt->output,
                                "Failed to write to %s\n", filename);
                return (-1);
            }
#endif /* LIBXML_HTML_ENABLED */
            break;
        default:{
                FILE *f;

                f = fopen((char *) filename, "wb");
                if (f == NULL) {
                    fprintf(ctxt->output,
                                    "Failed to write to %s\n", filename);
                    return (-1);
                }
                xmlElemDump(f, ctxt->doc, node);
                fclose(f);
            }
    }
    return (0);
}

/**
 * Implements the XML shell function "save"
 * Write the current document to the filename, or it's original name
 *
 * @param ctxt  the shell context
 * @param filename  the file name (optional)
 * @param node  unused
 * @param node2  unused
 * @returns 0 or -1 in case of error
 */
static int
xmllintShellSave(xmllintShellCtxtPtr ctxt, char *filename,
             xmlNodePtr node ATTRIBUTE_UNUSED,
             xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    if ((ctxt == NULL) || (ctxt->doc == NULL))
        return (-1);
    if ((filename == NULL) || (filename[0] == 0))
        filename = ctxt->filename;
    if (filename == NULL)
        return (-1);
    switch (ctxt->doc->type) {
        case XML_DOCUMENT_NODE:
            if (xmlSaveFile((char *) filename, ctxt->doc) < 0) {
                fprintf(ctxt->output,
                                "Failed to save to %s\n", filename);
            }
            break;
        case XML_HTML_DOCUMENT_NODE:
#ifdef LIBXML_HTML_ENABLED
            if (htmlSaveFile((char *) filename, ctxt->doc) < 0) {
                fprintf(ctxt->output,
                                "Failed to save to %s\n", filename);
            }
#else
            if (xmlSaveFile((char *) filename, ctxt->doc) < 0) {
                fprintf(ctxt->output,
                                "Failed to save to %s\n", filename);
            }
#endif /* LIBXML_HTML_ENABLED */
            break;
        default:
            fprintf(ctxt->output,
	    "To save to subparts of a document use the 'write' command\n");
            return (-1);

    }
    return (0);
}
#endif /* LIBXML_OUTPUT_ENABLED */

#ifdef LIBXML_VALID_ENABLED
/**
 * Implements the XML shell function "validate"
 * Validate the document, if a DTD path is provided, then the validation
 * is done against the given DTD.
 *
 * @param ctxt  the shell context
 * @param dtd  the DTD URI (optional)
 * @param node  unused
 * @param node2  unused
 * @returns 0 or -1 in case of error
 */
static int
xmllintShellValidate(xmllintShellCtxtPtr ctxt, char *dtd,
                 xmlNodePtr node ATTRIBUTE_UNUSED,
                 xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlValidCtxt vctxt;
    int res = -1;

    if ((ctxt == NULL) || (ctxt->doc == NULL)) return(-1);
    memset(&vctxt, 0, sizeof(vctxt));
    vctxt.error = xmllintShellPrintf;
    vctxt.warning = xmllintShellPrintf;
    vctxt.userData = ctxt;

    if ((dtd == NULL) || (dtd[0] == 0)) {
        res = xmlValidateDocument(&vctxt, ctxt->doc);
    } else {
        xmlDtdPtr subset;

        subset = xmlParseDTD(NULL, (xmlChar *) dtd);
        if (subset != NULL) {
            res = xmlValidateDtd(&vctxt, ctxt->doc, subset);

            xmlFreeDtd(subset);
        }
    }
    return (res);
}
#endif /* LIBXML_VALID_ENABLED */

/**
 * Implements the XML shell function "du"
 * show the structure of the subtree under node `tree`
 * If `tree` is null, the command works on the current node.
 *
 * @param ctxt  the shell context
 * @param arg  unused
 * @param tree  a node defining a subtree
 * @param node2  unused
 * @returns 0 or -1 in case of error
 */
static int
xmllintShellDu(xmllintShellCtxtPtr ctxt,
           char *arg ATTRIBUTE_UNUSED, xmlNodePtr tree,
           xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlNodePtr node;
    int indent = 0, i;

    if (!ctxt)
	return (-1);

    if (tree == NULL)
        return (-1);
    node = tree;
    while (node != NULL) {
        if ((node->type == XML_DOCUMENT_NODE) ||
            (node->type == XML_HTML_DOCUMENT_NODE)) {
            fprintf(ctxt->output, "/\n");
        } else if (node->type == XML_ELEMENT_NODE) {
            for (i = 0; i < indent; i++)
                fprintf(ctxt->output, "  ");
            if ((node->ns) && (node->ns->prefix))
                fprintf(ctxt->output, "%s:", node->ns->prefix);
            fprintf(ctxt->output, "%s\n", node->name);
        } else {
        }

        /*
         * Browse the full subtree, deep first
         */

        if ((node->type == XML_DOCUMENT_NODE) ||
            (node->type == XML_HTML_DOCUMENT_NODE)) {
            node = ((xmlDocPtr) node)->children;
        } else if ((node->children != NULL)
                   && (node->type != XML_ENTITY_REF_NODE)) {
            /* deep first */
            node = node->children;
            indent++;
        } else if ((node != tree) && (node->next != NULL)) {
            /* then siblings */
            node = node->next;
        } else if (node != tree) {
            /* go up to parents->next if needed */
            while (node != tree) {
                if (node->parent != NULL) {
                    node = node->parent;
                    indent--;
                }
                if ((node != tree) && (node->next != NULL)) {
                    node = node->next;
                    break;
                }
                if (node->parent == NULL) {
                    node = NULL;
                    break;
                }
                if (node == tree) {
                    node = NULL;
                    break;
                }
            }
            /* exit condition */
            if (node == tree)
                node = NULL;
        } else
            node = NULL;
    }
    return (0);
}

/**
 * Implements the XML shell function "pwd"
 * Show the full path from the root to the node, if needed building
 * thumblers when similar elements exists at a given ancestor level.
 * The output is compatible with XPath commands.
 *
 * @param ctxt  the shell context
 * @param buffer  the output buffer
 * @param node  a node
 * @param node2  unused
 * @returns 0 or -1 in case of error
 */
static int
xmllintShellPwd(xmllintShellCtxtPtr ctxt ATTRIBUTE_UNUSED, char *buffer,
            xmlNodePtr node, xmlNodePtr node2 ATTRIBUTE_UNUSED)
{
    xmlChar *path;

    if ((node == NULL) || (buffer == NULL))
        return (-1);

    path = xmlGetNodePath(node);
    if (path == NULL)
	return (-1);

    /*
     * This test prevents buffer overflow, because this routine
     * is only called by xmllintShell, in which the second argument is
     * 500 chars long.
     * It is a dirty hack before a cleaner solution is found.
     * Documentation should mention that the second argument must
     * be at least 500 chars long, and could be stripped if too long.
     */
    snprintf(buffer, 499, "%s", path);
    buffer[499] = '0';
    xmlFree(path);

    return (0);
}

#define MAX_PROMPT_SIZE     500
#define MAX_ARG_SIZE        400
#define MAX_COMMAND_SIZE    100

/**
 * Read a string
 *
 * @param prompt  the prompt value
 * @returns a pointer to it or NULL on EOF the caller is expected to
 *     free the returned string.
 */
static char *
xmllintShellReadline(char *prompt) {
    char buf[MAX_PROMPT_SIZE+1];
    char *ret;
    int len;

#ifdef HAVE_LIBREADLINE
    if (isatty(STDIN_FILENO)) {
        char *line_read;

        /* Get a line from the user. */
        line_read = readline (prompt);

#ifdef HAVE_LIBHISTORY
        /* If the line has any text in it, save it on the history. */
        if (line_read && *line_read)
           add_history (line_read);
#endif

        return (line_read);
    }
#endif

    if (prompt != NULL)
       fprintf(stdout, "%s", prompt);
    fflush(stdout);
    if (!fgets(buf, MAX_PROMPT_SIZE, stdin))
        return(NULL);
    buf[MAX_PROMPT_SIZE] = 0;
    len = strlen(buf);
    ret = (char *) malloc(len + 1);
    if (ret != NULL) {
       memcpy (ret, buf, len + 1);
    }
    return(ret);
}

/**
 * Implements the XML shell
 * This allow to load, validate, view, modify and save a document
 * using a environment similar to a UNIX commandline.
 *
 * @param doc  the initial document
 * @param filename  the output buffer
 * @param output  the output FILE*, defaults to stdout if NULL
 */
void
xmllintShell(xmlDoc *doc, const char *filename, FILE * output)
{
    char prompt[MAX_PROMPT_SIZE] = "/ > ";
    char *cmdline = NULL, *cur;
    char command[MAX_COMMAND_SIZE];
    char arg[MAX_ARG_SIZE];
    int i;
    xmllintShellCtxtPtr ctxt;
#ifdef LIBXML_XPATH_ENABLED
    xmlXPathObjectPtr list;
#endif

    if (doc == NULL)
        return;
    if (filename == NULL)
        return;
    if (output == NULL)
        output = stdout;
    ctxt = (xmllintShellCtxtPtr) xmlMalloc(sizeof(xmllintShellCtxt));
    if (ctxt == NULL)
        return;
    ctxt->loaded = 0;
    ctxt->doc = doc;
    ctxt->output = output;
    ctxt->filename = (char *) xmlStrdup((xmlChar *) filename);
    ctxt->node = (xmlNodePtr) ctxt->doc;

#ifdef LIBXML_XPATH_ENABLED
    ctxt->pctxt = xmlXPathNewContext(ctxt->doc);
    if (ctxt->pctxt == NULL) {
        xmlFree(ctxt);
        return;
    }
#endif /* LIBXML_XPATH_ENABLED */
    while (1) {
        if (ctxt->node == (xmlNodePtr) ctxt->doc)
            snprintf(prompt, sizeof(prompt), "%s > ", "/");
        else if ((ctxt->node != NULL) && (ctxt->node->name) &&
                 (ctxt->node->ns) && (ctxt->node->ns->prefix))
            snprintf(prompt, sizeof(prompt), "%s:%s > ",
                     (ctxt->node->ns->prefix), ctxt->node->name);
        else if ((ctxt->node != NULL) && (ctxt->node->name))
            snprintf(prompt, sizeof(prompt), "%s > ", ctxt->node->name);
        else
            snprintf(prompt, sizeof(prompt), "? > ");
        prompt[sizeof(prompt) - 1] = 0;

        /*
         * Get a new command line
         */
        cmdline = xmllintShellReadline(prompt);
        if (cmdline == NULL)
            break;

        /*
         * Parse the command itself
         */
        cur = cmdline;
        while ((*cur == ' ') || (*cur == '\t'))
            cur++;
        i = 0;
        while ((*cur != ' ') && (*cur != '\t') &&
               (*cur != '\n') && (*cur != '\r') &&
               (i < (MAX_COMMAND_SIZE - 1))) {
            if (*cur == 0)
                break;
            command[i++] = *cur++;
        }
        command[i] = 0;
        if (i == 0)
            continue;

        /*
         * Parse the argument
         */
        while ((*cur == ' ') || (*cur == '\t'))
            cur++;
        i = 0;
        while ((*cur != '\n') && (*cur != '\r') && (*cur != 0) && (i < (MAX_ARG_SIZE-1))) {
            if (*cur == 0)
                break;
            arg[i++] = *cur++;
        }
        arg[i] = 0;

        /*
         * start interpreting the command
         */
        if (!strcmp(command, "exit"))
            break;
        if (!strcmp(command, "quit"))
            break;
        if (!strcmp(command, "bye"))
            break;
		if (!strcmp(command, "help")) {
		  fprintf(ctxt->output, "\tbase         display XML base of the node\n");
		  fprintf(ctxt->output, "\tsetbase URI  change the XML base of the node\n");
		  fprintf(ctxt->output, "\tbye          leave shell\n");
		  fprintf(ctxt->output, "\tcat [node]   display node or current node\n");
		  fprintf(ctxt->output, "\tcd [path]    change directory to path or to root\n");
		  fprintf(ctxt->output, "\tdir [path]   dumps information about the node (namespace, attributes, content)\n");
		  fprintf(ctxt->output, "\tdu [path]    show the structure of the subtree under path or the current node\n");
		  fprintf(ctxt->output, "\texit         leave shell\n");
		  fprintf(ctxt->output, "\thelp         display this help\n");
		  fprintf(ctxt->output, "\tfree         display memory usage\n");
		  fprintf(ctxt->output, "\tload [name]  load a new document with name\n");
		  fprintf(ctxt->output, "\tls [path]    list contents of path or the current directory\n");
		  fprintf(ctxt->output, "\tset xml_fragment replace the current node content with the fragment parsed in context\n");
#ifdef LIBXML_XPATH_ENABLED
		  fprintf(ctxt->output, "\txpath expr   evaluate the XPath expression in that context and print the result\n");
		  fprintf(ctxt->output, "\tsetns nsreg  register a namespace to a prefix in the XPath evaluation context\n");
		  fprintf(ctxt->output, "\t             format for nsreg is: prefix=[nsuri] (i.e. prefix= unsets a prefix)\n");
		  fprintf(ctxt->output, "\tsetrootns    register all namespace found on the root element\n");
		  fprintf(ctxt->output, "\t             the default namespace if any uses 'defaultns' prefix\n");
#endif /* LIBXML_XPATH_ENABLED */
		  fprintf(ctxt->output, "\tpwd          display current working directory\n");
		  fprintf(ctxt->output, "\twhereis      display absolute path of [path] or current working directory\n");
		  fprintf(ctxt->output, "\tquit         leave shell\n");
#ifdef LIBXML_OUTPUT_ENABLED
		  fprintf(ctxt->output, "\tsave [name]  save this document to name or the original name\n");
		  fprintf(ctxt->output, "\twrite [name] write the current node to the filename\n");
#endif /* LIBXML_OUTPUT_ENABLED */
#ifdef LIBXML_VALID_ENABLED
		  fprintf(ctxt->output, "\tvalidate     check the document for errors\n");
#endif /* LIBXML_VALID_ENABLED */
#ifdef LIBXML_RELAXNG_ENABLED
		  fprintf(ctxt->output, "\trelaxng rng  validate the document against the Relax-NG schemas\n");
#endif
		  fprintf(ctxt->output, "\tgrep string  search for a string in the subtree\n");
#ifdef LIBXML_VALID_ENABLED
        } else if (!strcmp(command, "validate")) {
            xmllintShellValidate(ctxt, arg, NULL, NULL);
#endif /* LIBXML_VALID_ENABLED */
        } else if (!strcmp(command, "load")) {
            xmllintShellLoad(ctxt, arg, NULL, NULL);
#ifdef LIBXML_RELAXNG_ENABLED
        } else if (!strcmp(command, "relaxng")) {
            xmllintShellRNGValidate(ctxt, arg, NULL, NULL);
#endif
#ifdef LIBXML_OUTPUT_ENABLED
        } else if (!strcmp(command, "save")) {
            xmllintShellSave(ctxt, arg, NULL, NULL);
        } else if (!strcmp(command, "write")) {
	    if (arg[0] == 0)
		fprintf(ctxt->output,
                        "Write command requires a filename argument\n");
	    else
		xmllintShellWrite(ctxt, arg, ctxt->node, NULL);
#endif /* LIBXML_OUTPUT_ENABLED */
        } else if (!strcmp(command, "grep")) {
            xmllintShellGrep(ctxt, arg, ctxt->node, NULL);
        } else if (!strcmp(command, "pwd")) {
            char dir[500];

            if (!xmllintShellPwd(ctxt, dir, ctxt->node, NULL))
                fprintf(ctxt->output, "%s\n", dir);
        } else if (!strcmp(command, "du")) {
            if (arg[0] == 0) {
                xmllintShellDu(ctxt, NULL, ctxt->node, NULL);
            } else {
#ifdef LIBXML_XPATH_ENABLED
                ctxt->pctxt->node = ctxt->node;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
                if (list != NULL) {
                    switch (list->type) {
                        case XPATH_UNDEFINED:
                            fprintf(ctxt->output,
                                            "%s: no such node\n", arg);
                            break;
                        case XPATH_NODESET:{
                            int indx;

                            if (list->nodesetval == NULL)
                                break;

                            for (indx = 0;
                                 indx < list->nodesetval->nodeNr;
                                 indx++)
                                xmllintShellDu(ctxt, NULL,
                                           list->nodesetval->
                                           nodeTab[indx], NULL);
                            break;
                        }
                        case XPATH_BOOLEAN:
                            fprintf(ctxt->output,
                                            "%s is a Boolean\n", arg);
                            break;
                        case XPATH_NUMBER:
                            fprintf(ctxt->output,
                                            "%s is a number\n", arg);
                            break;
                        case XPATH_STRING:
                            fprintf(ctxt->output,
                                            "%s is a string\n", arg);
                            break;
                        case XPATH_USERS:
                            fprintf(ctxt->output,
                                            "%s is user-defined\n", arg);
                            break;
                        case XPATH_XSLT_TREE:
                            fprintf(ctxt->output,
                                            "%s is an XSLT value tree\n",
                                            arg);
                            break;
                    }
                    xmlXPathFreeObject(list);
                } else {
                    fprintf(ctxt->output,
                                    "%s: no such node\n", arg);
                }
                ctxt->pctxt->node = NULL;
#endif /* LIBXML_XPATH_ENABLED */
            }
        } else if (!strcmp(command, "base")) {
            xmllintShellBase(ctxt, NULL, ctxt->node, NULL);
        } else if (!strcmp(command, "set")) {
	    xmllintShellSetContent(ctxt, arg, ctxt->node, NULL);
#ifdef LIBXML_XPATH_ENABLED
        } else if (!strcmp(command, "setns")) {
            if (arg[0] == 0) {
		fprintf(ctxt->output,
				"setns: prefix=[nsuri] required\n");
            } else {
                xmllintShellRegisterNamespace(ctxt, arg, NULL, NULL);
            }
        } else if (!strcmp(command, "setrootns")) {
	    xmlNodePtr root;

	    root = xmlDocGetRootElement(ctxt->doc);
	    xmllintShellRegisterRootNamespaces(ctxt, NULL, root, NULL);
#ifdef LIBXML_DEBUG_ENABLED
        } else if (!strcmp(command, "xpath")) {
            if (arg[0] == 0) {
		fprintf(ctxt->output,
				"xpath: expression required\n");
	    } else {
                ctxt->pctxt->node = ctxt->node;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
		xmlXPathDebugDumpObject(ctxt->output, list, 0);
		xmlXPathFreeObject(list);
	    }
#endif /* LIBXML_DEBUG_ENABLED */
#endif /* LIBXML_XPATH_ENABLED */
        } else if (!strcmp(command, "setbase")) {
            xmllintShellSetBase(ctxt, arg, ctxt->node, NULL);
        } else if ((!strcmp(command, "ls")) || (!strcmp(command, "dir"))) {
            int dir = (!strcmp(command, "dir"));

            if (arg[0] == 0) {
                if (dir)
                    xmllintShellDir(ctxt, NULL, ctxt->node, NULL);
                else
                    xmllintShellList(ctxt, NULL, ctxt->node, NULL);
            } else {
#ifdef LIBXML_XPATH_ENABLED
                ctxt->pctxt->node = ctxt->node;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
                if (list != NULL) {
                    switch (list->type) {
                        case XPATH_UNDEFINED:
                            fprintf(ctxt->output,
                                            "%s: no such node\n", arg);
                            break;
                        case XPATH_NODESET:{
                                int indx;

				if (list->nodesetval == NULL)
				    break;

                                for (indx = 0;
                                     indx < list->nodesetval->nodeNr;
                                     indx++) {
                                    if (dir)
                                        xmllintShellDir(ctxt, NULL,
                                                    list->nodesetval->
                                                    nodeTab[indx], NULL);
                                    else
                                        xmllintShellList(ctxt, NULL,
                                                     list->nodesetval->
                                                     nodeTab[indx], NULL);
                                }
                                break;
                            }
                        case XPATH_BOOLEAN:
                            fprintf(ctxt->output,
                                            "%s is a Boolean\n", arg);
                            break;
                        case XPATH_NUMBER:
                            fprintf(ctxt->output,
                                            "%s is a number\n", arg);
                            break;
                        case XPATH_STRING:
                            fprintf(ctxt->output,
                                            "%s is a string\n", arg);
                            break;
                        case XPATH_USERS:
                            fprintf(ctxt->output,
                                            "%s is user-defined\n", arg);
                            break;
                        case XPATH_XSLT_TREE:
                            fprintf(ctxt->output,
                                            "%s is an XSLT value tree\n",
                                            arg);
                            break;
                    }
                    xmlXPathFreeObject(list);
                } else {
                    fprintf(ctxt->output,
                                    "%s: no such node\n", arg);
                }
                ctxt->pctxt->node = NULL;
#endif /* LIBXML_XPATH_ENABLED */
            }
        } else if (!strcmp(command, "whereis")) {
            char dir[500];

            if (arg[0] == 0) {
                if (!xmllintShellPwd(ctxt, dir, ctxt->node, NULL))
                    fprintf(ctxt->output, "%s\n", dir);
            } else {
#ifdef LIBXML_XPATH_ENABLED
                ctxt->pctxt->node = ctxt->node;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
                if (list != NULL) {
                    switch (list->type) {
                        case XPATH_UNDEFINED:
                            fprintf(ctxt->output,
                                            "%s: no such node\n", arg);
                            break;
                        case XPATH_NODESET:{
                                int indx;

				if (list->nodesetval == NULL)
				    break;

                                for (indx = 0;
                                     indx < list->nodesetval->nodeNr;
                                     indx++) {
                                    if (!xmllintShellPwd(ctxt, dir, list->nodesetval->
                                                     nodeTab[indx], NULL))
                                        fprintf(ctxt->output, "%s\n", dir);
                                }
                                break;
                            }
                        case XPATH_BOOLEAN:
                            fprintf(ctxt->output,
                                            "%s is a Boolean\n", arg);
                            break;
                        case XPATH_NUMBER:
                            fprintf(ctxt->output,
                                            "%s is a number\n", arg);
                            break;
                        case XPATH_STRING:
                            fprintf(ctxt->output,
                                            "%s is a string\n", arg);
                            break;
                        case XPATH_USERS:
                            fprintf(ctxt->output,
                                            "%s is user-defined\n", arg);
                            break;
                        case XPATH_XSLT_TREE:
                            fprintf(ctxt->output,
                                            "%s is an XSLT value tree\n",
                                            arg);
                            break;
                    }
                    xmlXPathFreeObject(list);
                } else {
                    fprintf(ctxt->output,
                                    "%s: no such node\n", arg);
                }
                ctxt->pctxt->node = NULL;
#endif /* LIBXML_XPATH_ENABLED */
            }
        } else if (!strcmp(command, "cd")) {
            if (arg[0] == 0) {
                ctxt->node = (xmlNodePtr) ctxt->doc;
            } else {
#ifdef LIBXML_XPATH_ENABLED
                int l;

                ctxt->pctxt->node = ctxt->node;
		l = strlen(arg);
		if ((l >= 2) && (arg[l - 1] == '/'))
		    arg[l - 1] = 0;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
                if (list != NULL) {
                    switch (list->type) {
                        case XPATH_UNDEFINED:
                            fprintf(ctxt->output,
                                            "%s: no such node\n", arg);
                            break;
                        case XPATH_NODESET:
                            if (list->nodesetval != NULL) {
				if (list->nodesetval->nodeNr == 1) {
				    ctxt->node = list->nodesetval->nodeTab[0];
				    if ((ctxt->node != NULL) &&
				        (ctxt->node->type ==
					 XML_NAMESPACE_DECL)) {
					fprintf(ctxt->output,
						    "cannot cd to namespace\n");
					ctxt->node = NULL;
				    }
				} else
				    fprintf(ctxt->output,
						    "%s is a %d Node Set\n",
						    arg,
						    list->nodesetval->nodeNr);
                            } else
                                fprintf(ctxt->output,
                                                "%s is an empty Node Set\n",
                                                arg);
                            break;
                        case XPATH_BOOLEAN:
                            fprintf(ctxt->output,
                                            "%s is a Boolean\n", arg);
                            break;
                        case XPATH_NUMBER:
                            fprintf(ctxt->output,
                                            "%s is a number\n", arg);
                            break;
                        case XPATH_STRING:
                            fprintf(ctxt->output,
                                            "%s is a string\n", arg);
                            break;
                        case XPATH_USERS:
                            fprintf(ctxt->output,
                                            "%s is user-defined\n", arg);
                            break;
                        case XPATH_XSLT_TREE:
                            fprintf(ctxt->output,
                                            "%s is an XSLT value tree\n",
                                            arg);
                            break;
                    }
                    xmlXPathFreeObject(list);
                } else {
                    fprintf(ctxt->output,
                                    "%s: no such node\n", arg);
                }
                ctxt->pctxt->node = NULL;
#endif /* LIBXML_XPATH_ENABLED */
            }
#ifdef LIBXML_OUTPUT_ENABLED
        } else if (!strcmp(command, "cat")) {
            if (arg[0] == 0) {
                xmllintShellCat(ctxt, NULL, ctxt->node, NULL);
            } else {
#ifdef LIBXML_XPATH_ENABLED
                ctxt->pctxt->node = ctxt->node;
                list = xmlXPathEval((xmlChar *) arg, ctxt->pctxt);
                if (list != NULL) {
                    switch (list->type) {
                        case XPATH_UNDEFINED:
                            fprintf(ctxt->output,
                                            "%s: no such node\n", arg);
                            break;
                        case XPATH_NODESET:{
                                int indx;

				if (list->nodesetval == NULL)
				    break;

                                for (indx = 0;
                                     indx < list->nodesetval->nodeNr;
                                     indx++) {
                                    if (i > 0)
                                        fprintf(ctxt->output, " -------\n");
                                    xmllintShellCat(ctxt, NULL,
                                                list->nodesetval->
                                                nodeTab[indx], NULL);
                                }
                                break;
                            }
                        case XPATH_BOOLEAN:
                            fprintf(ctxt->output,
                                            "%s is a Boolean\n", arg);
                            break;
                        case XPATH_NUMBER:
                            fprintf(ctxt->output,
                                            "%s is a number\n", arg);
                            break;
                        case XPATH_STRING:
                            fprintf(ctxt->output,
                                            "%s is a string\n", arg);
                            break;
                        case XPATH_USERS:
                            fprintf(ctxt->output,
                                            "%s is user-defined\n", arg);
                            break;
                        case XPATH_XSLT_TREE:
                            fprintf(ctxt->output,
                                            "%s is an XSLT value tree\n",
                                            arg);
                            break;
                    }
                    xmlXPathFreeObject(list);
                } else {
                    fprintf(ctxt->output,
                                    "%s: no such node\n", arg);
                }
                ctxt->pctxt->node = NULL;
#endif /* LIBXML_XPATH_ENABLED */
            }
#endif /* LIBXML_OUTPUT_ENABLED */
        } else {
            fprintf(ctxt->output,
                            "Unknown command %s\n", command);
        }
        free(cmdline);          /* not xmlFree here ! */
	cmdline = NULL;
    }
#ifdef LIBXML_XPATH_ENABLED
    xmlXPathFreeContext(ctxt->pctxt);
#endif /* LIBXML_XPATH_ENABLED */
    if (ctxt->loaded) {
        xmlFreeDoc(ctxt->doc);
    }
    if (ctxt->filename != NULL)
        xmlFree(ctxt->filename);
    xmlFree(ctxt);
    if (cmdline != NULL)
        free(cmdline);          /* not xmlFree here ! */
}
