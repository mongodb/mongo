#include "external/libcbor/cbor.h"
#include <aws/common/cbor.h>

#include <aws/common/array_list.h>
#include <aws/common/logging.h>
#include <aws/common/private/byte_buf.h>
#include <aws/common/private/external_module_impl.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>

static bool s_aws_cbor_module_initialized = false;

const static size_t s_cbor_element_width_64bit = 9;
const static size_t s_cbor_element_width_32bit = 5;

enum s_cbor_simple_val {
    AWS_CBOR_SIMPLE_VAL_FALSE = 20,
    AWS_CBOR_SIMPLE_VAL_TRUE = 21,
    AWS_CBOR_SIMPLE_VAL_NULL = 22,
    AWS_CBOR_SIMPLE_VAL_UNDEFINED = 23,
    AWS_CBOR_SIMPLE_VAL_BREAK = 31,
};

void aws_cbor_module_init(struct aws_allocator *allocator) {
    (void)allocator;
    if (!s_aws_cbor_module_initialized) {
        /* Not allow any allocation from libcbor */
        cbor_set_allocs(NULL, NULL, NULL);
        s_aws_cbor_module_initialized = true;
    }
}

void aws_cbor_module_cleanup(void) {
    if (s_aws_cbor_module_initialized) {
        s_aws_cbor_module_initialized = false;
    }
}

/* Return c-string for aws_cbor_type */
const char *aws_cbor_type_cstr(enum aws_cbor_type type) {
    /* clang-format off */
    switch (type) {
        case (AWS_CBOR_TYPE_UINT): return "AWS_CBOR_TYPE_UINT";
        case (AWS_CBOR_TYPE_NEGINT): return "AWS_CBOR_TYPE_NEGINT";
        case (AWS_CBOR_TYPE_FLOAT): return "AWS_CBOR_TYPE_FLOAT";
        case (AWS_CBOR_TYPE_BYTES): return "AWS_CBOR_TYPE_BYTES";
        case (AWS_CBOR_TYPE_TEXT): return "AWS_CBOR_TYPE_TEXT";
        case (AWS_CBOR_TYPE_ARRAY_START): return "AWS_CBOR_TYPE_ARRAY_START";
        case (AWS_CBOR_TYPE_MAP_START): return "AWS_CBOR_TYPE_MAP_START";
        case (AWS_CBOR_TYPE_TAG): return "AWS_CBOR_TYPE_TAG";
        case (AWS_CBOR_TYPE_BOOL): return "AWS_CBOR_TYPE_BOOL";
        case (AWS_CBOR_TYPE_NULL): return "AWS_CBOR_TYPE_NULL";
        case (AWS_CBOR_TYPE_UNDEFINED): return "AWS_CBOR_TYPE_UNDEFINED";
        case (AWS_CBOR_TYPE_BREAK): return "AWS_CBOR_TYPE_BREAK";
        case (AWS_CBOR_TYPE_INDEF_BYTES_START): return "AWS_CBOR_TYPE_INDEF_BYTES_START";
        case (AWS_CBOR_TYPE_INDEF_TEXT_START): return "AWS_CBOR_TYPE_INDEF_TEXT_START";
        case (AWS_CBOR_TYPE_INDEF_ARRAY_START): return "AWS_CBOR_TYPE_INDEF_ARRAY_START";
        case (AWS_CBOR_TYPE_INDEF_MAP_START): return "AWS_CBOR_TYPE_INDEF_MAP_START";
        default: return "<UNKNOWN TYPE>";
    }
    /* clang-format on */
}

/*******************************************************************************
 * ENCODE
 ******************************************************************************/

struct aws_cbor_encoder {
    struct aws_allocator *allocator;
    struct aws_byte_buf encoded_buf;
};

struct aws_cbor_encoder *aws_cbor_encoder_new(struct aws_allocator *allocator) {
    struct aws_cbor_encoder *encoder = aws_mem_calloc(allocator, 1, sizeof(struct aws_cbor_encoder));
    encoder->allocator = allocator;
    aws_byte_buf_init(&encoder->encoded_buf, allocator, 256);

    return encoder;
}

struct aws_cbor_encoder *aws_cbor_encoder_destroy(struct aws_cbor_encoder *encoder) {
    aws_byte_buf_clean_up(&encoder->encoded_buf);
    aws_mem_release(encoder->allocator, encoder);
    return NULL;
}

struct aws_byte_cursor aws_cbor_encoder_get_encoded_data(const struct aws_cbor_encoder *encoder) {
    return aws_byte_cursor_from_buf(&encoder->encoded_buf);
}

void aws_cbor_encoder_reset(struct aws_cbor_encoder *encoder) {
    aws_byte_buf_reset(&encoder->encoded_buf, false);
}

static uint8_t *s_get_encoder_current_position(struct aws_cbor_encoder *encoder) {
    return encoder->encoded_buf.buffer + encoder->encoded_buf.len;
}

static size_t s_get_encoder_remaining_len(struct aws_cbor_encoder *encoder) {
    return encoder->encoded_buf.capacity - encoder->encoded_buf.len;
}

/**
 * @brief Marcos to ensure the encoder have enough space to encode the value into the buffer using given `fn`, and then
 * encode it.
 */
#define ENCODE_THROUGH_LIBCBOR(encoder, length_to_reserve, value, fn)                                                  \
    do {                                                                                                               \
        int error = aws_byte_buf_reserve_smart_relative(&(encoder)->encoded_buf, length_to_reserve);                   \
        (void)error;                                                                                                   \
        AWS_FATAL_ASSERT(error == AWS_ERROR_SUCCESS);                                                                  \
        size_t encoded_len = fn(value, s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder)); \
        AWS_FATAL_ASSERT((encoded_len) != 0);                                                                          \
        (encoder)->encoded_buf.len += (encoded_len);                                                                   \
    } while (false)

void aws_cbor_encoder_write_uint(struct aws_cbor_encoder *encoder, uint64_t value) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, value, cbor_encode_uint);
}

void aws_cbor_encoder_write_negint(struct aws_cbor_encoder *encoder, uint64_t value) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, value, cbor_encode_negint);
}

void aws_cbor_encoder_write_single_float(struct aws_cbor_encoder *encoder, float value) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_32bit, value, cbor_encode_single);
}

void aws_cbor_encoder_write_float(struct aws_cbor_encoder *encoder, double value) {
    /**
     * As suggested by AWS SDK SEP, write the float value as small as possible. But, do not encode to half-float.
     * Convert the float value to integer if the conversion will not cause any precision loss.
     */
    if (!isfinite(value)) {
        /* For special value: NAN/INFINITY, type cast to float and encode into single float. */
        aws_cbor_encoder_write_single_float(encoder, (float)value);
        return;
    }
    /* Conversation from int to floating-type is implementation defined if loss of precision */
    if (value <= (double)INT64_MAX && value >= (double)INT64_MIN) {
        /**
         * A prvalue of a floating point type can be converted to a prvalue of an integer type. The conversion
         * truncates; that is, the fractional part is discarded. The behavior is undefined if the truncated value cannot
         * be represented in the destination type.
         * Check against the INT64 range to avoid undefined behavior
         *
         * Comparing against INT64_MAX instead of UINT64_MAX to simplify the code, which may loss the opportunity to
         * convert the UINT64 range from double to uint64_t. However, converting double to uint64_t will not benefit the
         * total length encoded.
         **/
        int64_t int_value = (int64_t)value;
        if (value == (double)int_value) {
            if (int_value < 0) {
                aws_cbor_encoder_write_negint(encoder, (uint64_t)(-1 - int_value));
            } else {
                aws_cbor_encoder_write_uint(encoder, (uint64_t)(int_value));
            }
            return;
        }
    }
    if (value <= FLT_MAX && value >= -FLT_MAX) {
        /* Only try to convert the value within the range of float. */
        float float_value = (float)value;
        double converted_value = (double)float_value;
        /* Try to cast a round trip to detect any precision loss. */
        if (value == converted_value) {
            aws_cbor_encoder_write_single_float(encoder, float_value);
            return;
        }
    }

    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, value, cbor_encode_double);
}

void aws_cbor_encoder_write_map_start(struct aws_cbor_encoder *encoder, size_t number_entries) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, number_entries, cbor_encode_map_start);
}

void aws_cbor_encoder_write_tag(struct aws_cbor_encoder *encoder, uint64_t tag_number) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, tag_number, cbor_encode_tag);
}

void aws_cbor_encoder_write_array_start(struct aws_cbor_encoder *encoder, size_t number_entries) {
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit, number_entries, cbor_encode_array_start);
}

void aws_cbor_encoder_write_bytes(struct aws_cbor_encoder *encoder, struct aws_byte_cursor from) {
    /* Reserve the bytes for the byte string start cbor item and the actual bytes */
    /* Encode the first cbor item for byte string */
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit + from.len, from.len, cbor_encode_bytestring_start);
    /* Append the actual bytes to follow the cbor item */
    aws_byte_buf_append(&encoder->encoded_buf, &from);
}

void aws_cbor_encoder_write_text(struct aws_cbor_encoder *encoder, struct aws_byte_cursor from) {
    /* Reserve the bytes for the byte string start cbor item and the actual string */
    /* Encode the first cbor item for byte string */
    ENCODE_THROUGH_LIBCBOR(encoder, s_cbor_element_width_64bit + from.len, from.len, cbor_encode_string_start);
    /* Append the actual string to follow the cbor item */
    aws_byte_buf_append(&encoder->encoded_buf, &from);
}

void aws_cbor_encoder_write_bool(struct aws_cbor_encoder *encoder, bool value) {
    /* Major type 7 (simple), value 20 (false) and 21 (true) */
    uint8_t ctrl_value = value == true ? AWS_CBOR_SIMPLE_VAL_TRUE : AWS_CBOR_SIMPLE_VAL_FALSE;
    ENCODE_THROUGH_LIBCBOR(encoder, 1, ctrl_value, cbor_encode_ctrl);
}

void aws_cbor_encoder_write_null(struct aws_cbor_encoder *encoder) {
    /* Major type 7 (simple), value 22 (null) */
    ENCODE_THROUGH_LIBCBOR(encoder, 1, AWS_CBOR_SIMPLE_VAL_NULL /*null*/, cbor_encode_ctrl);
}

void aws_cbor_encoder_write_undefined(struct aws_cbor_encoder *encoder) {
    /* Major type 7 (simple), value 23 (undefined) */
    ENCODE_THROUGH_LIBCBOR(encoder, 1, AWS_CBOR_SIMPLE_VAL_UNDEFINED /*undefined*/, cbor_encode_ctrl);
}

static void s_cbor_encoder_write_type_only(struct aws_cbor_encoder *encoder, enum aws_cbor_type type) {
    /* All inf start takes 1 byte only */
    aws_byte_buf_reserve_smart_relative(&encoder->encoded_buf, 1);
    size_t encoded_len = 0;
    switch (type) {
        case AWS_CBOR_TYPE_INDEF_BYTES_START:
            encoded_len = cbor_encode_indef_bytestring_start(
                s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder));
            break;
        case AWS_CBOR_TYPE_INDEF_TEXT_START:
            encoded_len = cbor_encode_indef_string_start(
                s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder));
            break;
        case AWS_CBOR_TYPE_INDEF_ARRAY_START:
            encoded_len = cbor_encode_indef_array_start(
                s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder));
            break;
        case AWS_CBOR_TYPE_INDEF_MAP_START:
            encoded_len = cbor_encode_indef_map_start(
                s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder));
            break;
        case AWS_CBOR_TYPE_BREAK:
            encoded_len =
                cbor_encode_break(s_get_encoder_current_position(encoder), s_get_encoder_remaining_len(encoder));
            break;

        default:
            AWS_ASSERT(false);
            break;
    }
    AWS_ASSERT(encoded_len == 1);
    encoder->encoded_buf.len += encoded_len;
}
void aws_cbor_encoder_write_indef_bytes_start(struct aws_cbor_encoder *encoder) {
    s_cbor_encoder_write_type_only(encoder, AWS_CBOR_TYPE_INDEF_BYTES_START);
}

void aws_cbor_encoder_write_indef_text_start(struct aws_cbor_encoder *encoder) {
    s_cbor_encoder_write_type_only(encoder, AWS_CBOR_TYPE_INDEF_TEXT_START);
}

void aws_cbor_encoder_write_indef_array_start(struct aws_cbor_encoder *encoder) {
    s_cbor_encoder_write_type_only(encoder, AWS_CBOR_TYPE_INDEF_ARRAY_START);
}

void aws_cbor_encoder_write_indef_map_start(struct aws_cbor_encoder *encoder) {
    s_cbor_encoder_write_type_only(encoder, AWS_CBOR_TYPE_INDEF_MAP_START);
}

void aws_cbor_encoder_write_break(struct aws_cbor_encoder *encoder) {
    s_cbor_encoder_write_type_only(encoder, AWS_CBOR_TYPE_BREAK);
}

/*******************************************************************************
 * DECODE
 ******************************************************************************/

struct aws_cbor_decoder_context {
    enum aws_cbor_type type;

    /* All the values only valid when the type is set to corresponding type. */
    union {
        uint64_t unsigned_int_val;
        uint64_t negative_int_val;
        double float_val;
        uint64_t tag_val;
        bool boolean_val;
        struct aws_byte_cursor bytes_val;
        struct aws_byte_cursor text_val;
        uint64_t map_start;
        uint64_t array_start;
    } u;
};

struct aws_cbor_decoder {
    struct aws_allocator *allocator;

    struct aws_byte_cursor src;

    struct aws_cbor_decoder_context cached_context;

    /* Error code during decoding. Fail the decoding process without recovering, */
    int error_code;
};

struct aws_cbor_decoder *aws_cbor_decoder_new(struct aws_allocator *allocator, struct aws_byte_cursor src) {

    struct aws_cbor_decoder *decoder = aws_mem_calloc(allocator, 1, sizeof(struct aws_cbor_decoder));
    decoder->allocator = allocator;
    decoder->src = src;
    decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
    return decoder;
}

struct aws_cbor_decoder *aws_cbor_decoder_destroy(struct aws_cbor_decoder *decoder) {
    aws_mem_release(decoder->allocator, decoder);
    return NULL;
}

size_t aws_cbor_decoder_get_remaining_length(const struct aws_cbor_decoder *decoder) {
    return decoder->src.len;
}

#define LIBCBOR_VALUE_CALLBACK(field, callback_type, cbor_type)                                                        \
    static void s_##field##_callback(void *ctx, callback_type val) {                                                   \
        struct aws_cbor_decoder *decoder = ctx;                                                                        \
        AWS_ASSERT((decoder)->cached_context.type == AWS_CBOR_TYPE_UNKNOWN);                                           \
        (decoder)->cached_context.u.field = val;                                                                       \
        (decoder)->cached_context.type = cbor_type;                                                                    \
    }

LIBCBOR_VALUE_CALLBACK(unsigned_int_val, uint64_t, AWS_CBOR_TYPE_UINT)
LIBCBOR_VALUE_CALLBACK(negative_int_val, uint64_t, AWS_CBOR_TYPE_NEGINT)
LIBCBOR_VALUE_CALLBACK(boolean_val, bool, AWS_CBOR_TYPE_BOOL)
LIBCBOR_VALUE_CALLBACK(float_val, double, AWS_CBOR_TYPE_FLOAT)
LIBCBOR_VALUE_CALLBACK(map_start, uint64_t, AWS_CBOR_TYPE_MAP_START)
LIBCBOR_VALUE_CALLBACK(array_start, uint64_t, AWS_CBOR_TYPE_ARRAY_START)
LIBCBOR_VALUE_CALLBACK(tag_val, uint64_t, AWS_CBOR_TYPE_TAG)

static void s_uint8_callback(void *ctx, uint8_t data) {
    s_unsigned_int_val_callback(ctx, (uint64_t)data);
}

static void s_uint16_callback(void *ctx, uint16_t data) {
    s_unsigned_int_val_callback(ctx, (uint64_t)data);
}

static void s_uint32_callback(void *ctx, uint32_t data) {
    s_unsigned_int_val_callback(ctx, (uint64_t)data);
}

static void s_negint8_callback(void *ctx, uint8_t data) {
    s_negative_int_val_callback(ctx, (uint64_t)data);
}

static void s_negint16_callback(void *ctx, uint16_t data) {
    s_negative_int_val_callback(ctx, (uint64_t)data);
}

static void s_negint32_callback(void *ctx, uint32_t data) {
    s_negative_int_val_callback(ctx, (uint64_t)data);
}

static void s_float_callback(void *ctx, float data) {
    s_float_val_callback(ctx, (double)data);
}

static void s_bytes_callback(void *ctx, const unsigned char *cbor_data, uint64_t length) {
    struct aws_cbor_decoder *decoder = ctx;
    AWS_ASSERT((decoder)->cached_context.type == AWS_CBOR_TYPE_UNKNOWN);
    if (length > SIZE_MAX) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_CBOR, "Decoded a bytes with %" PRIu64 " bytes causing overflow .", length);
        decoder->error_code = AWS_ERROR_OVERFLOW_DETECTED;
        return;
    }
    decoder->cached_context.type = AWS_CBOR_TYPE_BYTES;
    decoder->cached_context.u.bytes_val.ptr = (uint8_t *)cbor_data;
    decoder->cached_context.u.bytes_val.len = (size_t)length;
}

static void s_str_callback(void *ctx, const unsigned char *cbor_data, uint64_t length) {
    struct aws_cbor_decoder *decoder = ctx;
    AWS_ASSERT((decoder)->cached_context.type == AWS_CBOR_TYPE_UNKNOWN);
    if (length > SIZE_MAX) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_CBOR, "Decoded a string with %" PRIu64 " bytes causing overflow .", length);
        decoder->error_code = AWS_ERROR_OVERFLOW_DETECTED;
        return;
    }
    decoder->cached_context.type = AWS_CBOR_TYPE_TEXT;
    decoder->cached_context.u.text_val.ptr = (uint8_t *)cbor_data;
    decoder->cached_context.u.text_val.len = (size_t)length;
}

#define LIBCBOR_SIMPLE_CALLBACK(field, cbor_type)                                                                      \
    static void s_##field##_callback(void *ctx) {                                                                      \
        struct aws_cbor_decoder *decoder = ctx;                                                                        \
        AWS_ASSERT((decoder)->cached_context.type == AWS_CBOR_TYPE_UNKNOWN);                                           \
        (decoder)->cached_context.type = cbor_type;                                                                    \
    }

LIBCBOR_SIMPLE_CALLBACK(inf_bytes, AWS_CBOR_TYPE_INDEF_BYTES_START)
LIBCBOR_SIMPLE_CALLBACK(inf_str, AWS_CBOR_TYPE_INDEF_TEXT_START)
LIBCBOR_SIMPLE_CALLBACK(inf_array, AWS_CBOR_TYPE_INDEF_ARRAY_START)
LIBCBOR_SIMPLE_CALLBACK(inf_map, AWS_CBOR_TYPE_INDEF_MAP_START)

LIBCBOR_SIMPLE_CALLBACK(inf_break, AWS_CBOR_TYPE_BREAK)
LIBCBOR_SIMPLE_CALLBACK(undefined, AWS_CBOR_TYPE_UNDEFINED)
LIBCBOR_SIMPLE_CALLBACK(null, AWS_CBOR_TYPE_NULL)

static struct cbor_callbacks s_callbacks = {
    /** Unsigned int */
    .uint64 = s_unsigned_int_val_callback,
    /** Unsigned int */
    .uint32 = s_uint32_callback,
    /** Unsigned int */
    .uint16 = s_uint16_callback,
    /** Unsigned int */
    .uint8 = s_uint8_callback,

    /** Negative int */
    .negint64 = s_negative_int_val_callback,
    /** Negative int */
    .negint32 = s_negint32_callback,
    /** Negative int */
    .negint16 = s_negint16_callback,
    /** Negative int */
    .negint8 = s_negint8_callback,

    /** Indefinite byte string start */
    .byte_string_start = s_inf_bytes_callback,
    /** Definite byte string */
    .byte_string = s_bytes_callback,

    /** Definite string */
    .string = s_str_callback,
    /** Indefinite string start */
    .string_start = s_inf_str_callback,

    /** Definite array */
    .indef_array_start = s_inf_array_callback,
    /** Indefinite array */
    .array_start = s_array_start_callback,

    /** Definite map */
    .indef_map_start = s_inf_map_callback,
    /** Indefinite map */
    .map_start = s_map_start_callback,

    /** Tags */
    .tag = s_tag_val_callback,

    /** Half float */
    .float2 = s_float_callback,
    /** Single float */
    .float4 = s_float_callback,
    /** Double float */
    .float8 = s_float_val_callback,
    /** Undef */
    .undefined = s_undefined_callback,
    /** Null */
    .null = s_null_callback,
    /** Bool */
    .boolean = s_boolean_val_callback,

    /** Indefinite item break */
    .indef_break = s_inf_break_callback,
};

/**
 * decode the next element to the cached_content.
 */
static int s_cbor_decode_next_element(struct aws_cbor_decoder *decoder) {
    struct cbor_decoder_result result = cbor_stream_decode(decoder->src.ptr, decoder->src.len, &s_callbacks, decoder);
    switch (result.status) {
        case CBOR_DECODER_NEDATA:
            AWS_LOGF_ERROR(
                AWS_LS_COMMON_CBOR,
                "The decoder doesn't have enough data to decode the next element. At least %zu bytes more needed.",
                result.required);
            decoder->error_code = AWS_ERROR_INVALID_CBOR;
            break;
        case CBOR_DECODER_ERROR:
            AWS_LOGF_ERROR(AWS_LS_COMMON_CBOR, "The cbor data is malformed to decode.");
            decoder->error_code = AWS_ERROR_INVALID_CBOR;
            break;
        default:
            break;
    }

    if (decoder->error_code) {
        /* Error happened during decoding */
        return aws_raise_error(decoder->error_code);
    }

    aws_byte_cursor_advance(&decoder->src, result.read);

    return AWS_OP_SUCCESS;
}

#define GET_NEXT_ITEM(field, out_type, expected_cbor_type)                                                             \
    /* NOLINTNEXTLINE(bugprone-macro-parentheses) */                                                                   \
    int aws_cbor_decoder_pop_next_##field(struct aws_cbor_decoder *decoder, out_type *out) {                           \
        if ((decoder)->error_code) {                                                                                   \
            /* Error happened during decoding */                                                                       \
            return aws_raise_error((decoder)->error_code);                                                             \
        }                                                                                                              \
        if ((decoder)->cached_context.type != AWS_CBOR_TYPE_UNKNOWN) {                                                 \
            /* There was a cached context, check if the cached one meets the expected. */                              \
            goto decode_done;                                                                                          \
        }                                                                                                              \
        if (s_cbor_decode_next_element(decoder)) {                                                                     \
            return AWS_OP_ERR;                                                                                         \
        }                                                                                                              \
    decode_done:                                                                                                       \
        if ((decoder)->cached_context.type != (expected_cbor_type)) {                                                  \
            AWS_LOGF_ERROR(                                                                                            \
                AWS_LS_COMMON_CBOR,                                                                                    \
                "The decoder got unexpected type: %d (%s), while expecting type: %d (%s).",                            \
                (decoder)->cached_context.type,                                                                        \
                aws_cbor_type_cstr((decoder)->cached_context.type),                                                    \
                (expected_cbor_type),                                                                                  \
                aws_cbor_type_cstr(expected_cbor_type));                                                               \
            return aws_raise_error(AWS_ERROR_CBOR_UNEXPECTED_TYPE);                                                    \
        } else {                                                                                                       \
            /* Clear the cache as we give it out. */                                                                   \
            (decoder)->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;                                                    \
            *out = (decoder)->cached_context.u.field;                                                                  \
        }                                                                                                              \
        return AWS_OP_SUCCESS;                                                                                         \
    }

GET_NEXT_ITEM(unsigned_int_val, uint64_t, AWS_CBOR_TYPE_UINT)
GET_NEXT_ITEM(negative_int_val, uint64_t, AWS_CBOR_TYPE_NEGINT)
GET_NEXT_ITEM(float_val, double, AWS_CBOR_TYPE_FLOAT)
GET_NEXT_ITEM(boolean_val, bool, AWS_CBOR_TYPE_BOOL)
GET_NEXT_ITEM(text_val, struct aws_byte_cursor, AWS_CBOR_TYPE_TEXT)
GET_NEXT_ITEM(bytes_val, struct aws_byte_cursor, AWS_CBOR_TYPE_BYTES)
GET_NEXT_ITEM(map_start, uint64_t, AWS_CBOR_TYPE_MAP_START)
GET_NEXT_ITEM(array_start, uint64_t, AWS_CBOR_TYPE_ARRAY_START)
GET_NEXT_ITEM(tag_val, uint64_t, AWS_CBOR_TYPE_TAG)

int aws_cbor_decoder_peek_type(struct aws_cbor_decoder *decoder, enum aws_cbor_type *out_type) {
    if (decoder->error_code) {
        /* Error happened during decoding */
        return aws_raise_error(decoder->error_code);
    }

    if (decoder->cached_context.type != AWS_CBOR_TYPE_UNKNOWN) {
        /* There was a cached context, return the type. */
        *out_type = decoder->cached_context.type;
        return AWS_OP_SUCCESS;
    }

    /* Decode */
    if (s_cbor_decode_next_element(decoder)) {
        return AWS_OP_ERR;
    }
    *out_type = decoder->cached_context.type;
    return AWS_OP_SUCCESS;
}

int aws_cbor_decoder_consume_next_whole_data_item(struct aws_cbor_decoder *decoder) {
    if (decoder->error_code) {
        /* Error happened during decoding */
        return aws_raise_error(decoder->error_code);
    }

    if (decoder->cached_context.type == AWS_CBOR_TYPE_UNKNOWN) {
        /* There was no cache, decode the next item */
        if (s_cbor_decode_next_element(decoder)) {
            return AWS_OP_ERR;
        }
    }
    switch (decoder->cached_context.type) {
        case AWS_CBOR_TYPE_TAG:
            /* Read the next data item */
            decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
            if (aws_cbor_decoder_consume_next_whole_data_item(decoder)) {
                return AWS_OP_ERR;
            }
            break;
        case AWS_CBOR_TYPE_MAP_START: {
            uint64_t num_map_item = decoder->cached_context.u.map_start;
            /* Reset type */
            decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
            for (uint64_t i = 0; i < num_map_item; i++) {
                /* Key */
                if (aws_cbor_decoder_consume_next_whole_data_item(decoder)) {
                    return AWS_OP_ERR;
                }
                /* Value */
                if (aws_cbor_decoder_consume_next_whole_data_item(decoder)) {
                    return AWS_OP_ERR;
                }
            }
            break;
        }
        case AWS_CBOR_TYPE_ARRAY_START: {
            uint64_t num_array_item = decoder->cached_context.u.array_start;
            /* Reset type */
            decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
            for (uint64_t i = 0; i < num_array_item; i++) {
                /* item */
                if (aws_cbor_decoder_consume_next_whole_data_item(decoder)) {
                    return AWS_OP_ERR;
                }
            }
            break;
        }
        case AWS_CBOR_TYPE_INDEF_BYTES_START:
        case AWS_CBOR_TYPE_INDEF_TEXT_START:
        case AWS_CBOR_TYPE_INDEF_ARRAY_START:
        case AWS_CBOR_TYPE_INDEF_MAP_START: {
            enum aws_cbor_type next_type;
            /* Reset the cache for the tag val */
            decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
            if (aws_cbor_decoder_peek_type(decoder, &next_type)) {
                return AWS_OP_ERR;
            }
            while (next_type != AWS_CBOR_TYPE_BREAK) {
                if (aws_cbor_decoder_consume_next_whole_data_item(decoder)) {
                    return AWS_OP_ERR;
                }
                if (aws_cbor_decoder_peek_type(decoder, &next_type)) {
                    return AWS_OP_ERR;
                }
            }
            break;
        }

        default:
            break;
    }

    /* Done, just reset the cache */
    decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
    return AWS_OP_SUCCESS;
}

int aws_cbor_decoder_consume_next_single_element(struct aws_cbor_decoder *decoder) {
    enum aws_cbor_type out_type = 0;
    if (aws_cbor_decoder_peek_type(decoder, &out_type)) {
        return AWS_OP_ERR;
    }
    /* Reset the type to clear the cache. */
    decoder->cached_context.type = AWS_CBOR_TYPE_UNKNOWN;
    return AWS_OP_SUCCESS;
}
