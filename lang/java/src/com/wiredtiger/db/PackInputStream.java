/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
package com.wiredtiger.db;

import java.io.ByteArrayInputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.PackUtil;
import com.wiredtiger.db.WiredTigerPackingException;

/**
 * An internal helper class for decoding WiredTiger packed values.
 *
 * Applications should not need to use this class.
 */
public class PackInputStream {

    protected PackFormatInputStream format;
    protected byte[] value;
    protected int valueOff;
    protected int valueLen;
    protected boolean isRaw;

    /**
     * Constructor.
     *
     * \param format A String that contains the WiredTiger format that
     *               defines the layout of this packed value.
     * \param value The raw bytes that back the stream.
     */
    public PackInputStream(String format, byte[] value) {
        this(format, value, false, 0, value.length);
    }

    /**
     * Constructor.
     *
     * \param format A String that contains the WiredTiger format that
     *               defines the layout of this packed value.
     * \param value The raw bytes that back the stream.
     * \param isRaw The stream is opened raw.
     */
    public PackInputStream(String format, byte[] value, boolean isRaw) {
        this(format, value, isRaw, 0, value.length);
    }

    /**
     * Constructor.
     *
     * \param format A String that contains the WiredTiger format that
     *               defines the layout of this packed value.
     * \param value The raw bytes that back the stream.
     * \param isRaw The stream is opened raw.
     * \param off Offset into the value array at which the stream begins.
     * \param len Length of the value array that forms the stream.
     */
    public PackInputStream(
        String format, byte[] value, boolean isRaw, int off, int len) {
        this.format = new PackFormatInputStream(format, isRaw);
        this.value = value;
        this.valueOff = off;
        this.valueLen = len;
        this.isRaw = isRaw;
    }

    /**
     * Returns the raw packing format string.
     */
    public String getFormat() {
        return format.toString();
    }

    /**
     * Returns the raw value byte array.
     */
    public byte[] getValue() {
        return value;
    }

    /**
     * Retrieves a byte field from the stream.
     */
    public byte getByte()
    throws WiredTigerPackingException {
        format.checkType('b', false);
        format.consume();
        return (byte)(value[valueOff++] - 0x80);
    }

    /**
     * Retrieves a byte array field from the stream.
     *
     * \param dest The byte array where the returned value will be stored. The
     *             array should be large enough to store the entire data item,
     *             if it is not, a truncated value will be returned.
     */
    public void getByteArray(byte[] dest)
    throws WiredTigerPackingException {
        this.getByteArray(dest, 0, dest.length);
    }

    /**
     * Retrieves a byte array field from the stream.
     *
     * \param dest The byte array where the returned value will be stored.
     * \param off Offset into the destination buffer to start copying into.
     * \param len The length should be large enough to store the entire data
     *            item, if it is not, a truncated value will be returned.
     */
    public void getByteArray(byte[] dest, int off, int len)
    throws WiredTigerPackingException {
        if (!isRaw) {
            format.checkType('U', false);
        }
        getByteArrayInternal(getByteArrayLength(), dest, off, len);
    }

    /**
     * Retrieves a byte array field from the stream. Creates a new byte array
     * that is the size of the object being retrieved.
     */
    public byte[] getByteArray()
    throws WiredTigerPackingException {
        if (!isRaw) {
            format.checkType('U', false);
        }
        int itemLen = getByteArrayLength();
        byte[] unpacked = new byte[itemLen];
        getByteArrayInternal(itemLen, unpacked, 0, itemLen);
        return unpacked;
    }

    /**
     * Finds the length of a byte array. Either by decoding the length from
     * the format or using the remaining size of the stream.
     */
    private int getByteArrayLength()
    throws WiredTigerPackingException {
        int itemLen = 0;

        if (isRaw) {
            // The rest of the buffer is a byte array.
            itemLen = valueLen - valueOff;
        } else if (format.hasLength()) {
            // If the format has a length, it's always used.
            itemLen = format.getLengthFromFormat(true);
        } else if (format.getType() == 'U') {
            // The 'U' format is used internally, and may be exposed to us.
            // It indicates that the size is always stored unless there
            // is a size in the format.
            itemLen = unpackInt(false);
        } else if (format.available() == 1) {
            // The rest of the buffer is a byte array.
            itemLen = valueLen - valueOff;
        } else {
            itemLen = unpackInt(false);
        }
        return itemLen;
    }

    /**
     * Do the work of retrieving a byte array.
     */
    private void getByteArrayInternal(
        int itemLen, byte[] dest, int off, int destLen)
    throws WiredTigerPackingException {
        int copyLen = itemLen;
        if (itemLen > destLen) {
            copyLen = destLen;
        }
        format.consume();
        System.arraycopy(value, valueOff, dest, off, copyLen);
        valueOff += itemLen;
    }

    /**
     * Retrieves an integer field from the stream.
     */
    public int getInt()
    throws WiredTigerPackingException {
        boolean signed = true;
        format.checkType('i', false);
        if (format.getType() == 'I' ||
                format.getType() == 'L') {
            signed = false;
        }
        format.consume();
        return unpackInt(signed);
    }

    /**
     * Retrieves a long field from the stream.
     */
    public long getLong()
    throws WiredTigerPackingException {
        boolean signed = true;
        format.checkType('q', false);
        if (format.getType() == 'Q') {
            signed = false;
        }
        format.consume();
        return unpackLong(signed);
    }

    /**
     * Retrieves a record field from the stream.
     */
    public long getRecord()
    throws WiredTigerPackingException {
        format.checkType('r', false);
        format.consume();
        return unpackLong(false);
    }

    /**
     * Retrieves a short field from the stream.
     */
    public short getShort()
    throws WiredTigerPackingException {
        boolean signed = true;
        format.checkType('h', false);
        if (format.getType() == 'H') {
            signed = false;
        }
        format.consume();
        return unpackShort(signed);
    }

    /**
     * Retrieves a string field from the stream.
     */
    public String getString()
    throws WiredTigerPackingException {
        int stringLength = 0;
        int skipnull = 0;
        format.checkType('S', false);
        // Get the length for a fixed length string
        if (format.getType() != 'S') {
            stringLength = format.getLengthFromFormat(true);
        } else {
            // The string is null terminated, but we need to know how many
            // bytes are consumed - which won't necessarily match up to the
            // string length.
            for (; valueOff + stringLength < value.length &&
                    value[valueOff + stringLength] != 0; stringLength++) {}
            skipnull = 1;
        }
        format.consume();
        String result = new String(value, valueOff, stringLength);
        valueOff += stringLength + skipnull;
        return result;
    }

    /**
     * Decodes an encoded short from the stream. This method does bounds
     * checking, to ensure values fit, since some values may be encoded as
     * unsigned values, and Java types are all signed.
     */
    private short unpackShort(boolean signed)
    throws WiredTigerPackingException {
        long ret = unpackLong(true);
        if ((signed && (ret > Short.MAX_VALUE || ret < Short.MIN_VALUE)) ||
                (!signed && (short)ret < 0)) {
            throw new WiredTigerPackingException("Overflow unpacking short.");
        }
        return (short)ret;
    }

    /**
     * Decodes an encoded integer from the stream. This method does bounds
     * checking, to ensure values fit, since some values may be encoded as
     * unsigned values, and Java types are all signed.
     */
    private int unpackInt(boolean signed)
    throws WiredTigerPackingException {
        long ret = unpackLong(true);
        if ((signed && (ret > Integer.MAX_VALUE || ret < Integer.MIN_VALUE)) ||
                (!signed && (int)ret < 0)) {
            throw new WiredTigerPackingException("Overflow unpacking integer.");
        }
        return (int)ret;
    }

    /**
     * Decodes an encoded long from the stream. This method does bounds
     * checking, to ensure values fit, since some values may be encoded as
     * unsigned values, and Java types are all signed.
     * The packing format is defined in the WiredTiger C integer packing
     * implementation, which is at src/include/intpack.i
     */
    private long unpackLong(boolean signed)
    throws WiredTigerPackingException {
        int len;
        long unpacked = 0;
        switch (value[valueOff] & 0xf0) {
        case PackUtil.NEG_MULTI_MARKER & 0xff:
            len = (int)PackUtil.SIZEOF_LONG - (value[valueOff++] & 0xf);

            for (unpacked = 0xffffffff; len != 0; --len) {
                unpacked = (unpacked << 8) | value[valueOff++] & 0xff;
            }
            break;
        case PackUtil.NEG_2BYTE_MARKER & 0xff:
        case (PackUtil.NEG_2BYTE_MARKER | 0x10) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff++], 5, 0) << 8;
            unpacked |= value[valueOff++] & 0xff;
            unpacked += PackUtil.NEG_2BYTE_MIN;
            break;
        case PackUtil.NEG_1BYTE_MARKER & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x10) & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x20) & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x30) & 0xff:
            unpacked = PackUtil.NEG_1BYTE_MIN +
                       PackUtil.GET_BITS(value[valueOff++], 6, 0);
            break;
        case PackUtil.POS_1BYTE_MARKER & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x10) & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x20) & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x30) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff++], 6, 0);
            break;
        case PackUtil.POS_2BYTE_MARKER & 0xff:
        case (PackUtil.POS_2BYTE_MARKER | 0x10) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff++], 5, 0) << 8;
            unpacked |= value[valueOff++] & 0xff;
            unpacked += PackUtil.POS_1BYTE_MAX + 1;
            break;
        case PackUtil.POS_MULTI_MARKER & 0xff:
            // There are four length bits in the first byte.
            len = (value[valueOff++] & 0xf);

            for (unpacked = 0; len != 0; --len) {
                unpacked = (unpacked << 8) | value[valueOff++] & 0xff;
            }
            unpacked += PackUtil.POS_2BYTE_MAX + 1;
            break;
        default:
            throw new WiredTigerPackingException(
                "Error decoding packed value.");
        }
        // Check for overflow if decoding an unsigned value - since Java only
        // supports signed values.
        if (!signed && unpacked < 0) {
            throw new WiredTigerPackingException("Overflow unpacking long.");
        }

        return (unpacked);
    }
}

