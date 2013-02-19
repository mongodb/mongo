/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
package com.wiredtiger.db;

import java.lang.String;

/**
 * An internal helper class with utilities for packing and unpacking values.
 *
 * Applications should not need to use this class.
 */
class PackUtil {
    /* Contants. */
    final static byte NEG_MULTI_MARKER = (byte)0x10;
    final static byte NEG_2BYTE_MARKER = (byte)0x20;
    final static byte NEG_1BYTE_MARKER = (byte)0x40;
    final static byte POS_1BYTE_MARKER = (byte)0x80;
    final static byte POS_2BYTE_MARKER = (byte)0xc0;
    final static byte POS_MULTI_MARKER = (byte)0xe0;

    final static int NEG_1BYTE_MIN = ((-1) << 6);
    final static int NEG_2BYTE_MIN = (((-1) << 13) + NEG_1BYTE_MIN);
    final static int POS_1BYTE_MAX = ((1 << 6) - 1);
    final static int POS_2BYTE_MAX = ((1 << 13) + POS_1BYTE_MAX);

    // See: http://docs.python.org/2/library/struct.html for an explanation
    // of what these special characters mean.
    // TODO: Care about byte ordering and padding in packed formats.
    final static String PackSpecialCharacters = "@=<>!x";

    final static int SIZEOF_LONG = 8;

    /**
     * Extract bits from a value, counting from LSB == 0.
     *
     * \param x The value to extract bits from.
     * \param start The first bit to extract.
     * \param end The last bit to extract.
     */
    public static byte GET_BITS(long x, int start, int end) {
        return (byte)((x & ((1 << start) - 1)) >> end);
    }


}
