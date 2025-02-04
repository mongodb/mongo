/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* WARNING: THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT EDIT. */
/* clang-format off */

#include <aws/compression/huffman.h>

static struct aws_huffman_code code_points[] = {
    { .pattern = 0x1ff8, .num_bits = 13 }, /* ' ' 0 */
    { .pattern = 0x7fffd8, .num_bits = 23 }, /* ' ' 1 */
    { .pattern = 0xfffffe2, .num_bits = 28 }, /* ' ' 2 */
    { .pattern = 0xfffffe3, .num_bits = 28 }, /* ' ' 3 */
    { .pattern = 0xfffffe4, .num_bits = 28 }, /* ' ' 4 */
    { .pattern = 0xfffffe5, .num_bits = 28 }, /* ' ' 5 */
    { .pattern = 0xfffffe6, .num_bits = 28 }, /* ' ' 6 */
    { .pattern = 0xfffffe7, .num_bits = 28 }, /* ' ' 7 */
    { .pattern = 0xfffffe8, .num_bits = 28 }, /* ' ' 8 */
    { .pattern = 0xffffea, .num_bits = 24 }, /* ' ' 9 */
    { .pattern = 0x3ffffffc, .num_bits = 30 }, /* ' ' 10 */
    { .pattern = 0xfffffe9, .num_bits = 28 }, /* ' ' 11 */
    { .pattern = 0xfffffea, .num_bits = 28 }, /* ' ' 12 */
    { .pattern = 0x3ffffffd, .num_bits = 30 }, /* ' ' 13 */
    { .pattern = 0xfffffeb, .num_bits = 28 }, /* ' ' 14 */
    { .pattern = 0xfffffec, .num_bits = 28 }, /* ' ' 15 */
    { .pattern = 0xfffffed, .num_bits = 28 }, /* ' ' 16 */
    { .pattern = 0xfffffee, .num_bits = 28 }, /* ' ' 17 */
    { .pattern = 0xfffffef, .num_bits = 28 }, /* ' ' 18 */
    { .pattern = 0xffffff0, .num_bits = 28 }, /* ' ' 19 */
    { .pattern = 0xffffff1, .num_bits = 28 }, /* ' ' 20 */
    { .pattern = 0xffffff2, .num_bits = 28 }, /* ' ' 21 */
    { .pattern = 0x3ffffffe, .num_bits = 30 }, /* ' ' 22 */
    { .pattern = 0xffffff3, .num_bits = 28 }, /* ' ' 23 */
    { .pattern = 0xffffff4, .num_bits = 28 }, /* ' ' 24 */
    { .pattern = 0xffffff5, .num_bits = 28 }, /* ' ' 25 */
    { .pattern = 0xffffff6, .num_bits = 28 }, /* ' ' 26 */
    { .pattern = 0xffffff7, .num_bits = 28 }, /* ' ' 27 */
    { .pattern = 0xffffff8, .num_bits = 28 }, /* ' ' 28 */
    { .pattern = 0xffffff9, .num_bits = 28 }, /* ' ' 29 */
    { .pattern = 0xffffffa, .num_bits = 28 }, /* ' ' 30 */
    { .pattern = 0xffffffb, .num_bits = 28 }, /* ' ' 31 */
    { .pattern = 0x14, .num_bits = 6 }, /* ' ' 32 */
    { .pattern = 0x3f8, .num_bits = 10 }, /* '!' 33 */
    { .pattern = 0x3f9, .num_bits = 10 }, /* '"' 34 */
    { .pattern = 0xffa, .num_bits = 12 }, /* '#' 35 */
    { .pattern = 0x1ff9, .num_bits = 13 }, /* '$' 36 */
    { .pattern = 0x15, .num_bits = 6 }, /* '%' 37 */
    { .pattern = 0xf8, .num_bits = 8 }, /* '&' 38 */
    { .pattern = 0x7fa, .num_bits = 11 }, /* ''' 39 */
    { .pattern = 0x3fa, .num_bits = 10 }, /* '(' 40 */
    { .pattern = 0x3fb, .num_bits = 10 }, /* ')' 41 */
    { .pattern = 0xf9, .num_bits = 8 }, /* '*' 42 */
    { .pattern = 0x7fb, .num_bits = 11 }, /* '+' 43 */
    { .pattern = 0xfa, .num_bits = 8 }, /* ',' 44 */
    { .pattern = 0x16, .num_bits = 6 }, /* '-' 45 */
    { .pattern = 0x17, .num_bits = 6 }, /* '.' 46 */
    { .pattern = 0x18, .num_bits = 6 }, /* '/' 47 */
    { .pattern = 0x0, .num_bits = 5 }, /* '0' 48 */
    { .pattern = 0x1, .num_bits = 5 }, /* '1' 49 */
    { .pattern = 0x2, .num_bits = 5 }, /* '2' 50 */
    { .pattern = 0x19, .num_bits = 6 }, /* '3' 51 */
    { .pattern = 0x1a, .num_bits = 6 }, /* '4' 52 */
    { .pattern = 0x1b, .num_bits = 6 }, /* '5' 53 */
    { .pattern = 0x1c, .num_bits = 6 }, /* '6' 54 */
    { .pattern = 0x1d, .num_bits = 6 }, /* '7' 55 */
    { .pattern = 0x1e, .num_bits = 6 }, /* '8' 56 */
    { .pattern = 0x1f, .num_bits = 6 }, /* '9' 57 */
    { .pattern = 0x5c, .num_bits = 7 }, /* ':' 58 */
    { .pattern = 0xfb, .num_bits = 8 }, /* ';' 59 */
    { .pattern = 0x7ffc, .num_bits = 15 }, /* '<' 60 */
    { .pattern = 0x20, .num_bits = 6 }, /* '=' 61 */
    { .pattern = 0xffb, .num_bits = 12 }, /* '>' 62 */
    { .pattern = 0x3fc, .num_bits = 10 }, /* '?' 63 */
    { .pattern = 0x1ffa, .num_bits = 13 }, /* '@' 64 */
    { .pattern = 0x21, .num_bits = 6 }, /* 'A' 65 */
    { .pattern = 0x5d, .num_bits = 7 }, /* 'B' 66 */
    { .pattern = 0x5e, .num_bits = 7 }, /* 'C' 67 */
    { .pattern = 0x5f, .num_bits = 7 }, /* 'D' 68 */
    { .pattern = 0x60, .num_bits = 7 }, /* 'E' 69 */
    { .pattern = 0x61, .num_bits = 7 }, /* 'F' 70 */
    { .pattern = 0x62, .num_bits = 7 }, /* 'G' 71 */
    { .pattern = 0x63, .num_bits = 7 }, /* 'H' 72 */
    { .pattern = 0x64, .num_bits = 7 }, /* 'I' 73 */
    { .pattern = 0x65, .num_bits = 7 }, /* 'J' 74 */
    { .pattern = 0x66, .num_bits = 7 }, /* 'K' 75 */
    { .pattern = 0x67, .num_bits = 7 }, /* 'L' 76 */
    { .pattern = 0x68, .num_bits = 7 }, /* 'M' 77 */
    { .pattern = 0x69, .num_bits = 7 }, /* 'N' 78 */
    { .pattern = 0x6a, .num_bits = 7 }, /* 'O' 79 */
    { .pattern = 0x6b, .num_bits = 7 }, /* 'P' 80 */
    { .pattern = 0x6c, .num_bits = 7 }, /* 'Q' 81 */
    { .pattern = 0x6d, .num_bits = 7 }, /* 'R' 82 */
    { .pattern = 0x6e, .num_bits = 7 }, /* 'S' 83 */
    { .pattern = 0x6f, .num_bits = 7 }, /* 'T' 84 */
    { .pattern = 0x70, .num_bits = 7 }, /* 'U' 85 */
    { .pattern = 0x71, .num_bits = 7 }, /* 'V' 86 */
    { .pattern = 0x72, .num_bits = 7 }, /* 'W' 87 */
    { .pattern = 0xfc, .num_bits = 8 }, /* 'X' 88 */
    { .pattern = 0x73, .num_bits = 7 }, /* 'Y' 89 */
    { .pattern = 0xfd, .num_bits = 8 }, /* 'Z' 90 */
    { .pattern = 0x1ffb, .num_bits = 13 }, /* '[' 91 */
    { .pattern = 0x7fff0, .num_bits = 19 }, /* '\' 92 */
    { .pattern = 0x1ffc, .num_bits = 13 }, /* ']' 93 */
    { .pattern = 0x3ffc, .num_bits = 14 }, /* '^' 94 */
    { .pattern = 0x22, .num_bits = 6 }, /* '_' 95 */
    { .pattern = 0x7ffd, .num_bits = 15 }, /* '`' 96 */
    { .pattern = 0x3, .num_bits = 5 }, /* 'a' 97 */
    { .pattern = 0x23, .num_bits = 6 }, /* 'b' 98 */
    { .pattern = 0x4, .num_bits = 5 }, /* 'c' 99 */
    { .pattern = 0x24, .num_bits = 6 }, /* 'd' 100 */
    { .pattern = 0x5, .num_bits = 5 }, /* 'e' 101 */
    { .pattern = 0x25, .num_bits = 6 }, /* 'f' 102 */
    { .pattern = 0x26, .num_bits = 6 }, /* 'g' 103 */
    { .pattern = 0x27, .num_bits = 6 }, /* 'h' 104 */
    { .pattern = 0x6, .num_bits = 5 }, /* 'i' 105 */
    { .pattern = 0x74, .num_bits = 7 }, /* 'j' 106 */
    { .pattern = 0x75, .num_bits = 7 }, /* 'k' 107 */
    { .pattern = 0x28, .num_bits = 6 }, /* 'l' 108 */
    { .pattern = 0x29, .num_bits = 6 }, /* 'm' 109 */
    { .pattern = 0x2a, .num_bits = 6 }, /* 'n' 110 */
    { .pattern = 0x7, .num_bits = 5 }, /* 'o' 111 */
    { .pattern = 0x2b, .num_bits = 6 }, /* 'p' 112 */
    { .pattern = 0x76, .num_bits = 7 }, /* 'q' 113 */
    { .pattern = 0x2c, .num_bits = 6 }, /* 'r' 114 */
    { .pattern = 0x8, .num_bits = 5 }, /* 's' 115 */
    { .pattern = 0x9, .num_bits = 5 }, /* 't' 116 */
    { .pattern = 0x2d, .num_bits = 6 }, /* 'u' 117 */
    { .pattern = 0x77, .num_bits = 7 }, /* 'v' 118 */
    { .pattern = 0x78, .num_bits = 7 }, /* 'w' 119 */
    { .pattern = 0x79, .num_bits = 7 }, /* 'x' 120 */
    { .pattern = 0x7a, .num_bits = 7 }, /* 'y' 121 */
    { .pattern = 0x7b, .num_bits = 7 }, /* 'z' 122 */
    { .pattern = 0x7ffe, .num_bits = 15 }, /* '{' 123 */
    { .pattern = 0x7fc, .num_bits = 11 }, /* '|' 124 */
    { .pattern = 0x3ffd, .num_bits = 14 }, /* '}' 125 */
    { .pattern = 0x1ffd, .num_bits = 13 }, /* '~' 126 */
    { .pattern = 0xffffffc, .num_bits = 28 }, /* ' ' 127 */
    { .pattern = 0xfffe6, .num_bits = 20 }, /* ' ' 128 */
    { .pattern = 0x3fffd2, .num_bits = 22 }, /* ' ' 129 */
    { .pattern = 0xfffe7, .num_bits = 20 }, /* ' ' 130 */
    { .pattern = 0xfffe8, .num_bits = 20 }, /* ' ' 131 */
    { .pattern = 0x3fffd3, .num_bits = 22 }, /* ' ' 132 */
    { .pattern = 0x3fffd4, .num_bits = 22 }, /* ' ' 133 */
    { .pattern = 0x3fffd5, .num_bits = 22 }, /* ' ' 134 */
    { .pattern = 0x7fffd9, .num_bits = 23 }, /* ' ' 135 */
    { .pattern = 0x3fffd6, .num_bits = 22 }, /* ' ' 136 */
    { .pattern = 0x7fffda, .num_bits = 23 }, /* ' ' 137 */
    { .pattern = 0x7fffdb, .num_bits = 23 }, /* ' ' 138 */
    { .pattern = 0x7fffdc, .num_bits = 23 }, /* ' ' 139 */
    { .pattern = 0x7fffdd, .num_bits = 23 }, /* ' ' 140 */
    { .pattern = 0x7fffde, .num_bits = 23 }, /* ' ' 141 */
    { .pattern = 0xffffeb, .num_bits = 24 }, /* ' ' 142 */
    { .pattern = 0x7fffdf, .num_bits = 23 }, /* ' ' 143 */
    { .pattern = 0xffffec, .num_bits = 24 }, /* ' ' 144 */
    { .pattern = 0xffffed, .num_bits = 24 }, /* ' ' 145 */
    { .pattern = 0x3fffd7, .num_bits = 22 }, /* ' ' 146 */
    { .pattern = 0x7fffe0, .num_bits = 23 }, /* ' ' 147 */
    { .pattern = 0xffffee, .num_bits = 24 }, /* ' ' 148 */
    { .pattern = 0x7fffe1, .num_bits = 23 }, /* ' ' 149 */
    { .pattern = 0x7fffe2, .num_bits = 23 }, /* ' ' 150 */
    { .pattern = 0x7fffe3, .num_bits = 23 }, /* ' ' 151 */
    { .pattern = 0x7fffe4, .num_bits = 23 }, /* ' ' 152 */
    { .pattern = 0x1fffdc, .num_bits = 21 }, /* ' ' 153 */
    { .pattern = 0x3fffd8, .num_bits = 22 }, /* ' ' 154 */
    { .pattern = 0x7fffe5, .num_bits = 23 }, /* ' ' 155 */
    { .pattern = 0x3fffd9, .num_bits = 22 }, /* ' ' 156 */
    { .pattern = 0x7fffe6, .num_bits = 23 }, /* ' ' 157 */
    { .pattern = 0x7fffe7, .num_bits = 23 }, /* ' ' 158 */
    { .pattern = 0xffffef, .num_bits = 24 }, /* ' ' 159 */
    { .pattern = 0x3fffda, .num_bits = 22 }, /* ' ' 160 */
    { .pattern = 0x1fffdd, .num_bits = 21 }, /* ' ' 161 */
    { .pattern = 0xfffe9, .num_bits = 20 }, /* ' ' 162 */
    { .pattern = 0x3fffdb, .num_bits = 22 }, /* ' ' 163 */
    { .pattern = 0x3fffdc, .num_bits = 22 }, /* ' ' 164 */
    { .pattern = 0x7fffe8, .num_bits = 23 }, /* ' ' 165 */
    { .pattern = 0x7fffe9, .num_bits = 23 }, /* ' ' 166 */
    { .pattern = 0x1fffde, .num_bits = 21 }, /* ' ' 167 */
    { .pattern = 0x7fffea, .num_bits = 23 }, /* ' ' 168 */
    { .pattern = 0x3fffdd, .num_bits = 22 }, /* ' ' 169 */
    { .pattern = 0x3fffde, .num_bits = 22 }, /* ' ' 170 */
    { .pattern = 0xfffff0, .num_bits = 24 }, /* ' ' 171 */
    { .pattern = 0x1fffdf, .num_bits = 21 }, /* ' ' 172 */
    { .pattern = 0x3fffdf, .num_bits = 22 }, /* ' ' 173 */
    { .pattern = 0x7fffeb, .num_bits = 23 }, /* ' ' 174 */
    { .pattern = 0x7fffec, .num_bits = 23 }, /* ' ' 175 */
    { .pattern = 0x1fffe0, .num_bits = 21 }, /* ' ' 176 */
    { .pattern = 0x1fffe1, .num_bits = 21 }, /* ' ' 177 */
    { .pattern = 0x3fffe0, .num_bits = 22 }, /* ' ' 178 */
    { .pattern = 0x1fffe2, .num_bits = 21 }, /* ' ' 179 */
    { .pattern = 0x7fffed, .num_bits = 23 }, /* ' ' 180 */
    { .pattern = 0x3fffe1, .num_bits = 22 }, /* ' ' 181 */
    { .pattern = 0x7fffee, .num_bits = 23 }, /* ' ' 182 */
    { .pattern = 0x7fffef, .num_bits = 23 }, /* ' ' 183 */
    { .pattern = 0xfffea, .num_bits = 20 }, /* ' ' 184 */
    { .pattern = 0x3fffe2, .num_bits = 22 }, /* ' ' 185 */
    { .pattern = 0x3fffe3, .num_bits = 22 }, /* ' ' 186 */
    { .pattern = 0x3fffe4, .num_bits = 22 }, /* ' ' 187 */
    { .pattern = 0x7ffff0, .num_bits = 23 }, /* ' ' 188 */
    { .pattern = 0x3fffe5, .num_bits = 22 }, /* ' ' 189 */
    { .pattern = 0x3fffe6, .num_bits = 22 }, /* ' ' 190 */
    { .pattern = 0x7ffff1, .num_bits = 23 }, /* ' ' 191 */
    { .pattern = 0x3ffffe0, .num_bits = 26 }, /* ' ' 192 */
    { .pattern = 0x3ffffe1, .num_bits = 26 }, /* ' ' 193 */
    { .pattern = 0xfffeb, .num_bits = 20 }, /* ' ' 194 */
    { .pattern = 0x7fff1, .num_bits = 19 }, /* ' ' 195 */
    { .pattern = 0x3fffe7, .num_bits = 22 }, /* ' ' 196 */
    { .pattern = 0x7ffff2, .num_bits = 23 }, /* ' ' 197 */
    { .pattern = 0x3fffe8, .num_bits = 22 }, /* ' ' 198 */
    { .pattern = 0x1ffffec, .num_bits = 25 }, /* ' ' 199 */
    { .pattern = 0x3ffffe2, .num_bits = 26 }, /* ' ' 200 */
    { .pattern = 0x3ffffe3, .num_bits = 26 }, /* ' ' 201 */
    { .pattern = 0x3ffffe4, .num_bits = 26 }, /* ' ' 202 */
    { .pattern = 0x7ffffde, .num_bits = 27 }, /* ' ' 203 */
    { .pattern = 0x7ffffdf, .num_bits = 27 }, /* ' ' 204 */
    { .pattern = 0x3ffffe5, .num_bits = 26 }, /* ' ' 205 */
    { .pattern = 0xfffff1, .num_bits = 24 }, /* ' ' 206 */
    { .pattern = 0x1ffffed, .num_bits = 25 }, /* ' ' 207 */
    { .pattern = 0x7fff2, .num_bits = 19 }, /* ' ' 208 */
    { .pattern = 0x1fffe3, .num_bits = 21 }, /* ' ' 209 */
    { .pattern = 0x3ffffe6, .num_bits = 26 }, /* ' ' 210 */
    { .pattern = 0x7ffffe0, .num_bits = 27 }, /* ' ' 211 */
    { .pattern = 0x7ffffe1, .num_bits = 27 }, /* ' ' 212 */
    { .pattern = 0x3ffffe7, .num_bits = 26 }, /* ' ' 213 */
    { .pattern = 0x7ffffe2, .num_bits = 27 }, /* ' ' 214 */
    { .pattern = 0xfffff2, .num_bits = 24 }, /* ' ' 215 */
    { .pattern = 0x1fffe4, .num_bits = 21 }, /* ' ' 216 */
    { .pattern = 0x1fffe5, .num_bits = 21 }, /* ' ' 217 */
    { .pattern = 0x3ffffe8, .num_bits = 26 }, /* ' ' 218 */
    { .pattern = 0x3ffffe9, .num_bits = 26 }, /* ' ' 219 */
    { .pattern = 0xffffffd, .num_bits = 28 }, /* ' ' 220 */
    { .pattern = 0x7ffffe3, .num_bits = 27 }, /* ' ' 221 */
    { .pattern = 0x7ffffe4, .num_bits = 27 }, /* ' ' 222 */
    { .pattern = 0x7ffffe5, .num_bits = 27 }, /* ' ' 223 */
    { .pattern = 0xfffec, .num_bits = 20 }, /* ' ' 224 */
    { .pattern = 0xfffff3, .num_bits = 24 }, /* ' ' 225 */
    { .pattern = 0xfffed, .num_bits = 20 }, /* ' ' 226 */
    { .pattern = 0x1fffe6, .num_bits = 21 }, /* ' ' 227 */
    { .pattern = 0x3fffe9, .num_bits = 22 }, /* ' ' 228 */
    { .pattern = 0x1fffe7, .num_bits = 21 }, /* ' ' 229 */
    { .pattern = 0x1fffe8, .num_bits = 21 }, /* ' ' 230 */
    { .pattern = 0x7ffff3, .num_bits = 23 }, /* ' ' 231 */
    { .pattern = 0x3fffea, .num_bits = 22 }, /* ' ' 232 */
    { .pattern = 0x3fffeb, .num_bits = 22 }, /* ' ' 233 */
    { .pattern = 0x1ffffee, .num_bits = 25 }, /* ' ' 234 */
    { .pattern = 0x1ffffef, .num_bits = 25 }, /* ' ' 235 */
    { .pattern = 0xfffff4, .num_bits = 24 }, /* ' ' 236 */
    { .pattern = 0xfffff5, .num_bits = 24 }, /* ' ' 237 */
    { .pattern = 0x3ffffea, .num_bits = 26 }, /* ' ' 238 */
    { .pattern = 0x7ffff4, .num_bits = 23 }, /* ' ' 239 */
    { .pattern = 0x3ffffeb, .num_bits = 26 }, /* ' ' 240 */
    { .pattern = 0x7ffffe6, .num_bits = 27 }, /* ' ' 241 */
    { .pattern = 0x3ffffec, .num_bits = 26 }, /* ' ' 242 */
    { .pattern = 0x3ffffed, .num_bits = 26 }, /* ' ' 243 */
    { .pattern = 0x7ffffe7, .num_bits = 27 }, /* ' ' 244 */
    { .pattern = 0x7ffffe8, .num_bits = 27 }, /* ' ' 245 */
    { .pattern = 0x7ffffe9, .num_bits = 27 }, /* ' ' 246 */
    { .pattern = 0x7ffffea, .num_bits = 27 }, /* ' ' 247 */
    { .pattern = 0x7ffffeb, .num_bits = 27 }, /* ' ' 248 */
    { .pattern = 0xffffffe, .num_bits = 28 }, /* ' ' 249 */
    { .pattern = 0x7ffffec, .num_bits = 27 }, /* ' ' 250 */
    { .pattern = 0x7ffffed, .num_bits = 27 }, /* ' ' 251 */
    { .pattern = 0x7ffffee, .num_bits = 27 }, /* ' ' 252 */
    { .pattern = 0x7ffffef, .num_bits = 27 }, /* ' ' 253 */
    { .pattern = 0x7fffff0, .num_bits = 27 }, /* ' ' 254 */
    { .pattern = 0x3ffffee, .num_bits = 26 }, /* ' ' 255 */
};

static struct aws_huffman_code encode_symbol(uint8_t symbol, void *userdata) {
    (void)userdata;

    return code_points[symbol];
}

/* NOLINTNEXTLINE(readability-function-size) */
static uint8_t decode_symbol(uint32_t bits, uint8_t *symbol, void *userdata) {
    (void)userdata;

    if (bits & 0x80000000) {
        goto node_1;
    } else {
        goto node_0;
    }

node_0:
    if (bits & 0x40000000) {
        goto node_01;
    } else {
        goto node_00;
    }

node_00:
    if (bits & 0x20000000) {
        goto node_001;
    } else {
        goto node_000;
    }

node_000:
    if (bits & 0x10000000) {
        goto node_0001;
    } else {
        goto node_0000;
    }

node_0000:
    if (bits & 0x8000000) {
        *symbol = 49;
        return 5;
    } else {
        *symbol = 48;
        return 5;
    }

node_0001:
    if (bits & 0x8000000) {
        *symbol = 97;
        return 5;
    } else {
        *symbol = 50;
        return 5;
    }

node_001:
    if (bits & 0x10000000) {
        goto node_0011;
    } else {
        goto node_0010;
    }

node_0010:
    if (bits & 0x8000000) {
        *symbol = 101;
        return 5;
    } else {
        *symbol = 99;
        return 5;
    }

node_0011:
    if (bits & 0x8000000) {
        *symbol = 111;
        return 5;
    } else {
        *symbol = 105;
        return 5;
    }

node_01:
    if (bits & 0x20000000) {
        goto node_011;
    } else {
        goto node_010;
    }

node_010:
    if (bits & 0x10000000) {
        goto node_0101;
    } else {
        goto node_0100;
    }

node_0100:
    if (bits & 0x8000000) {
        *symbol = 116;
        return 5;
    } else {
        *symbol = 115;
        return 5;
    }

node_0101:
    if (bits & 0x8000000) {
        goto node_01011;
    } else {
        goto node_01010;
    }

node_01010:
    if (bits & 0x4000000) {
        *symbol = 37;
        return 6;
    } else {
        *symbol = 32;
        return 6;
    }

node_01011:
    if (bits & 0x4000000) {
        *symbol = 46;
        return 6;
    } else {
        *symbol = 45;
        return 6;
    }

node_011:
    if (bits & 0x10000000) {
        goto node_0111;
    } else {
        goto node_0110;
    }

node_0110:
    if (bits & 0x8000000) {
        goto node_01101;
    } else {
        goto node_01100;
    }

node_01100:
    if (bits & 0x4000000) {
        *symbol = 51;
        return 6;
    } else {
        *symbol = 47;
        return 6;
    }

node_01101:
    if (bits & 0x4000000) {
        *symbol = 53;
        return 6;
    } else {
        *symbol = 52;
        return 6;
    }

node_0111:
    if (bits & 0x8000000) {
        goto node_01111;
    } else {
        goto node_01110;
    }

node_01110:
    if (bits & 0x4000000) {
        *symbol = 55;
        return 6;
    } else {
        *symbol = 54;
        return 6;
    }

node_01111:
    if (bits & 0x4000000) {
        *symbol = 57;
        return 6;
    } else {
        *symbol = 56;
        return 6;
    }

node_1:
    if (bits & 0x40000000) {
        goto node_11;
    } else {
        goto node_10;
    }

node_10:
    if (bits & 0x20000000) {
        goto node_101;
    } else {
        goto node_100;
    }

node_100:
    if (bits & 0x10000000) {
        goto node_1001;
    } else {
        goto node_1000;
    }

node_1000:
    if (bits & 0x8000000) {
        goto node_10001;
    } else {
        goto node_10000;
    }

node_10000:
    if (bits & 0x4000000) {
        *symbol = 65;
        return 6;
    } else {
        *symbol = 61;
        return 6;
    }

node_10001:
    if (bits & 0x4000000) {
        *symbol = 98;
        return 6;
    } else {
        *symbol = 95;
        return 6;
    }

node_1001:
    if (bits & 0x8000000) {
        goto node_10011;
    } else {
        goto node_10010;
    }

node_10010:
    if (bits & 0x4000000) {
        *symbol = 102;
        return 6;
    } else {
        *symbol = 100;
        return 6;
    }

node_10011:
    if (bits & 0x4000000) {
        *symbol = 104;
        return 6;
    } else {
        *symbol = 103;
        return 6;
    }

node_101:
    if (bits & 0x10000000) {
        goto node_1011;
    } else {
        goto node_1010;
    }

node_1010:
    if (bits & 0x8000000) {
        goto node_10101;
    } else {
        goto node_10100;
    }

node_10100:
    if (bits & 0x4000000) {
        *symbol = 109;
        return 6;
    } else {
        *symbol = 108;
        return 6;
    }

node_10101:
    if (bits & 0x4000000) {
        *symbol = 112;
        return 6;
    } else {
        *symbol = 110;
        return 6;
    }

node_1011:
    if (bits & 0x8000000) {
        goto node_10111;
    } else {
        goto node_10110;
    }

node_10110:
    if (bits & 0x4000000) {
        *symbol = 117;
        return 6;
    } else {
        *symbol = 114;
        return 6;
    }

node_10111:
    if (bits & 0x4000000) {
        goto node_101111;
    } else {
        goto node_101110;
    }

node_101110:
    if (bits & 0x2000000) {
        *symbol = 66;
        return 7;
    } else {
        *symbol = 58;
        return 7;
    }

node_101111:
    if (bits & 0x2000000) {
        *symbol = 68;
        return 7;
    } else {
        *symbol = 67;
        return 7;
    }

node_11:
    if (bits & 0x20000000) {
        goto node_111;
    } else {
        goto node_110;
    }

node_110:
    if (bits & 0x10000000) {
        goto node_1101;
    } else {
        goto node_1100;
    }

node_1100:
    if (bits & 0x8000000) {
        goto node_11001;
    } else {
        goto node_11000;
    }

node_11000:
    if (bits & 0x4000000) {
        goto node_110001;
    } else {
        goto node_110000;
    }

node_110000:
    if (bits & 0x2000000) {
        *symbol = 70;
        return 7;
    } else {
        *symbol = 69;
        return 7;
    }

node_110001:
    if (bits & 0x2000000) {
        *symbol = 72;
        return 7;
    } else {
        *symbol = 71;
        return 7;
    }

node_11001:
    if (bits & 0x4000000) {
        goto node_110011;
    } else {
        goto node_110010;
    }

node_110010:
    if (bits & 0x2000000) {
        *symbol = 74;
        return 7;
    } else {
        *symbol = 73;
        return 7;
    }

node_110011:
    if (bits & 0x2000000) {
        *symbol = 76;
        return 7;
    } else {
        *symbol = 75;
        return 7;
    }

node_1101:
    if (bits & 0x8000000) {
        goto node_11011;
    } else {
        goto node_11010;
    }

node_11010:
    if (bits & 0x4000000) {
        goto node_110101;
    } else {
        goto node_110100;
    }

node_110100:
    if (bits & 0x2000000) {
        *symbol = 78;
        return 7;
    } else {
        *symbol = 77;
        return 7;
    }

node_110101:
    if (bits & 0x2000000) {
        *symbol = 80;
        return 7;
    } else {
        *symbol = 79;
        return 7;
    }

node_11011:
    if (bits & 0x4000000) {
        goto node_110111;
    } else {
        goto node_110110;
    }

node_110110:
    if (bits & 0x2000000) {
        *symbol = 82;
        return 7;
    } else {
        *symbol = 81;
        return 7;
    }

node_110111:
    if (bits & 0x2000000) {
        *symbol = 84;
        return 7;
    } else {
        *symbol = 83;
        return 7;
    }

node_111:
    if (bits & 0x10000000) {
        goto node_1111;
    } else {
        goto node_1110;
    }

node_1110:
    if (bits & 0x8000000) {
        goto node_11101;
    } else {
        goto node_11100;
    }

node_11100:
    if (bits & 0x4000000) {
        goto node_111001;
    } else {
        goto node_111000;
    }

node_111000:
    if (bits & 0x2000000) {
        *symbol = 86;
        return 7;
    } else {
        *symbol = 85;
        return 7;
    }

node_111001:
    if (bits & 0x2000000) {
        *symbol = 89;
        return 7;
    } else {
        *symbol = 87;
        return 7;
    }

node_11101:
    if (bits & 0x4000000) {
        goto node_111011;
    } else {
        goto node_111010;
    }

node_111010:
    if (bits & 0x2000000) {
        *symbol = 107;
        return 7;
    } else {
        *symbol = 106;
        return 7;
    }

node_111011:
    if (bits & 0x2000000) {
        *symbol = 118;
        return 7;
    } else {
        *symbol = 113;
        return 7;
    }

node_1111:
    if (bits & 0x8000000) {
        goto node_11111;
    } else {
        goto node_11110;
    }

node_11110:
    if (bits & 0x4000000) {
        goto node_111101;
    } else {
        goto node_111100;
    }

node_111100:
    if (bits & 0x2000000) {
        *symbol = 120;
        return 7;
    } else {
        *symbol = 119;
        return 7;
    }

node_111101:
    if (bits & 0x2000000) {
        *symbol = 122;
        return 7;
    } else {
        *symbol = 121;
        return 7;
    }

node_11111:
    if (bits & 0x4000000) {
        goto node_111111;
    } else {
        goto node_111110;
    }

node_111110:
    if (bits & 0x2000000) {
        goto node_1111101;
    } else {
        goto node_1111100;
    }

node_1111100:
    if (bits & 0x1000000) {
        *symbol = 42;
        return 8;
    } else {
        *symbol = 38;
        return 8;
    }

node_1111101:
    if (bits & 0x1000000) {
        *symbol = 59;
        return 8;
    } else {
        *symbol = 44;
        return 8;
    }

node_111111:
    if (bits & 0x2000000) {
        goto node_1111111;
    } else {
        goto node_1111110;
    }

node_1111110:
    if (bits & 0x1000000) {
        *symbol = 90;
        return 8;
    } else {
        *symbol = 88;
        return 8;
    }

node_1111111:
    if (bits & 0x1000000) {
        goto node_11111111;
    } else {
        goto node_11111110;
    }

node_11111110:
    if (bits & 0x800000) {
        goto node_111111101;
    } else {
        goto node_111111100;
    }

node_111111100:
    if (bits & 0x400000) {
        *symbol = 34;
        return 10;
    } else {
        *symbol = 33;
        return 10;
    }

node_111111101:
    if (bits & 0x400000) {
        *symbol = 41;
        return 10;
    } else {
        *symbol = 40;
        return 10;
    }

node_11111111:
    if (bits & 0x800000) {
        goto node_111111111;
    } else {
        goto node_111111110;
    }

node_111111110:
    if (bits & 0x400000) {
        goto node_1111111101;
    } else {
        *symbol = 63;
        return 10;
    }

node_1111111101:
    if (bits & 0x200000) {
        *symbol = 43;
        return 11;
    } else {
        *symbol = 39;
        return 11;
    }

node_111111111:
    if (bits & 0x400000) {
        goto node_1111111111;
    } else {
        goto node_1111111110;
    }

node_1111111110:
    if (bits & 0x200000) {
        goto node_11111111101;
    } else {
        *symbol = 124;
        return 11;
    }

node_11111111101:
    if (bits & 0x100000) {
        *symbol = 62;
        return 12;
    } else {
        *symbol = 35;
        return 12;
    }

node_1111111111:
    if (bits & 0x200000) {
        goto node_11111111111;
    } else {
        goto node_11111111110;
    }

node_11111111110:
    if (bits & 0x100000) {
        goto node_111111111101;
    } else {
        goto node_111111111100;
    }

node_111111111100:
    if (bits & 0x80000) {
        *symbol = 36;
        return 13;
    } else {
        *symbol = 0;
        return 13;
    }

node_111111111101:
    if (bits & 0x80000) {
        *symbol = 91;
        return 13;
    } else {
        *symbol = 64;
        return 13;
    }

node_11111111111:
    if (bits & 0x100000) {
        goto node_111111111111;
    } else {
        goto node_111111111110;
    }

node_111111111110:
    if (bits & 0x80000) {
        *symbol = 126;
        return 13;
    } else {
        *symbol = 93;
        return 13;
    }

node_111111111111:
    if (bits & 0x80000) {
        goto node_1111111111111;
    } else {
        goto node_1111111111110;
    }

node_1111111111110:
    if (bits & 0x40000) {
        *symbol = 125;
        return 14;
    } else {
        *symbol = 94;
        return 14;
    }

node_1111111111111:
    if (bits & 0x40000) {
        goto node_11111111111111;
    } else {
        goto node_11111111111110;
    }

node_11111111111110:
    if (bits & 0x20000) {
        *symbol = 96;
        return 15;
    } else {
        *symbol = 60;
        return 15;
    }

node_11111111111111:
    if (bits & 0x20000) {
        goto node_111111111111111;
    } else {
        *symbol = 123;
        return 15;
    }

node_111111111111111:
    if (bits & 0x10000) {
        goto node_1111111111111111;
    } else {
        goto node_1111111111111110;
    }

node_1111111111111110:
    if (bits & 0x8000) {
        goto node_11111111111111101;
    } else {
        goto node_11111111111111100;
    }

node_11111111111111100:
    if (bits & 0x4000) {
        goto node_111111111111111001;
    } else {
        goto node_111111111111111000;
    }

node_111111111111111000:
    if (bits & 0x2000) {
        *symbol = 195;
        return 19;
    } else {
        *symbol = 92;
        return 19;
    }

node_111111111111111001:
    if (bits & 0x2000) {
        goto node_1111111111111110011;
    } else {
        *symbol = 208;
        return 19;
    }

node_1111111111111110011:
    if (bits & 0x1000) {
        *symbol = 130;
        return 20;
    } else {
        *symbol = 128;
        return 20;
    }

node_11111111111111101:
    if (bits & 0x4000) {
        goto node_111111111111111011;
    } else {
        goto node_111111111111111010;
    }

node_111111111111111010:
    if (bits & 0x2000) {
        goto node_1111111111111110101;
    } else {
        goto node_1111111111111110100;
    }

node_1111111111111110100:
    if (bits & 0x1000) {
        *symbol = 162;
        return 20;
    } else {
        *symbol = 131;
        return 20;
    }

node_1111111111111110101:
    if (bits & 0x1000) {
        *symbol = 194;
        return 20;
    } else {
        *symbol = 184;
        return 20;
    }

node_111111111111111011:
    if (bits & 0x2000) {
        goto node_1111111111111110111;
    } else {
        goto node_1111111111111110110;
    }

node_1111111111111110110:
    if (bits & 0x1000) {
        *symbol = 226;
        return 20;
    } else {
        *symbol = 224;
        return 20;
    }

node_1111111111111110111:
    if (bits & 0x1000) {
        goto node_11111111111111101111;
    } else {
        goto node_11111111111111101110;
    }

node_11111111111111101110:
    if (bits & 0x800) {
        *symbol = 161;
        return 21;
    } else {
        *symbol = 153;
        return 21;
    }

node_11111111111111101111:
    if (bits & 0x800) {
        *symbol = 172;
        return 21;
    } else {
        *symbol = 167;
        return 21;
    }

node_1111111111111111:
    if (bits & 0x8000) {
        goto node_11111111111111111;
    } else {
        goto node_11111111111111110;
    }

node_11111111111111110:
    if (bits & 0x4000) {
        goto node_111111111111111101;
    } else {
        goto node_111111111111111100;
    }

node_111111111111111100:
    if (bits & 0x2000) {
        goto node_1111111111111111001;
    } else {
        goto node_1111111111111111000;
    }

node_1111111111111111000:
    if (bits & 0x1000) {
        goto node_11111111111111110001;
    } else {
        goto node_11111111111111110000;
    }

node_11111111111111110000:
    if (bits & 0x800) {
        *symbol = 177;
        return 21;
    } else {
        *symbol = 176;
        return 21;
    }

node_11111111111111110001:
    if (bits & 0x800) {
        *symbol = 209;
        return 21;
    } else {
        *symbol = 179;
        return 21;
    }

node_1111111111111111001:
    if (bits & 0x1000) {
        goto node_11111111111111110011;
    } else {
        goto node_11111111111111110010;
    }

node_11111111111111110010:
    if (bits & 0x800) {
        *symbol = 217;
        return 21;
    } else {
        *symbol = 216;
        return 21;
    }

node_11111111111111110011:
    if (bits & 0x800) {
        *symbol = 229;
        return 21;
    } else {
        *symbol = 227;
        return 21;
    }

node_111111111111111101:
    if (bits & 0x2000) {
        goto node_1111111111111111011;
    } else {
        goto node_1111111111111111010;
    }

node_1111111111111111010:
    if (bits & 0x1000) {
        goto node_11111111111111110101;
    } else {
        goto node_11111111111111110100;
    }

node_11111111111111110100:
    if (bits & 0x800) {
        goto node_111111111111111101001;
    } else {
        *symbol = 230;
        return 21;
    }

node_111111111111111101001:
    if (bits & 0x400) {
        *symbol = 132;
        return 22;
    } else {
        *symbol = 129;
        return 22;
    }

node_11111111111111110101:
    if (bits & 0x800) {
        goto node_111111111111111101011;
    } else {
        goto node_111111111111111101010;
    }

node_111111111111111101010:
    if (bits & 0x400) {
        *symbol = 134;
        return 22;
    } else {
        *symbol = 133;
        return 22;
    }

node_111111111111111101011:
    if (bits & 0x400) {
        *symbol = 146;
        return 22;
    } else {
        *symbol = 136;
        return 22;
    }

node_1111111111111111011:
    if (bits & 0x1000) {
        goto node_11111111111111110111;
    } else {
        goto node_11111111111111110110;
    }

node_11111111111111110110:
    if (bits & 0x800) {
        goto node_111111111111111101101;
    } else {
        goto node_111111111111111101100;
    }

node_111111111111111101100:
    if (bits & 0x400) {
        *symbol = 156;
        return 22;
    } else {
        *symbol = 154;
        return 22;
    }

node_111111111111111101101:
    if (bits & 0x400) {
        *symbol = 163;
        return 22;
    } else {
        *symbol = 160;
        return 22;
    }

node_11111111111111110111:
    if (bits & 0x800) {
        goto node_111111111111111101111;
    } else {
        goto node_111111111111111101110;
    }

node_111111111111111101110:
    if (bits & 0x400) {
        *symbol = 169;
        return 22;
    } else {
        *symbol = 164;
        return 22;
    }

node_111111111111111101111:
    if (bits & 0x400) {
        *symbol = 173;
        return 22;
    } else {
        *symbol = 170;
        return 22;
    }

node_11111111111111111:
    if (bits & 0x4000) {
        goto node_111111111111111111;
    } else {
        goto node_111111111111111110;
    }

node_111111111111111110:
    if (bits & 0x2000) {
        goto node_1111111111111111101;
    } else {
        goto node_1111111111111111100;
    }

node_1111111111111111100:
    if (bits & 0x1000) {
        goto node_11111111111111111001;
    } else {
        goto node_11111111111111111000;
    }

node_11111111111111111000:
    if (bits & 0x800) {
        goto node_111111111111111110001;
    } else {
        goto node_111111111111111110000;
    }

node_111111111111111110000:
    if (bits & 0x400) {
        *symbol = 181;
        return 22;
    } else {
        *symbol = 178;
        return 22;
    }

node_111111111111111110001:
    if (bits & 0x400) {
        *symbol = 186;
        return 22;
    } else {
        *symbol = 185;
        return 22;
    }

node_11111111111111111001:
    if (bits & 0x800) {
        goto node_111111111111111110011;
    } else {
        goto node_111111111111111110010;
    }

node_111111111111111110010:
    if (bits & 0x400) {
        *symbol = 189;
        return 22;
    } else {
        *symbol = 187;
        return 22;
    }

node_111111111111111110011:
    if (bits & 0x400) {
        *symbol = 196;
        return 22;
    } else {
        *symbol = 190;
        return 22;
    }

node_1111111111111111101:
    if (bits & 0x1000) {
        goto node_11111111111111111011;
    } else {
        goto node_11111111111111111010;
    }

node_11111111111111111010:
    if (bits & 0x800) {
        goto node_111111111111111110101;
    } else {
        goto node_111111111111111110100;
    }

node_111111111111111110100:
    if (bits & 0x400) {
        *symbol = 228;
        return 22;
    } else {
        *symbol = 198;
        return 22;
    }

node_111111111111111110101:
    if (bits & 0x400) {
        *symbol = 233;
        return 22;
    } else {
        *symbol = 232;
        return 22;
    }

node_11111111111111111011:
    if (bits & 0x800) {
        goto node_111111111111111110111;
    } else {
        goto node_111111111111111110110;
    }

node_111111111111111110110:
    if (bits & 0x400) {
        goto node_1111111111111111101101;
    } else {
        goto node_1111111111111111101100;
    }

node_1111111111111111101100:
    if (bits & 0x200) {
        *symbol = 135;
        return 23;
    } else {
        *symbol = 1;
        return 23;
    }

node_1111111111111111101101:
    if (bits & 0x200) {
        *symbol = 138;
        return 23;
    } else {
        *symbol = 137;
        return 23;
    }

node_111111111111111110111:
    if (bits & 0x400) {
        goto node_1111111111111111101111;
    } else {
        goto node_1111111111111111101110;
    }

node_1111111111111111101110:
    if (bits & 0x200) {
        *symbol = 140;
        return 23;
    } else {
        *symbol = 139;
        return 23;
    }

node_1111111111111111101111:
    if (bits & 0x200) {
        *symbol = 143;
        return 23;
    } else {
        *symbol = 141;
        return 23;
    }

node_111111111111111111:
    if (bits & 0x2000) {
        goto node_1111111111111111111;
    } else {
        goto node_1111111111111111110;
    }

node_1111111111111111110:
    if (bits & 0x1000) {
        goto node_11111111111111111101;
    } else {
        goto node_11111111111111111100;
    }

node_11111111111111111100:
    if (bits & 0x800) {
        goto node_111111111111111111001;
    } else {
        goto node_111111111111111111000;
    }

node_111111111111111111000:
    if (bits & 0x400) {
        goto node_1111111111111111110001;
    } else {
        goto node_1111111111111111110000;
    }

node_1111111111111111110000:
    if (bits & 0x200) {
        *symbol = 149;
        return 23;
    } else {
        *symbol = 147;
        return 23;
    }

node_1111111111111111110001:
    if (bits & 0x200) {
        *symbol = 151;
        return 23;
    } else {
        *symbol = 150;
        return 23;
    }

node_111111111111111111001:
    if (bits & 0x400) {
        goto node_1111111111111111110011;
    } else {
        goto node_1111111111111111110010;
    }

node_1111111111111111110010:
    if (bits & 0x200) {
        *symbol = 155;
        return 23;
    } else {
        *symbol = 152;
        return 23;
    }

node_1111111111111111110011:
    if (bits & 0x200) {
        *symbol = 158;
        return 23;
    } else {
        *symbol = 157;
        return 23;
    }

node_11111111111111111101:
    if (bits & 0x800) {
        goto node_111111111111111111011;
    } else {
        goto node_111111111111111111010;
    }

node_111111111111111111010:
    if (bits & 0x400) {
        goto node_1111111111111111110101;
    } else {
        goto node_1111111111111111110100;
    }

node_1111111111111111110100:
    if (bits & 0x200) {
        *symbol = 166;
        return 23;
    } else {
        *symbol = 165;
        return 23;
    }

node_1111111111111111110101:
    if (bits & 0x200) {
        *symbol = 174;
        return 23;
    } else {
        *symbol = 168;
        return 23;
    }

node_111111111111111111011:
    if (bits & 0x400) {
        goto node_1111111111111111110111;
    } else {
        goto node_1111111111111111110110;
    }

node_1111111111111111110110:
    if (bits & 0x200) {
        *symbol = 180;
        return 23;
    } else {
        *symbol = 175;
        return 23;
    }

node_1111111111111111110111:
    if (bits & 0x200) {
        *symbol = 183;
        return 23;
    } else {
        *symbol = 182;
        return 23;
    }

node_1111111111111111111:
    if (bits & 0x1000) {
        goto node_11111111111111111111;
    } else {
        goto node_11111111111111111110;
    }

node_11111111111111111110:
    if (bits & 0x800) {
        goto node_111111111111111111101;
    } else {
        goto node_111111111111111111100;
    }

node_111111111111111111100:
    if (bits & 0x400) {
        goto node_1111111111111111111001;
    } else {
        goto node_1111111111111111111000;
    }

node_1111111111111111111000:
    if (bits & 0x200) {
        *symbol = 191;
        return 23;
    } else {
        *symbol = 188;
        return 23;
    }

node_1111111111111111111001:
    if (bits & 0x200) {
        *symbol = 231;
        return 23;
    } else {
        *symbol = 197;
        return 23;
    }

node_111111111111111111101:
    if (bits & 0x400) {
        goto node_1111111111111111111011;
    } else {
        goto node_1111111111111111111010;
    }

node_1111111111111111111010:
    if (bits & 0x200) {
        goto node_11111111111111111110101;
    } else {
        *symbol = 239;
        return 23;
    }

node_11111111111111111110101:
    if (bits & 0x100) {
        *symbol = 142;
        return 24;
    } else {
        *symbol = 9;
        return 24;
    }

node_1111111111111111111011:
    if (bits & 0x200) {
        goto node_11111111111111111110111;
    } else {
        goto node_11111111111111111110110;
    }

node_11111111111111111110110:
    if (bits & 0x100) {
        *symbol = 145;
        return 24;
    } else {
        *symbol = 144;
        return 24;
    }

node_11111111111111111110111:
    if (bits & 0x100) {
        *symbol = 159;
        return 24;
    } else {
        *symbol = 148;
        return 24;
    }

node_11111111111111111111:
    if (bits & 0x800) {
        goto node_111111111111111111111;
    } else {
        goto node_111111111111111111110;
    }

node_111111111111111111110:
    if (bits & 0x400) {
        goto node_1111111111111111111101;
    } else {
        goto node_1111111111111111111100;
    }

node_1111111111111111111100:
    if (bits & 0x200) {
        goto node_11111111111111111111001;
    } else {
        goto node_11111111111111111111000;
    }

node_11111111111111111111000:
    if (bits & 0x100) {
        *symbol = 206;
        return 24;
    } else {
        *symbol = 171;
        return 24;
    }

node_11111111111111111111001:
    if (bits & 0x100) {
        *symbol = 225;
        return 24;
    } else {
        *symbol = 215;
        return 24;
    }

node_1111111111111111111101:
    if (bits & 0x200) {
        goto node_11111111111111111111011;
    } else {
        goto node_11111111111111111111010;
    }

node_11111111111111111111010:
    if (bits & 0x100) {
        *symbol = 237;
        return 24;
    } else {
        *symbol = 236;
        return 24;
    }

node_11111111111111111111011:
    if (bits & 0x100) {
        goto node_111111111111111111110111;
    } else {
        goto node_111111111111111111110110;
    }

node_111111111111111111110110:
    if (bits & 0x80) {
        *symbol = 207;
        return 25;
    } else {
        *symbol = 199;
        return 25;
    }

node_111111111111111111110111:
    if (bits & 0x80) {
        *symbol = 235;
        return 25;
    } else {
        *symbol = 234;
        return 25;
    }

node_111111111111111111111:
    if (bits & 0x400) {
        goto node_1111111111111111111111;
    } else {
        goto node_1111111111111111111110;
    }

node_1111111111111111111110:
    if (bits & 0x200) {
        goto node_11111111111111111111101;
    } else {
        goto node_11111111111111111111100;
    }

node_11111111111111111111100:
    if (bits & 0x100) {
        goto node_111111111111111111111001;
    } else {
        goto node_111111111111111111111000;
    }

node_111111111111111111111000:
    if (bits & 0x80) {
        goto node_1111111111111111111110001;
    } else {
        goto node_1111111111111111111110000;
    }

node_1111111111111111111110000:
    if (bits & 0x40) {
        *symbol = 193;
        return 26;
    } else {
        *symbol = 192;
        return 26;
    }

node_1111111111111111111110001:
    if (bits & 0x40) {
        *symbol = 201;
        return 26;
    } else {
        *symbol = 200;
        return 26;
    }

node_111111111111111111111001:
    if (bits & 0x80) {
        goto node_1111111111111111111110011;
    } else {
        goto node_1111111111111111111110010;
    }

node_1111111111111111111110010:
    if (bits & 0x40) {
        *symbol = 205;
        return 26;
    } else {
        *symbol = 202;
        return 26;
    }

node_1111111111111111111110011:
    if (bits & 0x40) {
        *symbol = 213;
        return 26;
    } else {
        *symbol = 210;
        return 26;
    }

node_11111111111111111111101:
    if (bits & 0x100) {
        goto node_111111111111111111111011;
    } else {
        goto node_111111111111111111111010;
    }

node_111111111111111111111010:
    if (bits & 0x80) {
        goto node_1111111111111111111110101;
    } else {
        goto node_1111111111111111111110100;
    }

node_1111111111111111111110100:
    if (bits & 0x40) {
        *symbol = 219;
        return 26;
    } else {
        *symbol = 218;
        return 26;
    }

node_1111111111111111111110101:
    if (bits & 0x40) {
        *symbol = 240;
        return 26;
    } else {
        *symbol = 238;
        return 26;
    }

node_111111111111111111111011:
    if (bits & 0x80) {
        goto node_1111111111111111111110111;
    } else {
        goto node_1111111111111111111110110;
    }

node_1111111111111111111110110:
    if (bits & 0x40) {
        *symbol = 243;
        return 26;
    } else {
        *symbol = 242;
        return 26;
    }

node_1111111111111111111110111:
    if (bits & 0x40) {
        goto node_11111111111111111111101111;
    } else {
        *symbol = 255;
        return 26;
    }

node_11111111111111111111101111:
    if (bits & 0x20) {
        *symbol = 204;
        return 27;
    } else {
        *symbol = 203;
        return 27;
    }

node_1111111111111111111111:
    if (bits & 0x200) {
        goto node_11111111111111111111111;
    } else {
        goto node_11111111111111111111110;
    }

node_11111111111111111111110:
    if (bits & 0x100) {
        goto node_111111111111111111111101;
    } else {
        goto node_111111111111111111111100;
    }

node_111111111111111111111100:
    if (bits & 0x80) {
        goto node_1111111111111111111111001;
    } else {
        goto node_1111111111111111111111000;
    }

node_1111111111111111111111000:
    if (bits & 0x40) {
        goto node_11111111111111111111110001;
    } else {
        goto node_11111111111111111111110000;
    }

node_11111111111111111111110000:
    if (bits & 0x20) {
        *symbol = 212;
        return 27;
    } else {
        *symbol = 211;
        return 27;
    }

node_11111111111111111111110001:
    if (bits & 0x20) {
        *symbol = 221;
        return 27;
    } else {
        *symbol = 214;
        return 27;
    }

node_1111111111111111111111001:
    if (bits & 0x40) {
        goto node_11111111111111111111110011;
    } else {
        goto node_11111111111111111111110010;
    }

node_11111111111111111111110010:
    if (bits & 0x20) {
        *symbol = 223;
        return 27;
    } else {
        *symbol = 222;
        return 27;
    }

node_11111111111111111111110011:
    if (bits & 0x20) {
        *symbol = 244;
        return 27;
    } else {
        *symbol = 241;
        return 27;
    }

node_111111111111111111111101:
    if (bits & 0x80) {
        goto node_1111111111111111111111011;
    } else {
        goto node_1111111111111111111111010;
    }

node_1111111111111111111111010:
    if (bits & 0x40) {
        goto node_11111111111111111111110101;
    } else {
        goto node_11111111111111111111110100;
    }

node_11111111111111111111110100:
    if (bits & 0x20) {
        *symbol = 246;
        return 27;
    } else {
        *symbol = 245;
        return 27;
    }

node_11111111111111111111110101:
    if (bits & 0x20) {
        *symbol = 248;
        return 27;
    } else {
        *symbol = 247;
        return 27;
    }

node_1111111111111111111111011:
    if (bits & 0x40) {
        goto node_11111111111111111111110111;
    } else {
        goto node_11111111111111111111110110;
    }

node_11111111111111111111110110:
    if (bits & 0x20) {
        *symbol = 251;
        return 27;
    } else {
        *symbol = 250;
        return 27;
    }

node_11111111111111111111110111:
    if (bits & 0x20) {
        *symbol = 253;
        return 27;
    } else {
        *symbol = 252;
        return 27;
    }

node_11111111111111111111111:
    if (bits & 0x100) {
        goto node_111111111111111111111111;
    } else {
        goto node_111111111111111111111110;
    }

node_111111111111111111111110:
    if (bits & 0x80) {
        goto node_1111111111111111111111101;
    } else {
        goto node_1111111111111111111111100;
    }

node_1111111111111111111111100:
    if (bits & 0x40) {
        goto node_11111111111111111111111001;
    } else {
        goto node_11111111111111111111111000;
    }

node_11111111111111111111111000:
    if (bits & 0x20) {
        goto node_111111111111111111111110001;
    } else {
        *symbol = 254;
        return 27;
    }

node_111111111111111111111110001:
    if (bits & 0x10) {
        *symbol = 3;
        return 28;
    } else {
        *symbol = 2;
        return 28;
    }

node_11111111111111111111111001:
    if (bits & 0x20) {
        goto node_111111111111111111111110011;
    } else {
        goto node_111111111111111111111110010;
    }

node_111111111111111111111110010:
    if (bits & 0x10) {
        *symbol = 5;
        return 28;
    } else {
        *symbol = 4;
        return 28;
    }

node_111111111111111111111110011:
    if (bits & 0x10) {
        *symbol = 7;
        return 28;
    } else {
        *symbol = 6;
        return 28;
    }

node_1111111111111111111111101:
    if (bits & 0x40) {
        goto node_11111111111111111111111011;
    } else {
        goto node_11111111111111111111111010;
    }

node_11111111111111111111111010:
    if (bits & 0x20) {
        goto node_111111111111111111111110101;
    } else {
        goto node_111111111111111111111110100;
    }

node_111111111111111111111110100:
    if (bits & 0x10) {
        *symbol = 11;
        return 28;
    } else {
        *symbol = 8;
        return 28;
    }

node_111111111111111111111110101:
    if (bits & 0x10) {
        *symbol = 14;
        return 28;
    } else {
        *symbol = 12;
        return 28;
    }

node_11111111111111111111111011:
    if (bits & 0x20) {
        goto node_111111111111111111111110111;
    } else {
        goto node_111111111111111111111110110;
    }

node_111111111111111111111110110:
    if (bits & 0x10) {
        *symbol = 16;
        return 28;
    } else {
        *symbol = 15;
        return 28;
    }

node_111111111111111111111110111:
    if (bits & 0x10) {
        *symbol = 18;
        return 28;
    } else {
        *symbol = 17;
        return 28;
    }

node_111111111111111111111111:
    if (bits & 0x80) {
        goto node_1111111111111111111111111;
    } else {
        goto node_1111111111111111111111110;
    }

node_1111111111111111111111110:
    if (bits & 0x40) {
        goto node_11111111111111111111111101;
    } else {
        goto node_11111111111111111111111100;
    }

node_11111111111111111111111100:
    if (bits & 0x20) {
        goto node_111111111111111111111111001;
    } else {
        goto node_111111111111111111111111000;
    }

node_111111111111111111111111000:
    if (bits & 0x10) {
        *symbol = 20;
        return 28;
    } else {
        *symbol = 19;
        return 28;
    }

node_111111111111111111111111001:
    if (bits & 0x10) {
        *symbol = 23;
        return 28;
    } else {
        *symbol = 21;
        return 28;
    }

node_11111111111111111111111101:
    if (bits & 0x20) {
        goto node_111111111111111111111111011;
    } else {
        goto node_111111111111111111111111010;
    }

node_111111111111111111111111010:
    if (bits & 0x10) {
        *symbol = 25;
        return 28;
    } else {
        *symbol = 24;
        return 28;
    }

node_111111111111111111111111011:
    if (bits & 0x10) {
        *symbol = 27;
        return 28;
    } else {
        *symbol = 26;
        return 28;
    }

node_1111111111111111111111111:
    if (bits & 0x40) {
        goto node_11111111111111111111111111;
    } else {
        goto node_11111111111111111111111110;
    }

node_11111111111111111111111110:
    if (bits & 0x20) {
        goto node_111111111111111111111111101;
    } else {
        goto node_111111111111111111111111100;
    }

node_111111111111111111111111100:
    if (bits & 0x10) {
        *symbol = 29;
        return 28;
    } else {
        *symbol = 28;
        return 28;
    }

node_111111111111111111111111101:
    if (bits & 0x10) {
        *symbol = 31;
        return 28;
    } else {
        *symbol = 30;
        return 28;
    }

node_11111111111111111111111111:
    if (bits & 0x20) {
        goto node_111111111111111111111111111;
    } else {
        goto node_111111111111111111111111110;
    }

node_111111111111111111111111110:
    if (bits & 0x10) {
        *symbol = 220;
        return 28;
    } else {
        *symbol = 127;
        return 28;
    }

node_111111111111111111111111111:
    if (bits & 0x10) {
        goto node_1111111111111111111111111111;
    } else {
        *symbol = 249;
        return 28;
    }

node_1111111111111111111111111111:
    if (bits & 0x8) {
        goto node_11111111111111111111111111111;
    } else {
        goto node_11111111111111111111111111110;
    }

node_11111111111111111111111111110:
    if (bits & 0x4) {
        *symbol = 13;
        return 30;
    } else {
        *symbol = 10;
        return 30;
    }

node_11111111111111111111111111111:
    if (bits & 0x4) {
        return 0; /* invalid node */
    } else {
        *symbol = 22;
        return 30;
    }

}

struct aws_huffman_symbol_coder *hpack_get_coder(void) {

    static struct aws_huffman_symbol_coder coder = {
        .encode = encode_symbol,
        .decode = decode_symbol,
        .userdata = NULL,
    };
    return &coder;
}
