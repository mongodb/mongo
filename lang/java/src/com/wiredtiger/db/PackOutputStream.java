package com.wiredtiger.db;

import java.io.ByteArrayOutputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.WiredTigerPackingException;

public class PackOutputStream {

    final static int MAX_INT_BYTES = 21;
    protected PackFormatInputStream format;
    protected ByteArrayOutputStream packed;
    protected byte[] intBuf;

    public PackOutputStream(String format) {
        this.format = new PackFormatInputStream(format);
        intBuf = new byte[MAX_INT_BYTES];
        packed = new ByteArrayOutputStream(100);
    }

    public String getFormat() {
        return format.toString();
    }

    public byte[] getValue() {
        return packed.toByteArray();
    }

    public void reset() {
        format.reset();
        packed.reset();
    }

    public void addFieldByte(byte value)
    throws WiredTigerPackingException {
        format.checkFieldType('b', true);
        /* Translate to maintain ordering with the sign bit. */
        byte input = (byte)(value + 0x80);
        packed.write(input);
    }

    public void addFieldByteArray(byte[] value)
    throws WiredTigerPackingException {
        this.addFieldByteArray(value, 0, value.length);
    }

    public void addFieldByteArray(byte[] value, int off, int len)
    throws WiredTigerPackingException {
        format.checkFieldType('U', true);
        // If this is not the last item, store the size.
        if (format.available() > 0)
            packLong(len, false);

        packed.write(value, off, len);
        /* TODO: padding. */
    }

    public void addFieldInt(int value)
    throws WiredTigerPackingException {
        format.checkFieldType('i', true);
        packLong(value, true);
    }

    public void addFieldLong(long value)
    throws WiredTigerPackingException {
        format.checkFieldType('q', true);
        packLong(value, true);
    }

    public void addFieldRecord(long value)
    throws WiredTigerPackingException {
        format.checkFieldType('r', true);
        packLong(value, true);
    }

    public void addFieldShort(short value)
    throws WiredTigerPackingException {
        format.checkFieldType('h', true);
        packLong(value, true);
    }

    //
    // Strings have two possible encodings. A lower case 's' is not null
    // terminated, and has a length define in the format (default 1). An
    // upper case 'S' is variable length and has a null terminator.
    public void addFieldString(String value)
    throws WiredTigerPackingException {
        format.checkFieldType('s', false);
        char fieldFormat = format.getFieldType();
        int stringLen = 0;
        int padBytes = 0;
        if (fieldFormat == 's') {
            stringLen = format.getLengthFromFormat(true);
            if (stringLen > value.length())
                padBytes = stringLen - value.length();
        } else {
            stringLen = value.length();
            padBytes = 1; // Null terminator
        }
        // We're done pulling information from the field now.
        format.consumeField();

        // Use the default Charset.
        packed.write(value.getBytes(), 0, stringLen);
        while(padBytes-- > 0)
            packed.write(0);
    }

    private void packLong(long x, boolean signed)
    throws WiredTigerPackingException {
        int offset = 0;

        if (!signed && x < 0)
            throw new WiredTigerPackingException("Overflow packing long.");

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
                    len != 0; shift -= 8, --len)
                intBuf[offset++] = (byte)(x >> shift);
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
        } else {
            x -= PackUtil.POS_2BYTE_MAX + 1;
            intBuf[offset] = PackUtil.POS_MULTI_MARKER;
            int lz = Long.numberOfLeadingZeros(x) / 8;
            int len = PackUtil.SIZEOF_LONG - lz;

            // There are four bits we can use in the first byte.
            intBuf[offset++] |= (len & 0xf);

            for (int shift = (len - 1) << 3;
                    len != 0; --len, shift -= 8)
                intBuf[offset++] = (byte)(x >> shift);
        }
        packed.write(intBuf, 0, offset);
    }
}
