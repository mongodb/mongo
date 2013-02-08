package com.wiredtiger.db;

import java.io.ByteArrayInputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.PackUtil;
import com.wiredtiger.db.WiredTigerPackingException;

public class PackOutputStream {

    protected PackFormatOutputStream format;
    protected byte[] value;
    protected int valueOff;
    protected int valueLen;

    public PackOutputStream(String format, byte[] value) {
        this(format, value, 0, value.length);
    }

    public PackOutputStream(String format, byte[] value, int off, int len) {
        this.format = new PackFormatOutputStream(format);
        this.value = value;
        this.valueOff = off;
        this.valueLen = len;
    }

    public String getFormat() {
        return format.toString();
    }

    public byte[] getValue() {
        return value;
    }

    public byte getFieldByte()
        throws WiredTigerPackingException {
        format.checkFieldType('b', false);
        format.consumeField();
        return (byte)(value[valueOff++] - 0x80);
    }

    /*
     * Adds a byte array as part of a complex format (which stores the size in
     * the encoding).
     * Format strings that consist solely of a byte array should use the 
     * addFieldByteArrayRaw method.
     */
    public void getFieldByteArray(byte[] dest)
        throws WiredTigerPackingException {
        this.getFieldByteArray(dest, 0, dest.length);
    }

    public void getFieldByteArray(byte[] dest, int off, int len)
        throws WiredTigerPackingException {
        int itemLen = 0;
        format.checkFieldType('U', false);
        /* The rest of the buffer is a byte array. */
        if (format.getFieldType() == 'U') {
            itemLen = valueLen - valueOff;
        } else {
            /* Check for a 'u' after the length */
            itemLen = format.getLengthFromFormat(true);
        }

        /* TODO: padding. */
        if (itemLen > len)
            itemLen = len;
        format.consumeField();
        System.arraycopy(value, valueOff, dest, off, itemLen);
        valueOff += itemLen;
    }

    public byte[] getFieldByteArray()
        throws WiredTigerPackingException {
        int itemLen;
        if (format.getFieldType() == 'U')
            itemLen = valueLen - valueOff;
        else
            itemLen = format.getLengthFromFormat(false);
        byte[] unpacked = new byte[itemLen];
        getFieldByteArray(unpacked, 0, unpacked.length);
        return unpacked;
    }

    public int getFieldInt()
        throws WiredTigerPackingException {
        boolean signed = false;
        format.checkFieldType('i', false);
        if (format.getFieldType() == 'I' ||
            format.getFieldType() == 'L')
            signed = true;
        format.consumeField();
        return unpackInt(signed);
    }

    public long getFieldLong()
        throws WiredTigerPackingException {
        boolean signed = false;
        format.checkFieldType('q', false);
        if (format.getFieldType() == 'Q')
            signed = true;
        format.consumeField();
        return unpackLong(signed);
    }

    public long getFieldRecord()
        throws WiredTigerPackingException {
        format.checkFieldType('r', false);
        format.consumeField();
        return unpackLong(false);
    }

    public short getFieldShort()
        throws WiredTigerPackingException {
        boolean signed = false;
        format.checkFieldType('h', false);
        if (format.getFieldType() == 'H')
            signed = true;
        format.consumeField();
        return unpackShort(signed);
    }

    public String getFieldString()
        throws WiredTigerPackingException {
        int stringLength = 0;
        format.checkFieldType('S', false);
        // Get the length for a fixed length string
        if (format.getFieldType() != 'S') {
            stringLength = format.getLengthFromFormat(true);
        } else {
            // The string is null terminated, but we need to know how many
            // bytes are consumed - which won't necessarily match up to the
            // string length.
            for (; valueOff + stringLength < value.length &&
                    value[valueOff + stringLength] != 0; stringLength++) {}
        }
        format.consumeField();
        String result = new String(value, valueOff, stringLength);
        valueOff += stringLength;
        return result;
    }

    private short unpackShort(boolean signed)
        throws WiredTigerPackingException {
        long ret = unpackLong(true);
        if ((signed && (ret > Short.MAX_VALUE || ret > Short.MIN_VALUE)) ||
            (!signed && (short)ret < 0))
            throw new WiredTigerPackingException("Overflow unpacking short.");
        return (short)ret;
    }

    private int unpackInt(boolean signed)
        throws WiredTigerPackingException {
        long ret = unpackLong(true);
        if ((signed && (ret > Integer.MAX_VALUE || ret > Integer.MIN_VALUE)) ||
            (!signed && (int)ret < 0))
            throw new WiredTigerPackingException("Overflow unpacking integer.");
        return (int)ret;
    }

    private long unpackLong(boolean signed)
        throws WiredTigerPackingException {
        int len;
        long unpacked = 0;
        switch (value[valueOff] & 0xf0) {
        case PackUtil.NEG_MULTI_MARKER & 0xff:
            len = (int)PackUtil.SIZEOF_LONG - (value[valueOff++] & 0xf);

            for (unpacked = 0xffffffff; len != 0; --len)
                unpacked = (unpacked << 8) | value[valueOff++] & 0xff;
            break;
        case PackUtil.NEG_2BYTE_MARKER & 0xff:
        case (PackUtil.NEG_2BYTE_MARKER | 0x10) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff], 5, 0) << 8;
            unpacked |= value[valueOff + 1] & 0xff;
            unpacked += PackUtil.NEG_2BYTE_MIN;
            break;
        case PackUtil.NEG_1BYTE_MARKER & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x10) & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x20) & 0xff:
        case (PackUtil.NEG_1BYTE_MARKER | 0x30) & 0xff:
            unpacked = PackUtil.NEG_1BYTE_MIN +
                PackUtil.GET_BITS(value[valueOff], 6, 0);
            break;
        case PackUtil.POS_1BYTE_MARKER & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x10) & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x20) & 0xff:
        case (PackUtil.POS_1BYTE_MARKER | 0x30) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff], 6, 0);
            break;
        case PackUtil.POS_2BYTE_MARKER & 0xff:
        case (PackUtil.POS_2BYTE_MARKER | 0x10) & 0xff:
            unpacked = PackUtil.GET_BITS(value[valueOff++], 5, 0) << 8;
            unpacked |= value[valueOff++] & 0xff;
            unpacked += PackUtil.POS_1BYTE_MAX + 1;
            break;
        case PackUtil.POS_MULTI_MARKER & 0xff:
            /* There are four length bits in the first byte. */
            len = (value[valueOff++] & 0xf);

            for (unpacked = 0; len != 0; --len)
                unpacked = (unpacked << 8) | value[valueOff++] & 0xff;
            unpacked += PackUtil.POS_2BYTE_MAX + 1;
            break;
        default:
            throw new WiredTigerPackingException(
                "Error decoding packed value.");
        }
        /* Check for overflow if decoding an unsigned value - since Java only
         * supports signed values.
         */
        if (!signed && unpacked < 0)
            throw new WiredTigerPackingException("Overflow unpacking long.");

        return (unpacked);
    }
}

