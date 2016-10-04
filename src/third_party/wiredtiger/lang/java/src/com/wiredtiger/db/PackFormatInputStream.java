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
 * An internal helper class for consuming pack format strings.
 *
 * Applications should not need to use this class.
 */
public class PackFormatInputStream {

    protected String format;
    protected boolean isRaw;
    protected int formatOff;
    protected int formatRepeatCount;

    /**
     * Constructor for a format stream.
     *
     * \param format the encoded format backing string.
     */
    protected PackFormatInputStream(String format, boolean isRaw) {
        this.format = format;
        this.isRaw = isRaw;
        formatOff = 0;
        formatRepeatCount = 0;
    }

    /**
     * Standard toString - returns the string used during construction.
     */
    public String toString() {
        return format;
    }

    /**
     * Returns the approximate count of elements left in the format.
     * This method does not account for repeat counts or string length
     * encodings - so should be used as a guide only.
     */
    public int available() {
        return format.length() - formatOff + formatRepeatCount;
    }

    /**
     * Reset the current stream position.
     */
    public void reset() {
        formatOff = 0;
        formatRepeatCount = 0;
    }

    /**
     * Return the decoded type for the next entry in the format stream. Does
     * not adjust the position of the stream.
     */
    protected char getType()
    throws WiredTigerPackingException {
        if (formatOff >= format.length()) {
            throw new WiredTigerPackingException(
                "No more fields in format.");
        }

        String fieldName;
        boolean lenOK = false;
        int countOff = 0;

        while (PackUtil.PackSpecialCharacters.indexOf(
                    format.charAt(formatOff + countOff)) != -1) {
            countOff++;
        }
        // Skip repeat counts and sizes
        while (Character.isDigit(format.charAt(formatOff + countOff))) {
            countOff++;
        }
        return format.charAt(formatOff + countOff);
    }

    /**
     * Check to see if the next entry is compatible with the requested type.
     *
     * \param asking the format type to match.
     * \param consume indicates whether to update the stream position.
     */
    protected void checkType(char asking, boolean consume)
    throws WiredTigerPackingException {

        char expected = getType();
        if (isRaw)
            throw new WiredTigerPackingException(
                "Format mismatch for raw mode");
        if (Character.toLowerCase(expected) != Character.toLowerCase(asking))
            throw new WiredTigerPackingException(
                "Format mismatch. Wanted: " + asking + ", got: " + expected);
        if (consume) {
            consume();
        }
    }

    /**
     * Move the format stream position ahead one position.
     */
    protected void consume() {
        if (formatRepeatCount > 1) {
            --formatRepeatCount;
        } else if (formatRepeatCount == 1) {
            formatRepeatCount = 0;
            ++formatOff;
        } else {
            while (PackUtil.PackSpecialCharacters.indexOf(
                        format.charAt(formatOff)) != -1) {
                ++formatOff;
            }

            // Don't need to worry about String or byte array size counts
            // since they have already been consumed.
            formatRepeatCount = getIntFromFormat(true);
            if (formatRepeatCount == 0) {
                ++formatOff;
            }
        }
    }

    /**
     * Decode an integer from the format string, return zero if not starting
     * on a digit.
     *
     * \param advance whether to move the stream position.
     */
    private int getIntFromFormat(boolean advance) {
        int valueLen = 0;
        int countOff;
        for (countOff = 0;
                Character.isDigit(format.charAt(formatOff + countOff));
                countOff++) {
            valueLen *= 10;
            valueLen += Character.digit(format.charAt(formatOff + countOff), 10);
        }
        if (advance) {
            formatOff += countOff;
        }
        return valueLen;
    }

    /**
     * Retrieve a length from the format string. Either for a repeat count
     * or a string length. Return one if no explicit repeat count.
     *
     * \param advance whether to move the stream position.
     */
    protected int getLengthFromFormat(boolean advance) {
        int valueLen = getIntFromFormat(advance);
        if (valueLen == 0) {
            valueLen = 1;
        }
        return valueLen;
    }

    /**
     * Return whether there is an explicit length indicated in the format
     * string.
     */
    protected boolean hasLength() {
        return (getIntFromFormat(false) > 0);
    }
}
