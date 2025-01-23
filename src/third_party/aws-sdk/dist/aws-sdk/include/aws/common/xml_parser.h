#ifndef AWS_COMMON_XML_PARSER_H
#define AWS_COMMON_XML_PARSER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>

#include <aws/common/exports.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_xml_node;

struct aws_xml_attribute {
    struct aws_byte_cursor name;
    struct aws_byte_cursor value;
};

/**
 * Callback for when an xml node is encountered in the document. As a user you have a few options:
 *
 * 1. fail the parse by returning AWS_OP_ERR (after an error has been raised). This will stop any further parsing.
 * 2. call aws_xml_node_traverse() on the node to descend into the node with a new callback and user_data.
 * 3. call aws_xml_node_as_body() to retrieve the contents of the node as text.
 *
 * You MUST NOT call both aws_xml_node_traverse() and aws_xml_node_as_body() on the same node.
 *
 * return true to continue the parsing operation.
 */
typedef int(aws_xml_parser_on_node_encountered_fn)(struct aws_xml_node *node, void *user_data);

struct aws_xml_parser_options {
    /* xml document to parse. */
    struct aws_byte_cursor doc;

    /* Max node depth used for parsing document. */
    size_t max_depth;

    /* Callback invoked on the root node */
    aws_xml_parser_on_node_encountered_fn *on_root_encountered;

    /* User data for callback */
    void *user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Parse an XML document.
 * WARNING: This is not a public API. It is only intended for use within the aws-c libraries.
 */
AWS_COMMON_API
int aws_xml_parse(struct aws_allocator *allocator, const struct aws_xml_parser_options *options);

/**
 * Writes the contents of the body of node into out_body. out_body is an output parameter in this case. Upon success,
 * out_body will contain the body of the node.
 */
AWS_COMMON_API
int aws_xml_node_as_body(struct aws_xml_node *node, struct aws_byte_cursor *out_body);

/**
 * Traverse node and invoke on_node_encountered when a nested node is encountered.
 */
AWS_COMMON_API
int aws_xml_node_traverse(
    struct aws_xml_node *node,
    aws_xml_parser_on_node_encountered_fn *on_node_encountered,
    void *user_data);

/*
 * Get the name of an xml node.
 */
AWS_COMMON_API
struct aws_byte_cursor aws_xml_node_get_name(const struct aws_xml_node *node);

/*
 * Get the number of attributes for an xml node.
 */
AWS_COMMON_API
size_t aws_xml_node_get_num_attributes(const struct aws_xml_node *node);

/*
 * Get an attribute for an xml node by its index.
 */
AWS_COMMON_API
struct aws_xml_attribute aws_xml_node_get_attribute(const struct aws_xml_node *node, size_t attribute_index);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_XML_PARSER_H */
