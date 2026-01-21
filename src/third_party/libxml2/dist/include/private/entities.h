#ifndef XML_ENTITIES_H_PRIVATE__
#define XML_ENTITIES_H_PRIVATE__

#include <libxml/tree.h>
#include <libxml/xmlstring.h>

/*
 * Entity flags
 *
 * XML_ENT_PARSED: The entity was parsed and `children` points to the
 * content.
 *
 * XML_ENT_CHECKED: The entity was checked for loops and amplification.
 * expandedSize was set.
 *
 * XML_ENT_VALIDATED: The entity contains a valid attribute value.
 * Only used when entities aren't substituted.
 */
#define XML_ENT_PARSED      (1u << 0)
#define XML_ENT_CHECKED     (1u << 1)
#define XML_ENT_VALIDATED   (1u << 2)
#define XML_ENT_EXPANDING   (1u << 3)

#endif /* XML_ENTITIES_H_PRIVATE__ */
