/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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
package com.wiredtiger.test;

import com.wiredtiger.db.PackOutputStream;
import com.wiredtiger.db.PackInputStream;
import com.wiredtiger.db.WiredTigerPackingException;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

public class PackTest03 {

    public interface PackingAdaptor {
        public void addval(PackOutputStream packer, long val);
        public long getval(PackInputStream unpacker);
    }
    public static class Scenario {
        public Scenario(String format, long low, long high, int nbits,
                        PackingAdaptor a) {
            this.format = format;
            this.low = low;
            this.high = high;
            this.nbits = nbits;
            if (format.equals("Q"))
                this.mask = -1L;
            else if (low == 0) // unsigned
                this.mask = (1L << nbits) - 1;
            else // signed - preserve the sign and lose one of the lower bits
                this.mask = (1L << (nbits - 1)) - 1;
            this.adaptor = a;
        }
        public String format;
        public long low;
        public long high;
        public int nbits;
        public long mask;
        public PackingAdaptor adaptor;
    }
    public static final Scenario scenarios[] = {
        new Scenario("b", -128, 127, 8, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addByte((byte)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getByte();
                }
            }),
        new Scenario("B", 0, 255, 8, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addByte((byte)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getByte();
                }
            }),
        new Scenario("h", -32768, 32767, 16, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addShort((short)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getShort();
                }
            }),
        /*
         * Note: high should be 65535, there's not currently a way to insert
         * unsigned shorts between 2^15 and 2^16.
         */
        new Scenario("H", 0, 32767, 16, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addShort((short)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getShort();
                }
            }),
        new Scenario("i", -2147483648, 2147483647, 32, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addInt((int)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getInt();
                }
            }),
        /*
         * Note: high should be 4294967295L, there's not currently a way to
         * insert unsigned ints between 2^31 and 2^32.
         */
        new Scenario("I", 0, 2147483647L, 32, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addInt((int)val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getInt();
                }
            }),
        new Scenario("q", -9223372036854775808L,
                     9223372036854775807L, 64, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addLong(val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getLong();
                }
            }),

        /* 'unsigned long' numbers larger than 2^63 cannot be
         * expressed in Java (except indirectly as negative numbers).
         */
        new Scenario("Q", 0, -1L, 64, new PackingAdaptor() {
                public void addval(PackOutputStream packer, long val) {
                    packer.addLong(val); }
                public long getval(PackInputStream unpacker) {
                    return unpacker.getLong();
                }
            }),
    };

    void tryPackSingle(Scenario scen, long value)
    throws WiredTigerPackingException {
        if ((value < scen.low || value > scen.high) && scen.low < scen.high)
            throw new IllegalArgumentException("value out of range: " + value);
        
        PackOutputStream packer = new PackOutputStream(scen.format);
        scen.adaptor.addval(packer, value);
        byte[] bytes = packer.getValue();
        PackInputStream unpacker =
            new PackInputStream(scen.format, packer.getValue());
        //long unpacked = scen.adaptor.getval(unpacker) & scen.mask;
        long unpacked = scen.adaptor.getval(unpacker);
        if (scen.low < 0 && unpacked < 0) {
            unpacked = (unpacked & scen.mask) | ~scen.mask;
        }
        else {
            unpacked = unpacked & scen.mask;
        }
        if (value != unpacked) {
            String arg = "ERROR: format " + scen.format + ": wanted=" +
                value + " did not match got=" + unpacked;
            System.out.println(arg);
            throw new IllegalArgumentException(arg);
        }
    }

    void tryPackRange(Scenario scen, long lowRange, long highRange)
    throws WiredTigerPackingException {
        if (lowRange < scen.low)
            lowRange = scen.low;
        if (highRange > scen.high && scen.high != -1L)
            highRange = scen.high;
        for (long val = lowRange; val <= highRange; val++) {
            try {
                tryPackSingle(scen, val);
            }
            catch (Exception ex) {
                System.out.println("EXCEPTION: format " + scen.format +
                                   ": value: " + val + ": " + ex);
                throw ex;
            }
            // watch for long overflow
            if (val == Long.MAX_VALUE)
                break;
        }
    }

    // A debug helper method
    private void printByteArray(byte[] bytes, int len) {
        for (int i = 0; i < len; i++) {
            System.out.println(String.format(
                                   "\t%8s", Integer.toBinaryString(
                                       bytes[i] & 0xff)).replace(' ', '0'));
        }
    }

    @Test
    public void packUnpackRanges()
    throws WiredTigerPackingException {
        // Do a comprehensive test of packing for various formats.
        // Based on test suite's test_intpack.py
        long imin = Integer.MIN_VALUE;
        long imax = Integer.MAX_VALUE;
        long e32 = 1L << 32;
        long lmin = Long.MIN_VALUE;
        long lmax = Long.MAX_VALUE;

        for (int scenidx = 0; scenidx < scenarios.length; scenidx++) {
            Scenario scen = scenarios[scenidx];
            tryPackRange(scen, -100000, 100000);
            tryPackRange(scen, imin - 100, imin + 100);
            tryPackRange(scen, imax - 100, imax + 100);
            tryPackRange(scen, -e32 - 100, -e32 + 100);
            tryPackRange(scen, e32 - 100, e32 + 100);
            tryPackRange(scen, lmin, lmin + 100);
            tryPackRange(scen, lmax - 100, lmax);
        }
    }

    public static void main(String[] args) {
        PackTest03 tester = new PackTest03();
        try {
            tester.packUnpackRanges();
        } catch (WiredTigerPackingException wtpe) {
            System.err.println("Packing exception: " + wtpe);
        }
    }
}
