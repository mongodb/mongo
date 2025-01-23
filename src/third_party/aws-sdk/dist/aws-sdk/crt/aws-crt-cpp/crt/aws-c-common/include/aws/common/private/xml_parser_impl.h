#ifndef AWS_COMMON_PRIVATE_XML_PARSER_IMPL_H
#define AWS_COMMON_PRIVATE_XML_PARSER_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/xml_parser.h>

struct aws_xml_node {
    struct aws_xml_parser *parser;
    struct aws_byte_cursor name;
    struct aws_array_list attributes;
    struct aws_byte_cursor doc_at_body;
    bool processed;
    bool is_empty;
};

struct aws_xml_parser {
    struct aws_allocator *allocator;
    struct aws_byte_cursor doc;
    struct aws_array_list callback_stack;
    /* maximum of 10 attributes */
    struct aws_xml_attribute attributes[10];
    /* splits on attributes and node name, so (10 attributes + 1 name) */
    struct aws_byte_cursor split_scratch[11];
    size_t max_depth;
    int error;
};

#endif /* AWS_COMMON_PRIVATE_XML_PARSER_IMPL_H */
