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

import java.io.ByteArrayOutputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.WiredTigerPackingException;

/**
 * An internal helper class for encoding WiredTiger packed values.
 *
 * Applications should not need to use this class.
 */
public class PackOutputStream {

    final static int MAX_INT_BYTES = 21;
    protected PackFormatInputStream format;
    protected ByteArrayOutputStream packed;
    protected byte[] intBuf;

    /**
     * Constructor.
     *
     * \param format A String that contains the WiredTiger format that
     *               defines the layout of this packed value.
     */
    public PackOutputStream(String format) {
        this.format = new PackFormatInputStream(format, false);
        intBuf = new byte[MAX_INT_BYTES];
        packed = new ByteArrayOutputStream(100);
    }

    /**
     * Returns the raw packing format string.
     */
    public String getFormat() {
        return format.toString();
    }

    /**
     * Returns the current packed value.
     */
    public byte[] getValue() {
        return packed.toByteArray();
    }

    /**
     * Reset the stream position.
     */
    public void reset() {
        format.reset();
        packed.reset();
    }

    /**
     * Add a byte field to the stream.
     *
     * \param value The byte value to be added.
     */
    public void addByte(byte value)
    throws WiredTigerPackingException {
        format.checkType('b', true);
        /* Translate to maintain ordering with the sign bit. */
        byte input = (byte)(value + 0x80);
        packed.write(input);
    }

    /**
     * Add a byte array field to the stream.
     *
     * \param value The byte array value to be added.
     */
    public void addByteArray(byte[] value)
    throws WiredTigerPackingException {
        this.addByteArray(value, 0, value.length);
    }

    /**
     * Add a byte array field to the stream.
     *
     * \param value The byte array value to be added.
     * \param off The offset from the start of value to begin using the array.
     * \param len The length of the value to encode.
     */
    public void addByteArray(byte[] value, int off, int len)
    throws WiredTigerPackingException {
        int padBytes = 0;

        format.checkType('U', false);
        boolean havesize = format.hasLength();
        char type = format.getType();
        if (havesize) {
            int size = format.getLengthFromFormat(true);
            if (len > size) {
                len = size;
            } else if (size > len) {
                padBytes = size - len;
            }
        }
        // We're done pulling information from the field now.
        format.consume();

        // If this is not the last item and the format does not have the
        // size, or we're using the internal 'U' format, store the size.
        if (!havesize && (format.available() > 0 || type == 'U')) {
            packLong(len, false);
        }
        packed.write(value, off, len);
        while(padBytes-- > 0) {
            packed.write(0);
        }
    }

    /**
     * Add an integer field to the stream.
     *
     * \param value The integer value to be added.
     */
    public void addInt(int value)
    throws WiredTigerPackingException {
        format.checkType('i', true);
        packLong(value, true);
    }

    /**
     * Add a long field to the stream.
     *
     * \param value The long value to be added.
     */
    public void addLong(long value)
    throws WiredTigerPackingException {
        format.checkType('q', true);
        packLong(value, true);
    }

    /**
     * Add a record field to the stream.
     *
     * \param value The record value to be added.
     */
    public void addRecord(long value)
    throws WiredTigerPackingException {
        format.checkType('r', true);
        packLong(value, true);
    }

    /**
     * Add a short field to the stream.
     *
     * \param value The short value to be added.
     */
    public void addShort(short value)
    throws WiredTigerPackingException {
        format.checkType('h', true);
        packLong(value, true);
    }

    /**
     * Add a string field to the stream.
     *
     * \param value The string value to be added.
     */
    public void addString(String value)
    throws WiredTigerPackingException {
        format.checkType('s', false);
        char fieldFormat = format.getType();
        int stringLen = 0;
        int padBytes = 0;
        int valLen = 0;
        // Strings have two possible encodings. A lower case 's' is not null
        // terminated, and has a length define in the format (default 1). An
        // upper case 'S' is variable length and has a null terminator.

        // Logic from python packing.py:
        boolean havesize = format.hasLength();
        int nullpos = value.indexOf('\0');
        int size = 0;

        if (fieldFormat == 'S' && nullpos >= 0) {
            stringLen = nullpos;
        } else {
            stringLen = value.length();
        }
        if (havesize || fieldFormat == 's') {
            size = format.getLengthFromFormat(true);
            if (stringLen > size) {
                stringLen = size;
            }
        }

        if (fieldFormat == 'S' && !havesize) {
            padBytes = 1;
        } else if (size > stringLen) {
            padBytes = size - stringLen;
        }

        // We're done pulling information from the field now.
        format.consume();

        // Use the default Charset.
        packed.write(value.getBytes(), 0, stringLen);
        while(padBytes-- > 0) {
            packed.write(0);
        }
    }

    /**
     * Add a long field to the stream.
     * The packing format is defined in the WiredTiger C integer packing
     * implementation, which is at src/include/intpack.i
     *
     * \param x The long value to be added.
     * \param signed Whether the value is signed or unsigned.
     */
    private void packLong(long x, boolean signed)
    throws WiredTigerPackingException {
        int offset = 0;

        if (!signed && x < 0) {
            throw new WiredTigerPackingException("Overflow packing long.");
        }

        if (x < PackUtil.NEG_2BYTE_MIN) {
            intBuf[offset] = PackUtil.NEG_MULTI_MARKER;
            int lz = Long.numberOfLeadingZeros(~x) / 8;
            int len = PackUtil.SIZEOF_LONG  - lz;

            //
            // There are four size bits we can use in the first
            // byte. For negative numbers, we store the number of
            // leading 0xff byes to maintain ordering (if this is
            // not obvious, it may help to remember that -1 is the
            // largest negative number).
            intBuf[offset++] |= (lz & 0xf);

            for (int shift = (len - 1) << 3;
                    len != 0; shift -= 8, --len) {
                intBuf[offset++] = (byte)(x >> shift);
            }
        } else if (x < PackUtil.NEG_1BYTE_MIN) {
            x -= PackUtil.NEG_2BYTE_MIN;
            intBuf[offset++] =
                (byte)(PackUtil.NEG_2BYTE_MARKER | PackUtil.GET_BITS(x, 13, 8));
            intBuf[offset++] = PackUtil.GET_BITS(x, 8, 0);
        } else if (x < 0) {
            x -= PackUtil.NEG_1BYTE_MIN;
            intBuf[offset++] =
                (byte)(PackUtil.NEG_1BYTE_MARKER | PackUtil.GET_BITS(x, 6, 0));
        } else if (x <= PackUtil.POS_1BYTE_MAX) {
            intBuf[offset++] =
                (byte)(PackUtil.POS_1BYTE_MARKER | PackUtil.GET_BITS(x, 6, 0));
        } else if (x <= PackUtil.POS_2BYTE_MAX) {
            x -= PackUtil.POS_1BYTE_MAX + 1;
            intBuf[offset++] =
                (byte)(PackUtil.POS_2BYTE_MARKER | PackUtil.GET_BITS(x, 13, 8));
            intBuf[offset++] = PackUtil.GET_BITS(x, 8, 0);
        } else if (x == PackUtil.POS_2BYTE_MAX + 1) {
            // This is a special case where we could store the value with
            // just a single byte, but we append a zero byte so that the
            // encoding doesn't get shorter for this one value.
            intBuf[offset++] = (byte)(PackUtil.POS_MULTI_MARKER | 0x01);
            intBuf[offset++] = 0;
        } else {
            x -= PackUtil.POS_2BYTE_MAX + 1;
            intBuf[offset] = PackUtil.POS_MULTI_MARKER;
            int lz = Long.numberOfLeadingZeros(x) / 8;
            int len = PackUtil.SIZEOF_LONG - lz;

            // There are four bits we can use in the first byte.
            intBuf[offset++] |= (len & 0xf);

            for (int shift = (len - 1) << 3;
                    len != 0; --len, shift -= 8) {
                intBuf[offset++] = (byte)(x >> shift);
            }
        }
        packed.write(intBuf, 0, offset);
    }
}
