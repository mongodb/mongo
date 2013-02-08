package com.wiredtiger.db;

import java.io.ByteArrayOutputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.WiredTigerPackingException;

public class PackInputStream {

    final static int MAX_INT_BYTES = 21;
    protected PackFormatOutputStream format;
    protected ByteArrayOutputStream packed;
    protected byte[] intBuf;

    public PackInputStream(String format) {
        this.format = new PackFormatOutputStream(format);
        intBuf = new byte[MAX_INT_BYTES];
        packed = new ByteArrayOutputStream(100);
    }

    public String getFormat() {
        return format.toString();
    }

    public byte[] getValue() {
        return packed.toByteArray();
    }

    public void addFieldByte(byte value)
        throws WiredTigerPackingException {
        format.checkFieldType('b', true);
        /* Translate to maintain ordering with the sign bit. */
        byte input = (byte)(value + 0x80);
        packed.write(input);
    }

    /*
     * Adds a byte array as part of a complex format (which stores the size in
     * the encoding).
     * Format strings that consist solely of a byte array should use the 
     * addFieldByteArrayRaw method.
     */
    public void addFieldByteArray(byte[] value)
        throws WiredTigerPackingException {
        this.addFieldByteArray(value, 0, value.length);
    }

    public void addFieldByteArray(byte[] value, int off, int len)
        throws WiredTigerPackingException {
        format.checkFieldType('U', true);
        packLong(len, false);
        packed.write(value, off, len);
        /* TODO: padding. */
    }

    /*
     * Adds a byte array as the only part of a packed format (which does not
     * store the size in the encoding).
     */
    public void addFieldByteArrayRaw(byte[] value)
        throws WiredTigerPackingException {
        this.addFieldByteArrayRaw(value, 0, value.length);
    }

    public void addFieldByteArrayRaw(byte[] value, int off, int len)
        throws WiredTigerPackingException {
        format.checkFieldType('u', true);
        /* TODO: padding. */
        packed.write(value, off, len);
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

    public void addFieldString(String value)
        throws WiredTigerPackingException {
        format.checkFieldType('S', true);
        /* Use the default Charset. */
        try {
            packed.write(value.getBytes());
        } catch(java.io.IOException ioe) {
            throw new WiredTigerPackingException(
                "Error retrieving content from string.");
        }
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

            /*
             * There are four size bits we can use in the first
             * byte. For negative numbers, we store the number of
             * leading 0xff byes to maintain ordering (if this is
             * not obvious, it may help to remember that -1 is the
             * largest negative number).
             */
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

            /* There are four bits we can use in the first byte. */
            intBuf[offset++] |= (len & 0xf);

            for (int shift = (len - 1) << 3;
                len != 0; --len, shift -= 8)
                intBuf[offset++] = (byte)(x >> shift);
        }
        packed.write(intBuf, 0, offset);
    }
}
