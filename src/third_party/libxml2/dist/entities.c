/*
 * entities.c : implementation for the XML entities handling
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

#include <string.h>
#include <stdlib.h>

#include <libxml/xmlmemory.h>
#include <libxml/hash.h>
#include <libxml/entities.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlerror.h>
#include <libxml/dict.h>
#include <libxml/xmlsave.h>

#include "private/entities.h"
#include "private/error.h"
#include "private/io.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

/*
 * The XML predefined entities.
 */

static xmlEntity xmlEntityLt = {
    NULL, XML_ENTITY_DECL, BAD_CAST "lt",
    NULL, NULL, NULL, NULL, NULL, NULL,
    BAD_CAST "<", BAD_CAST "<", 1,
    XML_INTERNAL_PREDEFINED_ENTITY,
    NULL, NULL, NULL, NULL, 0, 0, 0
};
static xmlEntity xmlEntityGt = {
    NULL, XML_ENTITY_DECL, BAD_CAST "gt",
    NULL, NULL, NULL, NULL, NULL, NULL,
    BAD_CAST ">", BAD_CAST ">", 1,
    XML_INTERNAL_PREDEFINED_ENTITY,
    NULL, NULL, NULL, NULL, 0, 0, 0
};
static xmlEntity xmlEntityAmp = {
    NULL, XML_ENTITY_DECL, BAD_CAST "amp",
    NULL, NULL, NULL, NULL, NULL, NULL,
    BAD_CAST "&", BAD_CAST "&", 1,
    XML_INTERNAL_PREDEFINED_ENTITY,
    NULL, NULL, NULL, NULL, 0, 0, 0
};
static xmlEntity xmlEntityQuot = {
    NULL, XML_ENTITY_DECL, BAD_CAST "quot",
    NULL, NULL, NULL, NULL, NULL, NULL,
    BAD_CAST "\"", BAD_CAST "\"", 1,
    XML_INTERNAL_PREDEFINED_ENTITY,
    NULL, NULL, NULL, NULL, 0, 0, 0
};
static xmlEntity xmlEntityApos = {
    NULL, XML_ENTITY_DECL, BAD_CAST "apos",
    NULL, NULL, NULL, NULL, NULL, NULL,
    BAD_CAST "'", BAD_CAST "'", 1,
    XML_INTERNAL_PREDEFINED_ENTITY,
    NULL, NULL, NULL, NULL, 0, 0, 0
};

/**
 * Frees the entity.
 *
 * @param entity  an entity
 */
void
xmlFreeEntity(xmlEntity *entity)
{
    xmlDictPtr dict = NULL;

    if (entity == NULL)
        return;

    if (entity->doc != NULL)
        dict = entity->doc->dict;


    if ((entity->children) &&
        (entity == (xmlEntityPtr) entity->children->parent))
        xmlFreeNodeList(entity->children);
    if ((entity->name != NULL) &&
        ((dict == NULL) || (!xmlDictOwns(dict, entity->name))))
        xmlFree((char *) entity->name);
    if (entity->ExternalID != NULL)
        xmlFree((char *) entity->ExternalID);
    if (entity->SystemID != NULL)
        xmlFree((char *) entity->SystemID);
    if (entity->URI != NULL)
        xmlFree((char *) entity->URI);
    if (entity->content != NULL)
        xmlFree((char *) entity->content);
    if (entity->orig != NULL)
        xmlFree((char *) entity->orig);
    xmlFree(entity);
}

/*
 * internal routine doing the entity node structures allocations
 */
static xmlEntityPtr
xmlCreateEntity(xmlDocPtr doc, const xmlChar *name, int type,
	        const xmlChar *publicId, const xmlChar *systemId,
	        const xmlChar *content) {
    xmlEntityPtr ret;

    ret = (xmlEntityPtr) xmlMalloc(sizeof(xmlEntity));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlEntity));
    ret->doc = doc;
    ret->type = XML_ENTITY_DECL;

    /*
     * fill the structure.
     */
    ret->etype = (xmlEntityType) type;
    if ((doc == NULL) || (doc->dict == NULL))
	ret->name = xmlStrdup(name);
    else
        ret->name = xmlDictLookup(doc->dict, name, -1);
    if (ret->name == NULL)
        goto error;
    if (publicId != NULL) {
        ret->ExternalID = xmlStrdup(publicId);
        if (ret->ExternalID == NULL)
            goto error;
    }
    if (systemId != NULL) {
        ret->SystemID = xmlStrdup(systemId);
        if (ret->SystemID == NULL)
            goto error;
    }
    if (content != NULL) {
        ret->length = xmlStrlen(content);
	ret->content = xmlStrndup(content, ret->length);
        if (ret->content == NULL)
            goto error;
     } else {
        ret->length = 0;
        ret->content = NULL;
    }
    ret->URI = NULL; /* to be computed by the layer knowing
			the defining entity */
    ret->orig = NULL;

    return(ret);

error:
    xmlFreeEntity(ret);
    return(NULL);
}

/**
 * Register a new entity for this document.
 *
 * @since 2.13.0
 *
 * @param doc  the document
 * @param extSubset  add to the external or internal subset
 * @param name  the entity name
 * @param type  an xmlEntityType value
 * @param publicId  the publid identifier (optional)
 * @param systemId  the system identifier (URL) (optional)
 * @param content  the entity content
 * @param out  pointer to resulting entity (optional)
 * @returns an xmlParserErrors error code.
 */
int
xmlAddEntity(xmlDoc *doc, int extSubset, const xmlChar *name, int type,
	  const xmlChar *publicId, const xmlChar *systemId,
	  const xmlChar *content, xmlEntity **out) {
    xmlDtdPtr dtd;
    xmlDictPtr dict = NULL;
    xmlEntitiesTablePtr table = NULL;
    xmlEntityPtr ret, predef;
    int res;

    if (out != NULL)
        *out = NULL;
    if ((doc == NULL) || (name == NULL))
	return(XML_ERR_ARGUMENT);
    dict = doc->dict;

    if (extSubset)
        dtd = doc->extSubset;
    else
        dtd = doc->intSubset;
    if (dtd == NULL)
        return(XML_DTD_NO_DTD);

    switch (type) {
        case XML_INTERNAL_GENERAL_ENTITY:
        case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
        case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
            predef = xmlGetPredefinedEntity(name);
            if (predef != NULL) {
                int valid = 0;

                /* 4.6 Predefined Entities */
                if ((type == XML_INTERNAL_GENERAL_ENTITY) &&
                    (content != NULL)) {
                    int c = predef->content[0];

                    if (((content[0] == c) && (content[1] == 0)) &&
                        ((c == '>') || (c == '\'') || (c == '"'))) {
                        valid = 1;
                    } else if ((content[0] == '&') && (content[1] == '#')) {
                        if (content[2] == 'x') {
                            xmlChar *hex = BAD_CAST "0123456789ABCDEF";
                            xmlChar ref[] = "00;";

                            ref[0] = hex[c / 16 % 16];
                            ref[1] = hex[c % 16];
                            if (xmlStrcasecmp(&content[3], ref) == 0)
                                valid = 1;
                        } else {
                            xmlChar ref[] = "00;";

                            ref[0] = '0' + c / 10 % 10;
                            ref[1] = '0' + c % 10;
                            if (xmlStrEqual(&content[2], ref))
                                valid = 1;
                        }
                    }
                }
                if (!valid)
                    return(XML_ERR_REDECL_PREDEF_ENTITY);
            }
	    if (dtd->entities == NULL) {
		dtd->entities = xmlHashCreateDict(0, dict);
                if (dtd->entities == NULL)
                    return(XML_ERR_NO_MEMORY);
            }
	    table = dtd->entities;
	    break;
        case XML_INTERNAL_PARAMETER_ENTITY:
        case XML_EXTERNAL_PARAMETER_ENTITY:
	    if (dtd->pentities == NULL) {
		dtd->pentities = xmlHashCreateDict(0, dict);
                if (dtd->pentities == NULL)
                    return(XML_ERR_NO_MEMORY);
            }
	    table = dtd->pentities;
	    break;
        default:
	    return(XML_ERR_ARGUMENT);
    }
    ret = xmlCreateEntity(dtd->doc, name, type, publicId, systemId, content);
    if (ret == NULL)
        return(XML_ERR_NO_MEMORY);

    res = xmlHashAdd(table, name, ret);
    if (res < 0) {
        xmlFreeEntity(ret);
        return(XML_ERR_NO_MEMORY);
    } else if (res == 0) {
	/*
	 * entity was already defined at another level.
	 */
        xmlFreeEntity(ret);
	return(XML_WAR_ENTITY_REDEFINED);
    }

    /*
     * Link it to the DTD
     */
    ret->parent = dtd;
    ret->doc = dtd->doc;
    if (dtd->last == NULL) {
	dtd->children = dtd->last = (xmlNodePtr) ret;
    } else {
	dtd->last->next = (xmlNodePtr) ret;
	ret->prev = dtd->last;
	dtd->last = (xmlNodePtr) ret;
    }

    if (out != NULL)
        *out = ret;
    return(0);
}

/**
 * Look up a predefined entity.
 *
 * @param name  the entity name
 * @returns the entity, or NULL if not found.
 */
xmlEntity *
xmlGetPredefinedEntity(const xmlChar *name) {
    if (name == NULL) return(NULL);
    switch (name[0]) {
        case 'l':
	    if (xmlStrEqual(name, BAD_CAST "lt"))
	        return(&xmlEntityLt);
	    break;
        case 'g':
	    if (xmlStrEqual(name, BAD_CAST "gt"))
	        return(&xmlEntityGt);
	    break;
        case 'a':
	    if (xmlStrEqual(name, BAD_CAST "amp"))
	        return(&xmlEntityAmp);
	    if (xmlStrEqual(name, BAD_CAST "apos"))
	        return(&xmlEntityApos);
	    break;
        case 'q':
	    if (xmlStrEqual(name, BAD_CAST "quot"))
	        return(&xmlEntityQuot);
	    break;
	default:
	    break;
    }
    return(NULL);
}

/**
 * Add a new entity to the document's external subset.
 *
 * #xmlAddEntity offers better error handling.
 *
 * @param doc  the document
 * @param name  the entity name
 * @param type  an xmlEntityType value
 * @param publicId  the publid identifier (optional)
 * @param systemId  the system identifier (URL) (optional)
 * @param content  the entity content
 * @returns a pointer to the entity or NULL in case of error
 */
xmlEntity *
xmlAddDtdEntity(xmlDoc *doc, const xmlChar *name, int type,
	        const xmlChar *publicId, const xmlChar *systemId,
		const xmlChar *content) {
    xmlEntityPtr ret;

    xmlAddEntity(doc, 1, name, type, publicId, systemId, content, &ret);
    return(ret);
}

/**
 * Add a new entity to the document's internal subset.
 *
 * #xmlAddEntity offers better error handling.
 *
 * @param doc  the document
 * @param name  the entity name
 * @param type  an xmlEntityType value
 * @param publicId  the publid identifier (optional)
 * @param systemId  the system identifier (URL) (optional)
 * @param content  the entity content
 * @returns a pointer to the entity or NULL in case of error
 */
xmlEntity *
xmlAddDocEntity(xmlDoc *doc, const xmlChar *name, int type,
	        const xmlChar *publicId, const xmlChar *systemId,
	        const xmlChar *content) {
    xmlEntityPtr ret;

    xmlAddEntity(doc, 0, name, type, publicId, systemId, content, &ret);
    return(ret);
}

/**
 * Create a new entity.
 *
 * Like #xmlAddDocEntity, but if `doc` is NULL or has no internal
 * subset defined, an unlinked entity will be returned. It is then
 * the responsibility of the caller to link it to the document later
 * or free it when not needed anymore.
 *
 * @param doc  the document (optional)
 * @param name  the entity name
 * @param type  an xmlEntityType value
 * @param publicId  the publid identifier (optional)
 * @param systemId  the system identifier (URL) (optional)
 * @param content  the entity content
 * @returns a pointer to the entity or NULL in case of error
 */
xmlEntity *
xmlNewEntity(xmlDoc *doc, const xmlChar *name, int type,
	     const xmlChar *publicId, const xmlChar *systemId,
	     const xmlChar *content) {
    if ((doc != NULL) && (doc->intSubset != NULL)) {
	return(xmlAddDocEntity(doc, name, type, publicId, systemId, content));
    }
    if (name == NULL)
        return(NULL);
    return(xmlCreateEntity(doc, name, type, publicId, systemId, content));
}

/**
 * Look up an entity in a table.
 *
 * @param table  an entity table
 * @param name  the entity name
 * @returns a pointer to the entity or NULL if not found.
 */
static xmlEntityPtr
xmlGetEntityFromTable(xmlEntitiesTablePtr table, const xmlChar *name) {
    return((xmlEntityPtr) xmlHashLookup(table, name));
}

/**
 * Look up a paramater entity in the internal and external subset
 * of `doc`.
 *
 * @param doc  the document
 * @param name  the entity name
 * @returns a pointer to the entity or NULL if not found.
 */
xmlEntity *
xmlGetParameterEntity(xmlDoc *doc, const xmlChar *name) {
    xmlEntitiesTablePtr table;
    xmlEntityPtr ret;

    if (doc == NULL)
	return(NULL);
    if ((doc->intSubset != NULL) && (doc->intSubset->pentities != NULL)) {
	table = (xmlEntitiesTablePtr) doc->intSubset->pentities;
	ret = xmlGetEntityFromTable(table, name);
	if (ret != NULL)
	    return(ret);
    }
    if ((doc->extSubset != NULL) && (doc->extSubset->pentities != NULL)) {
	table = (xmlEntitiesTablePtr) doc->extSubset->pentities;
	return(xmlGetEntityFromTable(table, name));
    }
    return(NULL);
}

/**
 * Look up a general entity in the external subset of `doc`.
 *
 * @param doc  the document
 * @param name  the entity name
 * @returns a pointer to the entity or NULL if not found.
 */
xmlEntity *
xmlGetDtdEntity(xmlDoc *doc, const xmlChar *name) {
    xmlEntitiesTablePtr table;

    if (doc == NULL)
	return(NULL);
    if ((doc->extSubset != NULL) && (doc->extSubset->entities != NULL)) {
	table = (xmlEntitiesTablePtr) doc->extSubset->entities;
	return(xmlGetEntityFromTable(table, name));
    }
    return(NULL);
}

/**
 * Look up a general entity in the internal and external subset
 * of `doc`. Also checks for predefined entities.
 *
 * @param doc  the document referencing the entity
 * @param name  the entity name
 * @returns a pointer to the entity or NULL if not found.
 */
xmlEntity *
xmlGetDocEntity(const xmlDoc *doc, const xmlChar *name) {
    xmlEntityPtr cur;
    xmlEntitiesTablePtr table;

    if (doc != NULL) {
	if ((doc->intSubset != NULL) && (doc->intSubset->entities != NULL)) {
	    table = (xmlEntitiesTablePtr) doc->intSubset->entities;
	    cur = xmlGetEntityFromTable(table, name);
	    if (cur != NULL)
		return(cur);
	}
	if (doc->standalone != 1) {
	    if ((doc->extSubset != NULL) &&
		(doc->extSubset->entities != NULL)) {
		table = (xmlEntitiesTablePtr) doc->extSubset->entities;
		cur = xmlGetEntityFromTable(table, name);
		if (cur != NULL)
		    return(cur);
	    }
	}
    }
    return(xmlGetPredefinedEntity(name));
}

/**
 * Replace special characters with predefined entities or numeric
 * character references.
 *
 * If `doc` is NULL or an XML document, replaces `<`, `>` and `&`
 * with predefined entities. Carriage return is replaced with
 * `&#13;`. If `doc` or its encoding are NULL, non-ASCII
 * characters are replaced with a hexadecimal character reference.
 *
 * If `doc` is an HTML document, follows the HTML serialization
 * rules.
 *
 * Silently removes some invalid characters like ASCII control
 * codes.
 *
 * See #xmlEncodeSpecialChars for an alternative.
 *
 * @param doc  the document containing the string (optional)
 * @param input  A string to convert to XML.
 * @returns a newly allocated string with substitutions.
 */
xmlChar *
xmlEncodeEntitiesReentrant(xmlDoc *doc, const xmlChar *input) {
    int flags = 0;

    if (input == NULL)
        return(NULL);

    if ((doc != NULL) && (doc->type == XML_HTML_DOCUMENT_NODE))
        flags |= XML_ESCAPE_HTML;
    else if ((doc == NULL) || (doc->encoding == NULL))
        flags |= XML_ESCAPE_NON_ASCII;

    return(xmlEscapeText(input, flags));
}

/**
 * Replace special characters with predefined entities or numeric
 * character references.
 *
 * Replaces `<`, `>`, `&` and `"` with predefined entities. Carriage
 * return is replaced with `&#13;`.
 *
 * @param doc  unused
 * @param input  A string to convert to XML.
 * @returns a newly allocated string with substitutions.
 */
xmlChar *
xmlEncodeSpecialChars(const xmlDoc *doc ATTRIBUTE_UNUSED,
                      const xmlChar *input) {
    if (input == NULL)
        return(NULL);

    return(xmlEscapeText(input, XML_ESCAPE_QUOT));
}

/**
 * Create and initialize an empty entities hash table.
 *
 * @deprecated Internal function, don't use.
 *
 * @returns the xmlEntitiesTable just created or NULL in case of error.
 */
xmlEntitiesTable *
xmlCreateEntitiesTable(void) {
    return((xmlEntitiesTablePtr) xmlHashCreate(0));
}

/**
 * Deallocate the memory used by an entities in the hash table.
 *
 * @param entity  An entity
 * @param name  its name
 */
static void
xmlFreeEntityWrapper(void *entity, const xmlChar *name ATTRIBUTE_UNUSED) {
    if (entity != NULL)
	xmlFreeEntity((xmlEntityPtr) entity);
}

/**
 * Deallocate the memory used by an entities hash table.
 *
 * @deprecated Internal function, don't use.
 *
 * @param table  An entity table
 */
void
xmlFreeEntitiesTable(xmlEntitiesTable *table) {
    xmlHashFree(table, xmlFreeEntityWrapper);
}

/**
 * Build a copy of an entity
 *
 * @param payload  An entity
 * @param name  unused
 * @returns the new xmlEntities or NULL in case of error.
 */
static void *
xmlCopyEntity(void *payload, const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlEntityPtr ent = (xmlEntityPtr) payload;
    xmlEntityPtr cur;

    cur = (xmlEntityPtr) xmlMalloc(sizeof(xmlEntity));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlEntity));
    cur->type = XML_ENTITY_DECL;

    cur->etype = ent->etype;
    if (ent->name != NULL) {
	cur->name = xmlStrdup(ent->name);
        if (cur->name == NULL)
            goto error;
    }
    if (ent->ExternalID != NULL) {
	cur->ExternalID = xmlStrdup(ent->ExternalID);
        if (cur->ExternalID == NULL)
            goto error;
    }
    if (ent->SystemID != NULL) {
	cur->SystemID = xmlStrdup(ent->SystemID);
        if (cur->SystemID == NULL)
            goto error;
    }
    if (ent->content != NULL) {
	cur->content = xmlStrdup(ent->content);
        if (cur->content == NULL)
            goto error;
    }
    if (ent->orig != NULL) {
	cur->orig = xmlStrdup(ent->orig);
        if (cur->orig == NULL)
            goto error;
    }
    if (ent->URI != NULL) {
	cur->URI = xmlStrdup(ent->URI);
        if (cur->URI == NULL)
            goto error;
    }
    return(cur);

error:
    xmlFreeEntity(cur);
    return(NULL);
}

/**
 * Build a copy of an entity table.
 *
 * @deprecated Internal function, don't use.
 *
 * @param table  An entity table
 * @returns the new xmlEntitiesTable or NULL in case of error.
 */
xmlEntitiesTable *
xmlCopyEntitiesTable(xmlEntitiesTable *table) {
    return(xmlHashCopySafe(table, xmlCopyEntity, xmlFreeEntityWrapper));
}

#ifdef LIBXML_OUTPUT_ENABLED

/**
 * This will dump the content of the entity table as an XML DTD
 * definition.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  An XML buffer.
 * @param ent  An entity table
 */
void
xmlDumpEntityDecl(xmlBuffer *buf, xmlEntity *ent) {
    xmlSaveCtxtPtr save;

    if ((buf == NULL) || (ent == NULL))
        return;

    save = xmlSaveToBuffer(buf, NULL, 0);
    xmlSaveTree(save, (xmlNodePtr) ent);
    if (xmlSaveFinish(save) != XML_ERR_OK)
        xmlFree(xmlBufferDetach(buf));
}

/**
 * When using the hash table scan function, arguments need to be
 * reversed.
 *
 * @param ent  an entity table
 * @param save  a save context
 * @param name  unused
 */
static void
xmlDumpEntityDeclScan(void *ent, void *save,
                      const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlSaveTree(save, ent);
}

/**
 * This will dump the content of the entity table as an XML DTD
 * definition.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  An XML buffer.
 * @param table  An entity table
 */
void
xmlDumpEntitiesTable(xmlBuffer *buf, xmlEntitiesTable *table) {
    xmlSaveCtxtPtr save;

    if ((buf == NULL) || (table == NULL))
        return;

    save = xmlSaveToBuffer(buf, NULL, 0);
    xmlHashScan(table, xmlDumpEntityDeclScan, save);
    if (xmlSaveFinish(save) != XML_ERR_OK)
        xmlFree(xmlBufferDetach(buf));
}
#endif /* LIBXML_OUTPUT_ENABLED */
