package com.wiredtiger.db;

import java.io.ByteArrayInputStream;
import java.lang.StringBuffer;
import com.wiredtiger.db.PackUtil;
import com.wiredtiger.db.WiredTigerPackingException;

public class PackFormatOutputStream {

    protected String format;
    protected int formatOff;
    protected int formatRepeatCount;

    protected PackFormatOutputStream(String format) {
            this.format = format;
            formatOff = 0;
            formatRepeatCount = 0;
    }

    public String toString() {
        return format;
    }

    protected char getFieldType()
        throws WiredTigerPackingException {
        if (formatOff > format.length())
                throw new WiredTigerPackingException(
                    "No more fields in format.");

        String fieldName;
        boolean lenOK = false;
        int countOff = 0;

        while (PackUtil.PackSpecialCharacters.indexOf(
            format.charAt(formatOff + countOff)) != -1)
                countOff++;
        // Skip repeat counts and sizes
        while (Character.isDigit(format.charAt(formatOff + countOff)))
            countOff++;
        return format.charAt(formatOff + countOff);
    }

    protected void checkFieldType(char asking, boolean consume)
        throws WiredTigerPackingException {

        char expected = getFieldType();
        if (Character.toLowerCase(expected) != Character.toLowerCase(asking))
            throw new WiredTigerPackingException(
                "Format mismatch. Wanted: " + asking + ", got: " + expected);
        if (consume)
                consumeField();
    }

    /* Move the format tracker ahead one slot. */
    protected void consumeField() {
        if (formatRepeatCount > 1)
            --formatRepeatCount;
        else if (formatRepeatCount == 1) {
            formatRepeatCount = 0;
            ++formatOff;
        } else {
            while (PackUtil.PackSpecialCharacters.indexOf(
                format.charAt(formatOff)) != -1)
                    ++formatOff;

            // Don't need to worry about String or byte array size counts
            // since they have already been consumed.
            formatRepeatCount = getLengthFromFormat(true);
            if (formatRepeatCount == 0)
                ++formatOff;
        }
    }

    protected int getLengthFromFormat(boolean advance) {
        int valueLen = 0;
        int countOff;
        for (countOff = 0;
            Character.isDigit(format.charAt(formatOff + countOff));
            countOff++) {
            valueLen *= 10;
            valueLen += Character.digit(format.charAt(formatOff + countOff), 10);
        }
        if (advance)
            formatOff += countOff;
        return valueLen;
    }
}

