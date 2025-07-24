/* pb_decode.c -- decode a protobuf using minimal resources
 *
 * 2011 Petteri Aimonen <jpa@kapsi.fi>
 */

/* Use the GCC warn_unused_result attribute to check that all return values
 * are propagated correctly. On other compilers and gcc before 3.4.0 just
 * ignore the annotation.
 */
#if !defined(__GNUC__) || ( __GNUC__ < 3) || (__GNUC__ == 3 && __GNUC_MINOR__ < 4)
    #define checkreturn
#else
    #define checkreturn __attribute__((warn_unused_result))
#endif

#include "nanopb/pb.h"
#include "nanopb/pb_decode.h"
#include "nanopb/pb_common.h"

/**************************************
 * Declarations internal to this file *
 **************************************/

static bool checkreturn buf_read(pb_istream_t *stream, pb_byte_t *buf, size_t count);
static bool checkreturn pb_decode_varint32_eof(pb_istream_t *stream, uint32_t *dest, bool *eof);
static bool checkreturn read_raw_value(pb_istream_t *stream, pb_wire_type_t wire_type, pb_byte_t *buf, size_t *size);
static bool checkreturn decode_basic_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field);
static bool checkreturn decode_static_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field);
static bool checkreturn decode_pointer_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field);
static bool checkreturn decode_callback_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field);
static bool checkreturn decode_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field);
static bool checkreturn default_extension_decoder(pb_istream_t *stream, pb_extension_t *extension, uint32_t tag, pb_wire_type_t wire_type);
static bool checkreturn decode_extension(pb_istream_t *stream, uint32_t tag, pb_wire_type_t wire_type, pb_extension_t *extension);
static bool pb_field_set_to_default(pb_field_iter_t *field);
static bool pb_message_set_to_defaults(pb_field_iter_t *iter);
static bool checkreturn pb_dec_bool(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_dec_varint(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_dec_bytes(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_dec_string(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_dec_submessage(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_dec_fixed_length_bytes(pb_istream_t *stream, const pb_field_iter_t *field);
static bool checkreturn pb_skip_varint(pb_istream_t *stream);
static bool checkreturn pb_skip_string(pb_istream_t *stream);

#ifdef PB_ENABLE_MALLOC
static bool checkreturn allocate_field(pb_istream_t *stream, void *pData, size_t data_size, size_t array_size);
static void initialize_pointer_field(void *pItem, pb_field_iter_t *field);
static bool checkreturn pb_release_union_field(pb_istream_t *stream, pb_field_iter_t *field);
static void pb_release_single_field(pb_field_iter_t *field);
#endif

#ifdef PB_WITHOUT_64BIT
#define pb_int64_t int32_t
#define pb_uint64_t uint32_t
#else
#define pb_int64_t int64_t
#define pb_uint64_t uint64_t
#endif

typedef struct {
    uint32_t bitfield[(PB_MAX_REQUIRED_FIELDS + 31) / 32];
} pb_fields_seen_t;

/*******************************
 * pb_istream_t implementation *
 *******************************/

static bool checkreturn buf_read(pb_istream_t *stream, pb_byte_t *buf, size_t count)
{
    const pb_byte_t *source = (const pb_byte_t*)stream->state;
    stream->state = (pb_byte_t*)stream->state + count;
    
    if (buf != NULL)
    {
        memcpy(buf, source, count * sizeof(pb_byte_t));
    }
    
    return true;
}

bool checkreturn pb_read(pb_istream_t *stream, pb_byte_t *buf, size_t count)
{
    if (count == 0)
        return true;

#ifndef PB_BUFFER_ONLY
	if (buf == NULL && stream->callback != buf_read)
	{
		/* Skip input bytes */
		pb_byte_t tmp[16];
		while (count > 16)
		{
			if (!pb_read(stream, tmp, 16))
				return false;
			
			count -= 16;
		}
		
		return pb_read(stream, tmp, count);
	}
#endif

    if (stream->bytes_left < count)
        PB_RETURN_ERROR(stream, "end-of-stream");
    
#ifndef PB_BUFFER_ONLY
    if (!stream->callback(stream, buf, count))
        PB_RETURN_ERROR(stream, "io error");
#else
    if (!buf_read(stream, buf, count))
        return false;
#endif
    
    if (stream->bytes_left < count)
        stream->bytes_left = 0;
    else
        stream->bytes_left -= count;

    return true;
}

/* Read a single byte from input stream. buf may not be NULL.
 * This is an optimization for the varint decoding. */
static bool checkreturn pb_readbyte(pb_istream_t *stream, pb_byte_t *buf)
{
    if (stream->bytes_left == 0)
        PB_RETURN_ERROR(stream, "end-of-stream");

#ifndef PB_BUFFER_ONLY
    if (!stream->callback(stream, buf, 1))
        PB_RETURN_ERROR(stream, "io error");
#else
    *buf = *(const pb_byte_t*)stream->state;
    stream->state = (pb_byte_t*)stream->state + 1;
#endif

    stream->bytes_left--;
    
    return true;    
}

pb_istream_t pb_istream_from_buffer(const pb_byte_t *buf, size_t msglen)
{
    pb_istream_t stream;
    /* Cast away the const from buf without a compiler error.  We are
     * careful to use it only in a const manner in the callbacks.
     */
    union {
        void *state;
        const void *c_state;
    } state;
#ifdef PB_BUFFER_ONLY
    stream.callback = NULL;
#else
    stream.callback = &buf_read;
#endif
    state.c_state = buf;
    stream.state = state.state;
    stream.bytes_left = msglen;
#ifndef PB_NO_ERRMSG
    stream.errmsg = NULL;
#endif
    return stream;
}

/********************
 * Helper functions *
 ********************/

static bool checkreturn pb_decode_varint32_eof(pb_istream_t *stream, uint32_t *dest, bool *eof)
{
    pb_byte_t byte;
    uint32_t result;
    
    if (!pb_readbyte(stream, &byte))
    {
        if (stream->bytes_left == 0)
        {
            if (eof)
            {
                *eof = true;
            }
        }

        return false;
    }
    
    if ((byte & 0x80) == 0)
    {
        /* Quick case, 1 byte value */
        result = byte;
    }
    else
    {
        /* Multibyte case */
        uint_fast8_t bitpos = 7;
        result = byte & 0x7F;
        
        do
        {
            if (!pb_readbyte(stream, &byte))
                return false;
            
            if (bitpos >= 32)
            {
                /* Note: The varint could have trailing 0x80 bytes, or 0xFF for negative. */
                pb_byte_t sign_extension = (bitpos < 63) ? 0xFF : 0x01;
                bool valid_extension = ((byte & 0x7F) == 0x00 ||
                         ((result >> 31) != 0 && byte == sign_extension));

                if (bitpos >= 64 || !valid_extension)
                {
                    PB_RETURN_ERROR(stream, "varint overflow");
                }
            }
            else if (bitpos == 28)
            {
                if ((byte & 0x70) != 0 && (byte & 0x78) != 0x78)
                {
                    PB_RETURN_ERROR(stream, "varint overflow");
                }
                result |= (uint32_t)(byte & 0x0F) << bitpos;
            }
            else
            {
                result |= (uint32_t)(byte & 0x7F) << bitpos;
            }
            bitpos = (uint_fast8_t)(bitpos + 7);
        } while (byte & 0x80);
   }
   
   *dest = result;
   return true;
}

bool checkreturn pb_decode_varint32(pb_istream_t *stream, uint32_t *dest)
{
    return pb_decode_varint32_eof(stream, dest, NULL);
}

#ifndef PB_WITHOUT_64BIT
bool checkreturn pb_decode_varint(pb_istream_t *stream, uint64_t *dest)
{
    pb_byte_t byte;
    uint_fast8_t bitpos = 0;
    uint64_t result = 0;
    
    do
    {
        if (!pb_readbyte(stream, &byte))
            return false;

        if (bitpos >= 63 && (byte & 0xFE) != 0)
            PB_RETURN_ERROR(stream, "varint overflow");

        result |= (uint64_t)(byte & 0x7F) << bitpos;
        bitpos = (uint_fast8_t)(bitpos + 7);
    } while (byte & 0x80);
    
    *dest = result;
    return true;
}
#endif

bool checkreturn pb_skip_varint(pb_istream_t *stream)
{
    pb_byte_t byte;
    do
    {
        if (!pb_read(stream, &byte, 1))
            return false;
    } while (byte & 0x80);
    return true;
}

bool checkreturn pb_skip_string(pb_istream_t *stream)
{
    uint32_t length;
    if (!pb_decode_varint32(stream, &length))
        return false;
    
    if ((size_t)length != length)
    {
        PB_RETURN_ERROR(stream, "size too large");
    }

    return pb_read(stream, NULL, (size_t)length);
}

bool checkreturn pb_decode_tag(pb_istream_t *stream, pb_wire_type_t *wire_type, uint32_t *tag, bool *eof)
{
    uint32_t temp;
    *eof = false;
    *wire_type = (pb_wire_type_t) 0;
    *tag = 0;
    
    if (!pb_decode_varint32_eof(stream, &temp, eof))
    {
        return false;
    }
    
    *tag = temp >> 3;
    *wire_type = (pb_wire_type_t)(temp & 7);
    return true;
}

bool checkreturn pb_skip_field(pb_istream_t *stream, pb_wire_type_t wire_type)
{
    switch (wire_type)
    {
        case PB_WT_VARINT: return pb_skip_varint(stream);
        case PB_WT_64BIT: return pb_read(stream, NULL, 8);
        case PB_WT_STRING: return pb_skip_string(stream);
        case PB_WT_32BIT: return pb_read(stream, NULL, 4);
        default: PB_RETURN_ERROR(stream, "invalid wire_type");
    }
}

/* Read a raw value to buffer, for the purpose of passing it to callback as
 * a substream. Size is maximum size on call, and actual size on return.
 */
static bool checkreturn read_raw_value(pb_istream_t *stream, pb_wire_type_t wire_type, pb_byte_t *buf, size_t *size)
{
    size_t max_size = *size;
    switch (wire_type)
    {
        case PB_WT_VARINT:
            *size = 0;
            do
            {
                (*size)++;
                if (*size > max_size)
                    PB_RETURN_ERROR(stream, "varint overflow");

                if (!pb_read(stream, buf, 1))
                    return false;
            } while (*buf++ & 0x80);
            return true;
            
        case PB_WT_64BIT:
            *size = 8;
            return pb_read(stream, buf, 8);
        
        case PB_WT_32BIT:
            *size = 4;
            return pb_read(stream, buf, 4);
        
        case PB_WT_STRING:
            /* Calling read_raw_value with a PB_WT_STRING is an error.
             * Explicitly handle this case and fallthrough to default to avoid
             * compiler warnings.
             */

        default: PB_RETURN_ERROR(stream, "invalid wire_type");
    }
}

/* Decode string length from stream and return a substream with limited length.
 * Remember to close the substream using pb_close_string_substream().
 */
bool checkreturn pb_make_string_substream(pb_istream_t *stream, pb_istream_t *substream)
{
    uint32_t size;
    if (!pb_decode_varint32(stream, &size))
        return false;
    
    *substream = *stream;
    if (substream->bytes_left < size)
        PB_RETURN_ERROR(stream, "parent stream too short");
    
    substream->bytes_left = (size_t)size;
    stream->bytes_left -= (size_t)size;
    return true;
}

bool checkreturn pb_close_string_substream(pb_istream_t *stream, pb_istream_t *substream)
{
    if (substream->bytes_left) {
        if (!pb_read(substream, NULL, substream->bytes_left))
            return false;
    }

    stream->state = substream->state;

#ifndef PB_NO_ERRMSG
    stream->errmsg = substream->errmsg;
#endif
    return true;
}

/*************************
 * Decode a single field *
 *************************/

static bool checkreturn decode_basic_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field)
{
    switch (PB_LTYPE(field->type))
    {
        case PB_LTYPE_BOOL:
            if (wire_type != PB_WT_VARINT && wire_type != PB_WT_PACKED)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_bool(stream, field);

        case PB_LTYPE_VARINT:
        case PB_LTYPE_UVARINT:
        case PB_LTYPE_SVARINT:
            if (wire_type != PB_WT_VARINT && wire_type != PB_WT_PACKED)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_varint(stream, field);

        case PB_LTYPE_FIXED32:
            if (wire_type != PB_WT_32BIT && wire_type != PB_WT_PACKED)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_decode_fixed32(stream, field->pData);

        case PB_LTYPE_FIXED64:
            if (wire_type != PB_WT_64BIT && wire_type != PB_WT_PACKED)
                PB_RETURN_ERROR(stream, "wrong wire type");

#ifdef PB_CONVERT_DOUBLE_FLOAT
            if (field->data_size == sizeof(float))
            {
                return pb_decode_double_as_float(stream, (float*)field->pData);
            }
#endif

#ifdef PB_WITHOUT_64BIT
            PB_RETURN_ERROR(stream, "invalid data_size");
#else
            return pb_decode_fixed64(stream, field->pData);
#endif

        case PB_LTYPE_BYTES:
            if (wire_type != PB_WT_STRING)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_bytes(stream, field);

        case PB_LTYPE_STRING:
            if (wire_type != PB_WT_STRING)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_string(stream, field);

        case PB_LTYPE_SUBMESSAGE:
        case PB_LTYPE_SUBMSG_W_CB:
            if (wire_type != PB_WT_STRING)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_submessage(stream, field);

        case PB_LTYPE_FIXED_LENGTH_BYTES:
            if (wire_type != PB_WT_STRING)
                PB_RETURN_ERROR(stream, "wrong wire type");

            return pb_dec_fixed_length_bytes(stream, field);

        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
}

static bool checkreturn decode_static_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field)
{
    switch (PB_HTYPE(field->type))
    {
        case PB_HTYPE_REQUIRED:
            return decode_basic_field(stream, wire_type, field);
            
        case PB_HTYPE_OPTIONAL:
            if (field->pSize != NULL)
                *(bool*)field->pSize = true;
            return decode_basic_field(stream, wire_type, field);
    
        case PB_HTYPE_REPEATED:
            if (wire_type == PB_WT_STRING
                && PB_LTYPE(field->type) <= PB_LTYPE_LAST_PACKABLE)
            {
                /* Packed array */
                bool status = true;
                pb_istream_t substream;
                pb_size_t *size = (pb_size_t*)field->pSize;
                field->pData = (char*)field->pField + field->data_size * (*size);

                if (!pb_make_string_substream(stream, &substream))
                    return false;

                while (substream.bytes_left > 0 && *size < field->array_size)
                {
                    if (!decode_basic_field(&substream, PB_WT_PACKED, field))
                    {
                        status = false;
                        break;
                    }
                    (*size)++;
                    field->pData = (char*)field->pData + field->data_size;
                }

                if (substream.bytes_left != 0)
                    PB_RETURN_ERROR(stream, "array overflow");
                if (!pb_close_string_substream(stream, &substream))
                    return false;

                return status;
            }
            else
            {
                /* Repeated field */
                pb_size_t *size = (pb_size_t*)field->pSize;
                field->pData = (char*)field->pField + field->data_size * (*size);

                if ((*size)++ >= field->array_size)
                    PB_RETURN_ERROR(stream, "array overflow");

                return decode_basic_field(stream, wire_type, field);
            }

        case PB_HTYPE_ONEOF:
            if (PB_LTYPE_IS_SUBMSG(field->type) &&
                *(pb_size_t*)field->pSize != field->tag)
            {
                /* We memset to zero so that any callbacks are set to NULL.
                 * This is because the callbacks might otherwise have values
                 * from some other union field.
                 * If callbacks are needed inside oneof field, use .proto
                 * option submsg_callback to have a separate callback function
                 * that can set the fields before submessage is decoded.
                 * pb_dec_submessage() will set any default values. */
                memset(field->pData, 0, (size_t)field->data_size);

                /* Set default values for the submessage fields. */
                if (field->submsg_desc->default_value != NULL ||
                    field->submsg_desc->field_callback != NULL ||
                    field->submsg_desc->submsg_info[0] != NULL)
                {
                    pb_field_iter_t submsg_iter;
                    if (pb_field_iter_begin(&submsg_iter, field->submsg_desc, field->pData))
                    {
                        if (!pb_message_set_to_defaults(&submsg_iter))
                            PB_RETURN_ERROR(stream, "failed to set defaults");
                    }
                }
            }
            *(pb_size_t*)field->pSize = field->tag;

            return decode_basic_field(stream, wire_type, field);

        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
}

#ifdef PB_ENABLE_MALLOC
/* Allocate storage for the field and store the pointer at iter->pData.
 * array_size is the number of entries to reserve in an array.
 * Zero size is not allowed, use pb_free() for releasing.
 */
static bool checkreturn allocate_field(pb_istream_t *stream, void *pData, size_t data_size, size_t array_size)
{    
    void *ptr = *(void**)pData;
    
    if (data_size == 0 || array_size == 0)
        PB_RETURN_ERROR(stream, "invalid size");
    
#ifdef __AVR__
    /* Workaround for AVR libc bug 53284: http://savannah.nongnu.org/bugs/?53284
     * Realloc to size of 1 byte can cause corruption of the malloc structures.
     */
    if (data_size == 1 && array_size == 1)
    {
        data_size = 2;
    }
#endif

    /* Check for multiplication overflows.
     * This code avoids the costly division if the sizes are small enough.
     * Multiplication is safe as long as only half of bits are set
     * in either multiplicand.
     */
    {
        const size_t check_limit = (size_t)1 << (sizeof(size_t) * 4);
        if (data_size >= check_limit || array_size >= check_limit)
        {
            const size_t size_max = (size_t)-1;
            if (size_max / array_size < data_size)
            {
                PB_RETURN_ERROR(stream, "size too large");
            }
        }
    }
    
    /* Allocate new or expand previous allocation */
    /* Note: on failure the old pointer will remain in the structure,
     * the message must be freed by caller also on error return. */
    ptr = pb_realloc(ptr, array_size * data_size);
    if (ptr == NULL)
        PB_RETURN_ERROR(stream, "realloc failed");
    
    *(void**)pData = ptr;
    return true;
}

/* Clear a newly allocated item in case it contains a pointer, or is a submessage. */
static void initialize_pointer_field(void *pItem, pb_field_iter_t *field)
{
    if (PB_LTYPE(field->type) == PB_LTYPE_STRING ||
        PB_LTYPE(field->type) == PB_LTYPE_BYTES)
    {
        *(void**)pItem = NULL;
    }
    else if (PB_LTYPE_IS_SUBMSG(field->type))
    {
        /* We memset to zero so that any callbacks are set to NULL.
         * Default values will be set by pb_dec_submessage(). */
        memset(pItem, 0, field->data_size);
    }
}
#endif

static bool checkreturn decode_pointer_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field)
{
#ifndef PB_ENABLE_MALLOC
    PB_UNUSED(wire_type);
    PB_UNUSED(field);
    PB_RETURN_ERROR(stream, "no malloc support");
#else
    switch (PB_HTYPE(field->type))
    {
        case PB_HTYPE_REQUIRED:
        case PB_HTYPE_OPTIONAL:
        case PB_HTYPE_ONEOF:
            if (PB_LTYPE_IS_SUBMSG(field->type) && *(void**)field->pField != NULL)
            {
                /* Duplicate field, have to release the old allocation first. */
                /* FIXME: Does this work correctly for oneofs? */
                pb_release_single_field(field);
            }
        
            if (PB_HTYPE(field->type) == PB_HTYPE_ONEOF)
            {
                *(pb_size_t*)field->pSize = field->tag;
            }

            if (PB_LTYPE(field->type) == PB_LTYPE_STRING ||
                PB_LTYPE(field->type) == PB_LTYPE_BYTES)
            {
                /* pb_dec_string and pb_dec_bytes handle allocation themselves */
                field->pData = field->pField;
                return decode_basic_field(stream, wire_type, field);
            }
            else
            {
                if (!allocate_field(stream, field->pField, field->data_size, 1))
                    return false;
                
                field->pData = *(void**)field->pField;
                initialize_pointer_field(field->pData, field);
                return decode_basic_field(stream, wire_type, field);
            }
    
        case PB_HTYPE_REPEATED:
            if (wire_type == PB_WT_STRING
                && PB_LTYPE(field->type) <= PB_LTYPE_LAST_PACKABLE)
            {
                /* Packed array, multiple items come in at once. */
                bool status = true;
                pb_size_t *size = (pb_size_t*)field->pSize;
                size_t allocated_size = *size;
                pb_istream_t substream;
                
                if (!pb_make_string_substream(stream, &substream))
                    return false;
                
                while (substream.bytes_left)
                {
                    if (*size == PB_SIZE_MAX)
                    {
#ifndef PB_NO_ERRMSG
                        stream->errmsg = "too many array entries";
#endif
                        status = false;
                        break;
                    }

                    if ((size_t)*size + 1 > allocated_size)
                    {
                        /* Allocate more storage. This tries to guess the
                         * number of remaining entries. Round the division
                         * upwards. */
                        size_t remain = (substream.bytes_left - 1) / field->data_size + 1;
                        if (remain < PB_SIZE_MAX - allocated_size)
                            allocated_size += remain;
                        else
                            allocated_size += 1;
                        
                        if (!allocate_field(&substream, field->pField, field->data_size, allocated_size))
                        {
                            status = false;
                            break;
                        }
                    }

                    /* Decode the array entry */
                    field->pData = *(char**)field->pField + field->data_size * (*size);
                    if (field->pData == NULL)
                    {
                        /* Shouldn't happen, but satisfies static analyzers */
                        status = false;
                        break;
                    }
                    initialize_pointer_field(field->pData, field);
                    if (!decode_basic_field(&substream, PB_WT_PACKED, field))
                    {
                        status = false;
                        break;
                    }
                    
                    (*size)++;
                }
                if (!pb_close_string_substream(stream, &substream))
                    return false;
                
                return status;
            }
            else
            {
                /* Normal repeated field, i.e. only one item at a time. */
                pb_size_t *size = (pb_size_t*)field->pSize;

                if (*size == PB_SIZE_MAX)
                    PB_RETURN_ERROR(stream, "too many array entries");
                
                if (!allocate_field(stream, field->pField, field->data_size, (size_t)(*size + 1)))
                    return false;
            
                field->pData = *(char**)field->pField + field->data_size * (*size);
                (*size)++;
                initialize_pointer_field(field->pData, field);
                return decode_basic_field(stream, wire_type, field);
            }

        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
#endif
}

static bool checkreturn decode_callback_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field)
{
    if (!field->descriptor->field_callback)
        return pb_skip_field(stream, wire_type);

    if (wire_type == PB_WT_STRING)
    {
        pb_istream_t substream;
        size_t prev_bytes_left;
        
        if (!pb_make_string_substream(stream, &substream))
            return false;
        
        do
        {
            prev_bytes_left = substream.bytes_left;
            if (!field->descriptor->field_callback(&substream, NULL, field))
            {
                PB_SET_ERROR(stream, substream.errmsg ? substream.errmsg : "callback failed");
                return false;
            }
        } while (substream.bytes_left > 0 && substream.bytes_left < prev_bytes_left);
        
        if (!pb_close_string_substream(stream, &substream))
            return false;

        return true;
    }
    else
    {
        /* Copy the single scalar value to stack.
         * This is required so that we can limit the stream length,
         * which in turn allows to use same callback for packed and
         * not-packed fields. */
        pb_istream_t substream;
        pb_byte_t buffer[10];
        size_t size = sizeof(buffer);
        
        if (!read_raw_value(stream, wire_type, buffer, &size))
            return false;
        substream = pb_istream_from_buffer(buffer, size);
        
        return field->descriptor->field_callback(&substream, NULL, field);
    }
}

static bool checkreturn decode_field(pb_istream_t *stream, pb_wire_type_t wire_type, pb_field_iter_t *field)
{
#ifdef PB_ENABLE_MALLOC
    /* When decoding an oneof field, check if there is old data that must be
     * released first. */
    if (PB_HTYPE(field->type) == PB_HTYPE_ONEOF)
    {
        if (!pb_release_union_field(stream, field))
            return false;
    }
#endif

    switch (PB_ATYPE(field->type))
    {
        case PB_ATYPE_STATIC:
            return decode_static_field(stream, wire_type, field);
        
        case PB_ATYPE_POINTER:
            return decode_pointer_field(stream, wire_type, field);
        
        case PB_ATYPE_CALLBACK:
            return decode_callback_field(stream, wire_type, field);
        
        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
}

/* Default handler for extension fields. Expects to have a pb_msgdesc_t
 * pointer in the extension->type->arg field, pointing to a message with
 * only one field in it.  */
static bool checkreturn default_extension_decoder(pb_istream_t *stream,
    pb_extension_t *extension, uint32_t tag, pb_wire_type_t wire_type)
{
    pb_field_iter_t iter;

    if (!pb_field_iter_begin_extension(&iter, extension))
        PB_RETURN_ERROR(stream, "invalid extension");

    if (iter.tag != tag || !iter.message)
        return true;

    extension->found = true;
    return decode_field(stream, wire_type, &iter);
}

/* Try to decode an unknown field as an extension field. Tries each extension
 * decoder in turn, until one of them handles the field or loop ends. */
static bool checkreturn decode_extension(pb_istream_t *stream,
    uint32_t tag, pb_wire_type_t wire_type, pb_extension_t *extension)
{
    size_t pos = stream->bytes_left;
    
    while (extension != NULL && pos == stream->bytes_left)
    {
        bool status;
        if (extension->type->decode)
            status = extension->type->decode(stream, extension, tag, wire_type);
        else
            status = default_extension_decoder(stream, extension, tag, wire_type);

        if (!status)
            return false;
        
        extension = extension->next;
    }
    
    return true;
}

/* Initialize message fields to default values, recursively */
static bool pb_field_set_to_default(pb_field_iter_t *field)
{
    pb_type_t type;
    type = field->type;

    if (PB_LTYPE(type) == PB_LTYPE_EXTENSION)
    {
        pb_extension_t *ext = *(pb_extension_t* const *)field->pData;
        while (ext != NULL)
        {
            pb_field_iter_t ext_iter;
            if (pb_field_iter_begin_extension(&ext_iter, ext))
            {
                ext->found = false;
                if (!pb_message_set_to_defaults(&ext_iter))
                    return false;
            }
            ext = ext->next;
        }
    }
    else if (PB_ATYPE(type) == PB_ATYPE_STATIC)
    {
        bool init_data = true;
        if (PB_HTYPE(type) == PB_HTYPE_OPTIONAL && field->pSize != NULL)
        {
            /* Set has_field to false. Still initialize the optional field
             * itself also. */
            *(bool*)field->pSize = false;
        }
        else if (PB_HTYPE(type) == PB_HTYPE_REPEATED ||
                 PB_HTYPE(type) == PB_HTYPE_ONEOF)
        {
            /* REPEATED: Set array count to 0, no need to initialize contents.
               ONEOF: Set which_field to 0. */
            *(pb_size_t*)field->pSize = 0;
            init_data = false;
        }

        if (init_data)
        {
            if (PB_LTYPE_IS_SUBMSG(field->type) &&
                (field->submsg_desc->default_value != NULL ||
                 field->submsg_desc->field_callback != NULL ||
                 field->submsg_desc->submsg_info[0] != NULL))
            {
                /* Initialize submessage to defaults.
                 * Only needed if it has default values
                 * or callback/submessage fields. */
                pb_field_iter_t submsg_iter;
                if (pb_field_iter_begin(&submsg_iter, field->submsg_desc, field->pData))
                {
                    if (!pb_message_set_to_defaults(&submsg_iter))
                        return false;
                }
            }
            else
            {
                /* Initialize to zeros */
                memset(field->pData, 0, (size_t)field->data_size);
            }
        }
    }
    else if (PB_ATYPE(type) == PB_ATYPE_POINTER)
    {
        /* Initialize the pointer to NULL. */
        *(void**)field->pField = NULL;

        /* Initialize array count to 0. */
        if (PB_HTYPE(type) == PB_HTYPE_REPEATED ||
            PB_HTYPE(type) == PB_HTYPE_ONEOF)
        {
            *(pb_size_t*)field->pSize = 0;
        }
    }
    else if (PB_ATYPE(type) == PB_ATYPE_CALLBACK)
    {
        /* Don't overwrite callback */
    }

    return true;
}

static bool pb_message_set_to_defaults(pb_field_iter_t *iter)
{
    pb_istream_t defstream = PB_ISTREAM_EMPTY;
    uint32_t tag = 0;
    pb_wire_type_t wire_type = PB_WT_VARINT;
    bool eof;

    if (iter->descriptor->default_value)
    {
        defstream = pb_istream_from_buffer(iter->descriptor->default_value, (size_t)-1);
        if (!pb_decode_tag(&defstream, &wire_type, &tag, &eof))
            return false;
    }

    do
    {
        if (!pb_field_set_to_default(iter))
            return false;

        if (tag != 0 && iter->tag == tag)
        {
            /* We have a default value for this field in the defstream */
            if (!decode_field(&defstream, wire_type, iter))
                return false;
            if (!pb_decode_tag(&defstream, &wire_type, &tag, &eof))
                return false;

            if (iter->pSize)
                *(bool*)iter->pSize = false;
        }
    } while (pb_field_iter_next(iter));

    return true;
}

/*********************
 * Decode all fields *
 *********************/

static bool checkreturn pb_decode_inner(pb_istream_t *stream, const pb_msgdesc_t *fields, void *dest_struct, unsigned int flags)
{
    uint32_t extension_range_start = 0;
    pb_extension_t *extensions = NULL;

    /* 'fixed_count_field' and 'fixed_count_size' track position of a repeated fixed
     * count field. This can only handle _one_ repeated fixed count field that
     * is unpacked and unordered among other (non repeated fixed count) fields.
     */
    pb_size_t fixed_count_field = PB_SIZE_MAX;
    pb_size_t fixed_count_size = 0;
    pb_size_t fixed_count_total_size = 0;

    pb_fields_seen_t fields_seen = {{0, 0}};
    const uint32_t allbits = ~(uint32_t)0;
    pb_field_iter_t iter;

    if (pb_field_iter_begin(&iter, fields, dest_struct))
    {
        if ((flags & PB_DECODE_NOINIT) == 0)
        {
            if (!pb_message_set_to_defaults(&iter))
                PB_RETURN_ERROR(stream, "failed to set defaults");
        }
    }

    while (stream->bytes_left)
    {
        uint32_t tag;
        pb_wire_type_t wire_type;
        bool eof;

        if (!pb_decode_tag(stream, &wire_type, &tag, &eof))
        {
            if (eof)
                break;
            else
                return false;
        }

        if (tag == 0)
        {
          if (flags & PB_DECODE_NULLTERMINATED)
          {
            break;
          }
          else
          {
            PB_RETURN_ERROR(stream, "zero tag");
          }
        }

        if (!pb_field_iter_find(&iter, tag) || PB_LTYPE(iter.type) == PB_LTYPE_EXTENSION)
        {
            /* No match found, check if it matches an extension. */
            if (extension_range_start == 0)
            {
                if (pb_field_iter_find_extension(&iter))
                {
                    extensions = *(pb_extension_t* const *)iter.pData;
                    extension_range_start = iter.tag;
                }

                if (!extensions)
                {
                    extension_range_start = (uint32_t)-1;
                }
            }

            if (tag >= extension_range_start)
            {
                size_t pos = stream->bytes_left;

                if (!decode_extension(stream, tag, wire_type, extensions))
                    return false;

                if (pos != stream->bytes_left)
                {
                    /* The field was handled */
                    continue;
                }
            }

            /* No match found, skip data */
            if (!pb_skip_field(stream, wire_type))
                return false;
            continue;
        }

        /* If a repeated fixed count field was found, get size from
         * 'fixed_count_field' as there is no counter contained in the struct.
         */
        if (PB_HTYPE(iter.type) == PB_HTYPE_REPEATED && iter.pSize == &iter.array_size)
        {
            if (fixed_count_field != iter.index) {
                /* If the new fixed count field does not match the previous one,
                 * check that the previous one is NULL or that it finished
                 * receiving all the expected data.
                 */
                if (fixed_count_field != PB_SIZE_MAX &&
                    fixed_count_size != fixed_count_total_size)
                {
                    PB_RETURN_ERROR(stream, "wrong size for fixed count field");
                }

                fixed_count_field = iter.index;
                fixed_count_size = 0;
                fixed_count_total_size = iter.array_size;
            }

            iter.pSize = &fixed_count_size;
        }

        if (PB_HTYPE(iter.type) == PB_HTYPE_REQUIRED
            && iter.required_field_index < PB_MAX_REQUIRED_FIELDS)
        {
            uint32_t tmp = ((uint32_t)1 << (iter.required_field_index & 31));
            fields_seen.bitfield[iter.required_field_index >> 5] |= tmp;
        }

        if (!decode_field(stream, wire_type, &iter))
            return false;
    }

    /* Check that all elements of the last decoded fixed count field were present. */
    if (fixed_count_field != PB_SIZE_MAX &&
        fixed_count_size != fixed_count_total_size)
    {
        PB_RETURN_ERROR(stream, "wrong size for fixed count field");
    }

    /* Check that all required fields were present. */
    {
        pb_size_t req_field_count = iter.descriptor->required_field_count;

        if (req_field_count > 0)
        {
            pb_size_t i;

            if (req_field_count > PB_MAX_REQUIRED_FIELDS)
                req_field_count = PB_MAX_REQUIRED_FIELDS;

            /* Check the whole words */
            for (i = 0; i < (req_field_count >> 5); i++)
            {
                if (fields_seen.bitfield[i] != allbits)
                    PB_RETURN_ERROR(stream, "missing required field");
            }

            /* Check the remaining bits (if any) */
            if ((req_field_count & 31) != 0)
            {
                if (fields_seen.bitfield[req_field_count >> 5] !=
                    (allbits >> (uint_least8_t)(32 - (req_field_count & 31))))
                {
                    PB_RETURN_ERROR(stream, "missing required field");
                }
            }
        }
    }

    return true;
}

bool checkreturn pb_decode_ex(pb_istream_t *stream, const pb_msgdesc_t *fields, void *dest_struct, unsigned int flags)
{
    bool status;

    if ((flags & PB_DECODE_DELIMITED) == 0)
    {
      status = pb_decode_inner(stream, fields, dest_struct, flags);
    }
    else
    {
      pb_istream_t substream;
      if (!pb_make_string_substream(stream, &substream))
        return false;

      status = pb_decode_inner(&substream, fields, dest_struct, flags);

      if (!pb_close_string_substream(stream, &substream))
        return false;
    }
    
#ifdef PB_ENABLE_MALLOC
    if (!status)
        pb_release(fields, dest_struct);
#endif
    
    return status;
}

bool checkreturn pb_decode(pb_istream_t *stream, const pb_msgdesc_t *fields, void *dest_struct)
{
    bool status;

    status = pb_decode_inner(stream, fields, dest_struct, 0);

#ifdef PB_ENABLE_MALLOC
    if (!status)
        pb_release(fields, dest_struct);
#endif

    return status;
}

#ifdef PB_ENABLE_MALLOC
/* Given an oneof field, if there has already been a field inside this oneof,
 * release it before overwriting with a different one. */
static bool pb_release_union_field(pb_istream_t *stream, pb_field_iter_t *field)
{
    pb_field_iter_t old_field = *field;
    pb_size_t old_tag = *(pb_size_t*)field->pSize; /* Previous which_ value */
    pb_size_t new_tag = field->tag; /* New which_ value */

    if (old_tag == 0)
        return true; /* Ok, no old data in union */

    if (old_tag == new_tag)
        return true; /* Ok, old data is of same type => merge */

    /* Release old data. The find can fail if the message struct contains
     * invalid data. */
    if (!pb_field_iter_find(&old_field, old_tag))
        PB_RETURN_ERROR(stream, "invalid union tag");

    pb_release_single_field(&old_field);

    if (PB_ATYPE(field->type) == PB_ATYPE_POINTER)
    {
        /* Initialize the pointer to NULL to make sure it is valid
         * even in case of error return. */
        *(void**)field->pField = NULL;
        field->pData = NULL;
    }

    return true;
}

static void pb_release_single_field(pb_field_iter_t *field)
{
    pb_type_t type;
    type = field->type;

    if (PB_HTYPE(type) == PB_HTYPE_ONEOF)
    {
        if (*(pb_size_t*)field->pSize != field->tag)
            return; /* This is not the current field in the union */
    }

    /* Release anything contained inside an extension or submsg.
     * This has to be done even if the submsg itself is statically
     * allocated. */
    if (PB_LTYPE(type) == PB_LTYPE_EXTENSION)
    {
        /* Release fields from all extensions in the linked list */
        pb_extension_t *ext = *(pb_extension_t**)field->pData;
        while (ext != NULL)
        {
            pb_field_iter_t ext_iter;
            if (pb_field_iter_begin_extension(&ext_iter, ext))
            {
                pb_release_single_field(&ext_iter);
            }
            ext = ext->next;
        }
    }
    else if (PB_LTYPE_IS_SUBMSG(type) && PB_ATYPE(type) != PB_ATYPE_CALLBACK)
    {
        /* Release fields in submessage or submsg array */
        pb_size_t count = 1;
        
        if (PB_ATYPE(type) == PB_ATYPE_POINTER)
        {
            field->pData = *(void**)field->pField;
        }
        else
        {
            field->pData = field->pField;
        }
        
        if (PB_HTYPE(type) == PB_HTYPE_REPEATED)
        {
            count = *(pb_size_t*)field->pSize;

            if (PB_ATYPE(type) == PB_ATYPE_STATIC && count > field->array_size)
            {
                /* Protect against corrupted _count fields */
                count = field->array_size;
            }
        }
        
        if (field->pData)
        {
            for (; count > 0; count--)
            {
                pb_release(field->submsg_desc, field->pData);
                field->pData = (char*)field->pData + field->data_size;
            }
        }
    }
    
    if (PB_ATYPE(type) == PB_ATYPE_POINTER)
    {
        if (PB_HTYPE(type) == PB_HTYPE_REPEATED &&
            (PB_LTYPE(type) == PB_LTYPE_STRING ||
             PB_LTYPE(type) == PB_LTYPE_BYTES))
        {
            /* Release entries in repeated string or bytes array */
            void **pItem = *(void***)field->pField;
            pb_size_t count = *(pb_size_t*)field->pSize;
            for (; count > 0; count--)
            {
                pb_free(*pItem);
                *pItem++ = NULL;
            }
        }
        
        if (PB_HTYPE(type) == PB_HTYPE_REPEATED)
        {
            /* We are going to release the array, so set the size to 0 */
            *(pb_size_t*)field->pSize = 0;
        }
        
        /* Release main pointer */
        pb_free(*(void**)field->pField);
        *(void**)field->pField = NULL;
    }
}

void pb_release(const pb_msgdesc_t *fields, void *dest_struct)
{
    pb_field_iter_t iter;
    
    if (!dest_struct)
        return; /* Ignore NULL pointers, similar to free() */

    if (!pb_field_iter_begin(&iter, fields, dest_struct))
        return; /* Empty message type */
    
    do
    {
        pb_release_single_field(&iter);
    } while (pb_field_iter_next(&iter));
}
#else
void pb_release(const pb_msgdesc_t *fields, void *dest_struct)
{
    /* Nothing to release without PB_ENABLE_MALLOC. */
    PB_UNUSED(fields);
    PB_UNUSED(dest_struct);
}
#endif

/* Field decoders */

bool pb_decode_bool(pb_istream_t *stream, bool *dest)
{
    uint32_t value;
    if (!pb_decode_varint32(stream, &value))
        return false;

    *(bool*)dest = (value != 0);
    return true;
}

bool pb_decode_svarint(pb_istream_t *stream, pb_int64_t *dest)
{
    pb_uint64_t value;
    if (!pb_decode_varint(stream, &value))
        return false;
    
    if (value & 1)
        *dest = (pb_int64_t)(~(value >> 1));
    else
        *dest = (pb_int64_t)(value >> 1);
    
    return true;
}

bool pb_decode_fixed32(pb_istream_t *stream, void *dest)
{
    union {
        uint32_t fixed32;
        pb_byte_t bytes[4];
    } u;

    if (!pb_read(stream, u.bytes, 4))
        return false;

#if defined(PB_LITTLE_ENDIAN_8BIT) && PB_LITTLE_ENDIAN_8BIT == 1
    /* fast path - if we know that we're on little endian, assign directly */
    *(uint32_t*)dest = u.fixed32;
#else
    *(uint32_t*)dest = ((uint32_t)u.bytes[0] << 0) |
                       ((uint32_t)u.bytes[1] << 8) |
                       ((uint32_t)u.bytes[2] << 16) |
                       ((uint32_t)u.bytes[3] << 24);
#endif
    return true;
}

#ifndef PB_WITHOUT_64BIT
bool pb_decode_fixed64(pb_istream_t *stream, void *dest)
{
    union {
        uint64_t fixed64;
        pb_byte_t bytes[8];
    } u;

    if (!pb_read(stream, u.bytes, 8))
        return false;

#if defined(PB_LITTLE_ENDIAN_8BIT) && PB_LITTLE_ENDIAN_8BIT == 1
    /* fast path - if we know that we're on little endian, assign directly */
    *(uint64_t*)dest = u.fixed64;
#else
    *(uint64_t*)dest = ((uint64_t)u.bytes[0] << 0) |
                       ((uint64_t)u.bytes[1] << 8) |
                       ((uint64_t)u.bytes[2] << 16) |
                       ((uint64_t)u.bytes[3] << 24) |
                       ((uint64_t)u.bytes[4] << 32) |
                       ((uint64_t)u.bytes[5] << 40) |
                       ((uint64_t)u.bytes[6] << 48) |
                       ((uint64_t)u.bytes[7] << 56);
#endif
    return true;
}
#endif

static bool checkreturn pb_dec_bool(pb_istream_t *stream, const pb_field_iter_t *field)
{
    return pb_decode_bool(stream, (bool*)field->pData);
}

static bool checkreturn pb_dec_varint(pb_istream_t *stream, const pb_field_iter_t *field)
{
    if (PB_LTYPE(field->type) == PB_LTYPE_UVARINT)
    {
        pb_uint64_t value, clamped;
        if (!pb_decode_varint(stream, &value))
            return false;

        /* Cast to the proper field size, while checking for overflows */
        if (field->data_size == sizeof(pb_uint64_t))
            clamped = *(pb_uint64_t*)field->pData = value;
        else if (field->data_size == sizeof(uint32_t))
            clamped = *(uint32_t*)field->pData = (uint32_t)value;
        else if (field->data_size == sizeof(uint_least16_t))
            clamped = *(uint_least16_t*)field->pData = (uint_least16_t)value;
        else if (field->data_size == sizeof(uint_least8_t))
            clamped = *(uint_least8_t*)field->pData = (uint_least8_t)value;
        else
            PB_RETURN_ERROR(stream, "invalid data_size");

        if (clamped != value)
            PB_RETURN_ERROR(stream, "integer too large");

        return true;
    }
    else
    {
        pb_uint64_t value;
        pb_int64_t svalue;
        pb_int64_t clamped;

        if (PB_LTYPE(field->type) == PB_LTYPE_SVARINT)
        {
            if (!pb_decode_svarint(stream, &svalue))
                return false;
        }
        else
        {
            if (!pb_decode_varint(stream, &value))
                return false;

            /* See issue 97: Google's C++ protobuf allows negative varint values to
            * be cast as int32_t, instead of the int64_t that should be used when
            * encoding. Nanopb versions before 0.2.5 had a bug in encoding. In order to
            * not break decoding of such messages, we cast <=32 bit fields to
            * int32_t first to get the sign correct.
            */
            if (field->data_size == sizeof(pb_int64_t))
                svalue = (pb_int64_t)value;
            else
                svalue = (int32_t)value;
        }

        /* Cast to the proper field size, while checking for overflows */
        if (field->data_size == sizeof(pb_int64_t))
            clamped = *(pb_int64_t*)field->pData = svalue;
        else if (field->data_size == sizeof(int32_t))
            clamped = *(int32_t*)field->pData = (int32_t)svalue;
        else if (field->data_size == sizeof(int_least16_t))
            clamped = *(int_least16_t*)field->pData = (int_least16_t)svalue;
        else if (field->data_size == sizeof(int_least8_t))
            clamped = *(int_least8_t*)field->pData = (int_least8_t)svalue;
        else
            PB_RETURN_ERROR(stream, "invalid data_size");

        if (clamped != svalue)
            PB_RETURN_ERROR(stream, "integer too large");

        return true;
    }
}

static bool checkreturn pb_dec_bytes(pb_istream_t *stream, const pb_field_iter_t *field)
{
    uint32_t size;
    size_t alloc_size;
    pb_bytes_array_t *dest;
    
    if (!pb_decode_varint32(stream, &size))
        return false;
    
    if (size > PB_SIZE_MAX)
        PB_RETURN_ERROR(stream, "bytes overflow");
    
    alloc_size = PB_BYTES_ARRAY_T_ALLOCSIZE(size);
    if (size > alloc_size)
        PB_RETURN_ERROR(stream, "size too large");
    
    if (PB_ATYPE(field->type) == PB_ATYPE_POINTER)
    {
#ifndef PB_ENABLE_MALLOC
        PB_RETURN_ERROR(stream, "no malloc support");
#else
        if (stream->bytes_left < size)
            PB_RETURN_ERROR(stream, "end-of-stream");

        if (!allocate_field(stream, field->pData, alloc_size, 1))
            return false;
        dest = *(pb_bytes_array_t**)field->pData;
#endif
    }
    else
    {
        if (alloc_size > field->data_size)
            PB_RETURN_ERROR(stream, "bytes overflow");
        dest = (pb_bytes_array_t*)field->pData;
    }

    dest->size = (pb_size_t)size;
    return pb_read(stream, dest->bytes, (size_t)size);
}

static bool checkreturn pb_dec_string(pb_istream_t *stream, const pb_field_iter_t *field)
{
    uint32_t size;
    size_t alloc_size;
    pb_byte_t *dest = (pb_byte_t*)field->pData;

    if (!pb_decode_varint32(stream, &size))
        return false;

    if (size == (uint32_t)-1)
        PB_RETURN_ERROR(stream, "size too large");

    /* Space for null terminator */
    alloc_size = (size_t)(size + 1);

    if (alloc_size < size)
        PB_RETURN_ERROR(stream, "size too large");

    if (PB_ATYPE(field->type) == PB_ATYPE_POINTER)
    {
#ifndef PB_ENABLE_MALLOC
        PB_RETURN_ERROR(stream, "no malloc support");
#else
        if (stream->bytes_left < size)
            PB_RETURN_ERROR(stream, "end-of-stream");

        if (!allocate_field(stream, field->pData, alloc_size, 1))
            return false;
        dest = *(pb_byte_t**)field->pData;
#endif
    }
    else
    {
        if (alloc_size > field->data_size)
            PB_RETURN_ERROR(stream, "string overflow");
    }
    
    dest[size] = 0;

    if (!pb_read(stream, dest, (size_t)size))
        return false;

#ifdef PB_VALIDATE_UTF8
    if (!pb_validate_utf8((const char*)dest))
        PB_RETURN_ERROR(stream, "invalid utf8");
#endif

    return true;
}

static bool checkreturn pb_dec_submessage(pb_istream_t *stream, const pb_field_iter_t *field)
{
    bool status = true;
    bool submsg_consumed = false;
    pb_istream_t substream;

    if (!pb_make_string_substream(stream, &substream))
        return false;
    
    if (field->submsg_desc == NULL)
        PB_RETURN_ERROR(stream, "invalid field descriptor");
    
    /* Submessages can have a separate message-level callback that is called
     * before decoding the message. Typically it is used to set callback fields
     * inside oneofs. */
    if (PB_LTYPE(field->type) == PB_LTYPE_SUBMSG_W_CB && field->pSize != NULL)
    {
        /* Message callback is stored right before pSize. */
        pb_callback_t *callback = (pb_callback_t*)field->pSize - 1;
        if (callback->funcs.decode)
        {
            status = callback->funcs.decode(&substream, field, &callback->arg);

            if (substream.bytes_left == 0)
            {
                submsg_consumed = true;
            }
        }
    }

    /* Now decode the submessage contents */
    if (status && !submsg_consumed)
    {
        unsigned int flags = 0;

        /* Static required/optional fields are already initialized by top-level
         * pb_decode(), no need to initialize them again. */
        if (PB_ATYPE(field->type) == PB_ATYPE_STATIC &&
            PB_HTYPE(field->type) != PB_HTYPE_REPEATED)
        {
            flags = PB_DECODE_NOINIT;
        }

        status = pb_decode_inner(&substream, field->submsg_desc, field->pData, flags);
    }
    
    if (!pb_close_string_substream(stream, &substream))
        return false;

    return status;
}

static bool checkreturn pb_dec_fixed_length_bytes(pb_istream_t *stream, const pb_field_iter_t *field)
{
    uint32_t size;

    if (!pb_decode_varint32(stream, &size))
        return false;

    if (size > PB_SIZE_MAX)
        PB_RETURN_ERROR(stream, "bytes overflow");

    if (size == 0)
    {
        /* As a special case, treat empty bytes string as all zeros for fixed_length_bytes. */
        memset(field->pData, 0, (size_t)field->data_size);
        return true;
    }

    if (size != field->data_size)
        PB_RETURN_ERROR(stream, "incorrect fixed length bytes size");

    return pb_read(stream, (pb_byte_t*)field->pData, (size_t)field->data_size);
}

#ifdef PB_CONVERT_DOUBLE_FLOAT
bool pb_decode_double_as_float(pb_istream_t *stream, float *dest)
{
    uint_least8_t sign;
    int exponent;
    uint32_t mantissa;
    uint64_t value;
    union { float f; uint32_t i; } out;

    if (!pb_decode_fixed64(stream, &value))
        return false;

    /* Decompose input value */
    sign = (uint_least8_t)((value >> 63) & 1);
    exponent = (int)((value >> 52) & 0x7FF) - 1023;
    mantissa = (value >> 28) & 0xFFFFFF; /* Highest 24 bits */

    /* Figure if value is in range representable by floats. */
    if (exponent == 1024)
    {
        /* Special value */
        exponent = 128;
        mantissa >>= 1;
    }
    else
    {
        if (exponent > 127)
        {
            /* Too large, convert to infinity */
            exponent = 128;
            mantissa = 0;
        }
        else if (exponent < -150)
        {
            /* Too small, convert to zero */
            exponent = -127;
            mantissa = 0;
        }
        else if (exponent < -126)
        {
            /* Denormalized */
            mantissa |= 0x1000000;
            mantissa >>= (-126 - exponent);
            exponent = -127;
        }

        /* Round off mantissa */
        mantissa = (mantissa + 1) >> 1;

        /* Check if mantissa went over 2.0 */
        if (mantissa & 0x800000)
        {
            exponent += 1;
            mantissa &= 0x7FFFFF;
            mantissa >>= 1;
        }
    }

    /* Combine fields */
    out.i = mantissa;
    out.i |= (uint32_t)(exponent + 127) << 23;
    out.i |= (uint32_t)sign << 31;

    *dest = out.f;
    return true;
}
#endif
