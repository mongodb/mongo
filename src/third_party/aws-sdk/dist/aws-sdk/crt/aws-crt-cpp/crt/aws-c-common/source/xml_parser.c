/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/logging.h>
#include <aws/common/private/xml_parser_impl.h>

#ifdef _MSC_VER
/* allow non-constant declared initializers. */
#    pragma warning(disable : 4204)
#endif

static const size_t s_max_document_depth = 20;
#define MAX_NAME_LEN ((size_t)256)
#define NODE_CLOSE_OVERHEAD ((size_t)3)

struct cb_stack_data {
    aws_xml_parser_on_node_encountered_fn *cb;
    void *user_data;
};

int s_node_next_sibling(struct aws_xml_parser *parser);

static bool s_double_quote_fn(uint8_t value) {
    return value == '"';
}

/* load the node declaration line, parsing node name and attributes.
 *
 * something of the form:
 * <NodeName Attribute1=Value1 Attribute2=Value2 ...>
 * */
static int s_load_node_decl(
    struct aws_xml_parser *parser,
    struct aws_byte_cursor *decl_body,
    struct aws_xml_node *node) {
    AWS_PRECONDITION(parser);
    AWS_PRECONDITION(decl_body);
    AWS_PRECONDITION(node);

    node->is_empty = decl_body->ptr[decl_body->len - 1] == '/';

    struct aws_array_list splits;
    AWS_ZERO_STRUCT(splits);

    AWS_ZERO_ARRAY(parser->split_scratch);
    aws_array_list_init_static(
        &splits, parser->split_scratch, AWS_ARRAY_SIZE(parser->split_scratch), sizeof(struct aws_byte_cursor));

    /* split by space, first split will be the node name, everything after will be attribute=value pairs. For now
     * we limit to 10 attributes, if this is exceeded we consider it invalid document. */
    if (aws_byte_cursor_split_on_char(decl_body, ' ', &splits)) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
        return aws_raise_error(AWS_ERROR_INVALID_XML);
    }

    size_t splits_count = aws_array_list_length(&splits);

    if (splits_count < 1) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
        return aws_raise_error(AWS_ERROR_INVALID_XML);
    }

    aws_array_list_get_at(&splits, &node->name, 0);

    AWS_ZERO_ARRAY(parser->attributes);
    if (splits.length > 1) {
        aws_array_list_init_static(
            &node->attributes,
            parser->attributes,
            AWS_ARRAY_SIZE(parser->attributes),
            sizeof(struct aws_xml_attribute));

        for (size_t i = 1; i < splits.length; ++i) {
            struct aws_byte_cursor attribute_pair;
            AWS_ZERO_STRUCT(attribute_pair);
            aws_array_list_get_at(&splits, &attribute_pair, i);

            struct aws_byte_cursor att_val_pair[2];
            AWS_ZERO_ARRAY(att_val_pair);
            struct aws_array_list att_val_pair_lst;
            AWS_ZERO_STRUCT(att_val_pair_lst);
            aws_array_list_init_static(&att_val_pair_lst, att_val_pair, 2, sizeof(struct aws_byte_cursor));

            if (!aws_byte_cursor_split_on_char(&attribute_pair, '=', &att_val_pair_lst)) {
                struct aws_xml_attribute attribute = {
                    .name = att_val_pair[0],
                    .value = aws_byte_cursor_trim_pred(&att_val_pair[1], s_double_quote_fn),
                };
                aws_array_list_push_back(&node->attributes, &attribute);
            }
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_xml_parse(struct aws_allocator *allocator, const struct aws_xml_parser_options *options) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->on_root_encountered);

    struct aws_xml_parser parser = {
        .allocator = allocator,
        .doc = options->doc,
        .max_depth = options->max_depth ? options->max_depth : s_max_document_depth,
        .error = AWS_OP_SUCCESS,
    };
    aws_array_list_init_dynamic(&parser.callback_stack, allocator, 4, sizeof(struct cb_stack_data));

    /* burn everything that precedes the actual xml nodes. */
    while (parser.doc.len) {
        const uint8_t *start = memchr(parser.doc.ptr, '<', parser.doc.len);
        if (!start) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
            parser.error = aws_raise_error(AWS_ERROR_INVALID_XML);
            goto clean_up;
        }

        const uint8_t *location = memchr(parser.doc.ptr, '>', parser.doc.len);
        if (!location) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
            parser.error = aws_raise_error(AWS_ERROR_INVALID_XML);
            goto clean_up;
        }

        aws_byte_cursor_advance(&parser.doc, start - parser.doc.ptr);
        /* if these are preamble statements, burn them. otherwise don't seek at all
         * and assume it's just the doc with no preamble statements. */
        if (*(parser.doc.ptr + 1) == '?' || *(parser.doc.ptr + 1) == '!') {
            /* nobody cares about the preamble */
            size_t advance = location - parser.doc.ptr + 1;
            aws_byte_cursor_advance(&parser.doc, advance);
        } else {
            break;
        }
    }

    /* now we should be at the start of the actual document. */
    struct cb_stack_data stack_data = {
        .cb = options->on_root_encountered,
        .user_data = options->user_data,
    };

    aws_array_list_push_back(&parser.callback_stack, &stack_data);
    parser.error = s_node_next_sibling(&parser);

clean_up:
    aws_array_list_clean_up(&parser.callback_stack);
    return parser.error;
}

int s_advance_to_closing_tag(
    struct aws_xml_parser *parser,
    struct aws_xml_node *node,
    struct aws_byte_cursor *out_body) {
    AWS_PRECONDITION(parser);
    AWS_PRECONDITION(node);

    if (node->is_empty) {
        if (out_body) {
            out_body->ptr = NULL;
            out_body->len = 0;
        }
        return AWS_OP_SUCCESS;
    }

    /* currently the max node name is 256 characters. This is arbitrary, but should be enough
     * for our uses. If we ever generalize this, we'll have to come back and rethink this. */
    uint8_t name_close[MAX_NAME_LEN + NODE_CLOSE_OVERHEAD] = {0};
    uint8_t name_open[MAX_NAME_LEN + NODE_CLOSE_OVERHEAD] = {0};

    struct aws_byte_buf closing_cmp_buf = aws_byte_buf_from_empty_array(name_close, sizeof(name_close));
    struct aws_byte_buf open_cmp_buf = aws_byte_buf_from_empty_array(name_open, sizeof(name_open));

    size_t closing_name_len = node->name.len + NODE_CLOSE_OVERHEAD;

    if (closing_name_len > node->doc_at_body.len) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
        parser->error = aws_raise_error(AWS_ERROR_INVALID_XML);
        return AWS_OP_ERR;
    }

    if (sizeof(name_close) < closing_name_len) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
        parser->error = aws_raise_error(AWS_ERROR_INVALID_XML);
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor open_bracket = aws_byte_cursor_from_c_str("<");
    struct aws_byte_cursor close_token = aws_byte_cursor_from_c_str("/");
    struct aws_byte_cursor close_bracket = aws_byte_cursor_from_c_str(">");

    aws_byte_buf_append(&open_cmp_buf, &open_bracket);
    aws_byte_buf_append(&open_cmp_buf, &node->name);

    aws_byte_buf_append(&closing_cmp_buf, &open_bracket);
    aws_byte_buf_append(&closing_cmp_buf, &close_token);
    aws_byte_buf_append(&closing_cmp_buf, &node->name);
    aws_byte_buf_append(&closing_cmp_buf, &close_bracket);

    size_t depth_count = 1;
    struct aws_byte_cursor to_find_open = aws_byte_cursor_from_buf(&open_cmp_buf);
    struct aws_byte_cursor to_find_close = aws_byte_cursor_from_buf(&closing_cmp_buf);
    struct aws_byte_cursor close_find_result;
    AWS_ZERO_STRUCT(close_find_result);
    do {
        if (aws_byte_cursor_find_exact(&parser->doc, &to_find_close, &close_find_result)) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
            return aws_raise_error(AWS_ERROR_INVALID_XML);
        }

        /* if we find an opening node with the same name, before the closing tag keep going. */
        struct aws_byte_cursor open_find_result;
        AWS_ZERO_STRUCT(open_find_result);

        while (parser->doc.len) {
            if (!aws_byte_cursor_find_exact(&parser->doc, &to_find_open, &open_find_result)) {
                if (open_find_result.ptr < close_find_result.ptr) {
                    size_t skip_len = open_find_result.ptr - parser->doc.ptr;
                    aws_byte_cursor_advance(&parser->doc, skip_len + 1);
                    depth_count++;
                    continue;
                }
            }
            size_t skip_len = close_find_result.ptr - parser->doc.ptr;
            aws_byte_cursor_advance(&parser->doc, skip_len + closing_cmp_buf.len);
            depth_count--;
            break;
        }
    } while (depth_count > 0);

    size_t len = close_find_result.ptr - node->doc_at_body.ptr;

    if (out_body) {
        *out_body = aws_byte_cursor_from_array(node->doc_at_body.ptr, len);
    }

    return parser->error;
}

int aws_xml_node_as_body(struct aws_xml_node *node, struct aws_byte_cursor *out_body) {
    AWS_PRECONDITION(node);

    AWS_FATAL_ASSERT(!node->processed && "XML node can be traversed, or read as body, but not both.");
    node->processed = true;
    return s_advance_to_closing_tag(node->parser, node, out_body);
}

int aws_xml_node_traverse(
    struct aws_xml_node *node,
    aws_xml_parser_on_node_encountered_fn *on_node_encountered,
    void *user_data) {
    AWS_PRECONDITION(node);
    AWS_PRECONDITION(on_node_encountered);

    struct aws_xml_parser *parser = node->parser;

    AWS_FATAL_ASSERT(!node->processed && "XML node can be traversed, or read as body, but not both.");
    node->processed = true;
    struct cb_stack_data stack_data = {
        .cb = on_node_encountered,
        .user_data = user_data,
    };

    size_t doc_depth = aws_array_list_length(&parser->callback_stack);
    if (doc_depth >= parser->max_depth) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document exceeds max depth.");
        aws_raise_error(AWS_ERROR_INVALID_XML);
        goto error;
    }

    aws_array_list_push_back(&parser->callback_stack, &stack_data);

    /* look for the next node at the current level. do this until we encounter the parent node's
     * closing tag. */
    while (!parser->error) {
        const uint8_t *next_location = memchr(parser->doc.ptr, '<', parser->doc.len);

        if (!next_location) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
            aws_raise_error(AWS_ERROR_INVALID_XML);
            goto error;
        }

        const uint8_t *end_location = memchr(parser->doc.ptr, '>', parser->doc.len);

        if (!end_location) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
            aws_raise_error(AWS_ERROR_INVALID_XML);
            goto error;
        }

        bool parent_closed = false;

        if (*(next_location + 1) == '/') {
            parent_closed = true;
        }

        size_t node_name_len = end_location - next_location;

        aws_byte_cursor_advance(&parser->doc, end_location - parser->doc.ptr + 1);

        if (parent_closed) {
            break;
        }

        struct aws_byte_cursor decl_body = aws_byte_cursor_from_array(next_location + 1, node_name_len - 1);

        struct aws_xml_node next_node = {
            .parser = parser,
            .doc_at_body = parser->doc,
            .processed = false,
        };

        if (s_load_node_decl(parser, &decl_body, &next_node)) {
            return AWS_OP_ERR;
        }

        if (on_node_encountered(&next_node, user_data)) {
            goto error;
        }

        /* if the user simply returned while skipping the node altogether, go ahead and do the skip over. */
        if (!next_node.processed) {
            if (s_advance_to_closing_tag(parser, &next_node, NULL)) {
                goto error;
            }
        }
    }

    aws_array_list_pop_back(&parser->callback_stack);
    return parser->error;

error:
    parser->error = AWS_OP_ERR;
    return parser->error;
}

struct aws_byte_cursor aws_xml_node_get_name(const struct aws_xml_node *node) {
    AWS_PRECONDITION(node);
    return node->name;
}

size_t aws_xml_node_get_num_attributes(const struct aws_xml_node *node) {
    AWS_PRECONDITION(node);
    return aws_array_list_length(&node->attributes);
}

struct aws_xml_attribute aws_xml_node_get_attribute(const struct aws_xml_node *node, size_t attribute_index) {
    AWS_PRECONDITION(node);

    struct aws_xml_attribute attribute;
    if (aws_array_list_get_at(&node->attributes, &attribute, attribute_index)) {
        AWS_FATAL_ASSERT(0 && "Invalid XML attribute index");
    }

    return attribute;
}

/* advance the parser to the next sibling node.*/
int s_node_next_sibling(struct aws_xml_parser *parser) {
    AWS_PRECONDITION(parser);

    const uint8_t *next_location = memchr(parser->doc.ptr, '<', parser->doc.len);

    if (!next_location) {
        return parser->error;
    }

    aws_byte_cursor_advance(&parser->doc, next_location - parser->doc.ptr);
    const uint8_t *end_location = memchr(parser->doc.ptr, '>', parser->doc.len);

    if (!end_location) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_XML_PARSER, "XML document is invalid.");
        return aws_raise_error(AWS_ERROR_INVALID_XML);
    }

    size_t node_name_len = end_location - next_location;
    aws_byte_cursor_advance(&parser->doc, end_location - parser->doc.ptr + 1);

    struct aws_byte_cursor node_decl_body = aws_byte_cursor_from_array(next_location + 1, node_name_len - 1);

    struct aws_xml_node sibling_node = {
        .parser = parser,
        .doc_at_body = parser->doc,
        .processed = false,
    };

    if (s_load_node_decl(parser, &node_decl_body, &sibling_node)) {
        return AWS_OP_ERR;
    }

    struct cb_stack_data stack_data;
    AWS_ZERO_STRUCT(stack_data);
    aws_array_list_back(&parser->callback_stack, &stack_data);
    AWS_FATAL_ASSERT(stack_data.cb);

    if (stack_data.cb(&sibling_node, stack_data.user_data)) {
        return AWS_OP_ERR;
    }

    /* if the user simply returned while skipping the node altogether, go ahead and do the skip over. */
    if (!sibling_node.processed) {
        if (s_advance_to_closing_tag(parser, &sibling_node, NULL)) {
            return AWS_OP_ERR;
        }
    }

    return parser->error;
}
