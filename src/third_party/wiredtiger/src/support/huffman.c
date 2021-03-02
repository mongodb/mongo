/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name MongoDB or the name WiredTiger
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY MONGODB INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "wt_internal.h"

#define __HUFFMAN_DETAIL 0 /* Set to 1 for debugging output. */

/* Length of header in compressed message, in bits. */
#define WT_HUFFMAN_HEADER 3

/*
 * Maximum allowed length of Huffman code words, which otherwise can range up to (#symbols - 1) bits
 * long. Lower value to use less memory for tables, higher value for better compression. Max value =
 * 16 (or 32-7=25 or 64-7=57 if adjust data types). FYI, JPEG uses 16. A side effect of limiting max
 * code length is that the worst case compression (a message of the least frequent symbols) is
 * shorter.
 */
#define MAX_CODE_LENGTH 16

typedef struct __wt_freqtree_node {
    /*
     * Data structure representing a node of the huffman tree. It holds a
     * 64-bit weight and pointers to the left and right child nodes.  The
     * node either has two child nodes or none.
     */
    uint8_t symbol; /* only used in leaf nodes */
    uint64_t weight;
    struct __wt_freqtree_node *left;  /* bit 0 */
    struct __wt_freqtree_node *right; /* bit 1 */
} WT_FREQTREE_NODE;

typedef struct __wt_huffman_code {
    uint16_t pattern; /* requirement: length of field's type
                       * in bits >= MAX_CODE_LENGTH.
                       */
    uint8_t length;
} WT_HUFFMAN_CODE;

typedef struct __wt_huffman_obj {
    /*
     * Data structure here defines specific instance of the encoder/decoder.
     */
    u_int numSymbols; /* Symbols: UINT16_MAX or UINT8_MAX */

    uint16_t max_depth, min_depth; /* Tree max/min depths */

    /*
     * use: codes[symbol] = struct with pattern and length. Used in encoding and decoding. memory:
     * codes[0-to-(number of symbols - 1)]
     */
    WT_HUFFMAN_CODE *codes;

    /*
     * use: code2symbol[Huffman_code] = symbol. Used in decoding. memory: code2symbol[1 <<
     * max_code_length]
     */
    uint8_t *code2symbol;
} WT_HUFFMAN_OBJ;

/*
 * Queue element data structure.
 *
 * Consists of a pointer to a huffman tree node, and a pointer to the next element in the queue.
 */
typedef struct node_queue_elem {
    WT_FREQTREE_NODE *node;
    struct node_queue_elem *next;
} NODE_QUEUE_ELEM;

/*
 * Queue of huffman tree nodes.
 *
 * Contains a pointer to the beginning and the end of the queue, which is implemented as a linked
 * list.
 */
typedef struct node_queue {
    NODE_QUEUE_ELEM *first;
    NODE_QUEUE_ELEM *last;
} NODE_QUEUE;

/*
 * Internal data structure used to preserve the symbol when rearranging the frequency array.
 */
typedef struct __indexed_byte {
    uint32_t symbol; /* not uint8_t: match external data structure */
    uint32_t frequency;
} INDEXED_SYMBOL;

static int WT_CDECL indexed_freq_compare(const void *, const void *);
static int WT_CDECL indexed_symbol_compare(const void *, const void *);
static void make_table(WT_SESSION_IMPL *, uint8_t *, uint16_t, WT_HUFFMAN_CODE *, u_int);
static void node_queue_close(WT_SESSION_IMPL *, NODE_QUEUE *);
static void node_queue_dequeue(WT_SESSION_IMPL *, NODE_QUEUE *, WT_FREQTREE_NODE **);
static int node_queue_enqueue(WT_SESSION_IMPL *, NODE_QUEUE *, WT_FREQTREE_NODE *);
static uint32_t profile_tree(WT_FREQTREE_NODE *, uint16_t, uint16_t *, uint16_t *);
static void recursive_free_node(WT_SESSION_IMPL *, WT_FREQTREE_NODE *);
static void set_codes(WT_FREQTREE_NODE *, WT_HUFFMAN_CODE *, uint16_t, uint8_t);

#define node_queue_is_empty(queue) ((queue) == NULL || (queue)->first == NULL)

/*
 * indexed_symbol_compare --
 *     Qsort comparator to order the table by symbol, lowest to highest.
 */
static int WT_CDECL
indexed_symbol_compare(const void *a, const void *b)
{
    return (((INDEXED_SYMBOL *)a)->symbol > ((INDEXED_SYMBOL *)b)->symbol ?
        1 :
        (((INDEXED_SYMBOL *)a)->symbol < ((INDEXED_SYMBOL *)b)->symbol ? -1 : 0));
}

/*
 * indexed_freq_compare --
 *     Qsort comparator to order the table by frequency (the most frequent symbols will be at the
 *     end of the array).
 */
static int WT_CDECL
indexed_freq_compare(const void *a, const void *b)
{
    return (((INDEXED_SYMBOL *)a)->frequency > ((INDEXED_SYMBOL *)b)->frequency ?
        1 :
        (((INDEXED_SYMBOL *)a)->frequency < ((INDEXED_SYMBOL *)b)->frequency ? -1 : 0));
}

/*
 * profile_tree --
 *     Traverses tree to determine #leaves under each node, max depth, min depth of leaf.
 */
static uint32_t
profile_tree(WT_FREQTREE_NODE *node, uint16_t len, uint16_t *max_depth, uint16_t *min_depth)
{
    uint32_t leaf_cnt;

    if (node->left == NULL && node->right == NULL) { /* leaf */
        leaf_cnt = 1;
        if (*max_depth < len)
            *max_depth = len;
        if (*min_depth > len)
            *min_depth = len;
    } else {
        /*
         * internal node -- way tree constructed internal always has left and right children
         */
        leaf_cnt = profile_tree(node->left, len + 1, max_depth, min_depth) +
          profile_tree(node->right, len + 1, max_depth, min_depth);
    }
    node->weight = leaf_cnt; /* abuse weight field */
    return (leaf_cnt);
}

/*
 * set_codes --
 *     Computes Huffman code for each symbol in tree. Method is standard way in the literature,
 *     except that limits maximum code length. A known max code length is important for limiting
 *     memory use by the tables and for knowing how large data types need to be such as the field
 *     that holds the code pattern.
 */
static void
set_codes(WT_FREQTREE_NODE *node, WT_HUFFMAN_CODE *codes, uint16_t pattern, uint8_t len)
{
    WT_HUFFMAN_CODE *code;
    uint16_t patternleft, patternright, half;
    uint8_t remaining;

    if (node->left == NULL && node->right == NULL) {
        code = &codes[node->symbol];
        code->pattern = pattern;
        code->length = len;
#if __HUFFMAN_DETAIL
        printf("%" PRIx16 ": code %" PRIx16 ", len %" PRIu8 "\n", node->symbol, pattern, len);
#endif
    } else {
        /*
         * Check each subtree individually to see if can afford to split up bits into possibly
         * shorter codes, or if need to employ all remaining bits up to MAX_CODE_LENGTH to
         * consecutively number leaves.
         */
        remaining = MAX_CODE_LENGTH - len;
        /*
         * If not already in "low-bit mode", but need to be, open up lower-order bits for
         * consecutive numbering.
         */
        if (len < MAX_CODE_LENGTH &&
          ((half = (uint16_t)(1 << (remaining - 1))) < node->left->weight ||
            half < node->right->weight)) {
            pattern = (uint16_t)(pattern << remaining);
            len = MAX_CODE_LENGTH;
        }

        if (len < MAX_CODE_LENGTH) {
            patternleft = (uint16_t)((pattern << 1) | 0);
            patternright = (uint16_t)((pattern << 1) | 1);
            len++;
        } else { /* "low bit mode" */
            patternleft = pattern;
            patternright = (uint16_t)(pattern + node->left->weight);
            /* len unchanged */
        }

        set_codes(node->left, codes, patternleft, len);
        set_codes(node->right, codes, patternright, len);
    }
}

/*
 * make_table --
 *     Computes Huffman table used for subsequent lookups in encoding and decoding. With the table,
 *     encoding from a symbol to Huffman code and decoding from a code to a symbol are simple array
 *     lookups.
 */
static void
make_table(WT_SESSION_IMPL *session, uint8_t *code2symbol, uint16_t max_depth,
  WT_HUFFMAN_CODE *codes, u_int symcnt)
{
    u_int i;
    uint32_t j, c1, c2; /* Exceeds uint16_t bounds at loop boundary. */
    uint16_t c;
    uint8_t len, shift;

    /* Zero out, for assertion below. */
    for (j = 0, c2 = (1U << max_depth); j < c2; j++)
        code2symbol[j] = 0;

    /*
     * Here's the magic: flood all bit patterns for lower-order bits to point to same symbol.
     */
    for (i = 0; i < symcnt; i++) {
        if ((len = codes[i].length) == 0)
            continue;

        /*
         * The size of the array index should be enough to hold largest
         * index into symbol table.  Pre-existing symbols were packed
         * 0-255, so 8 bits is enough.  Don't want to make it larger
         * than necessary, we allocate (2 ^ max-code-length) of them.
         */
        c = codes[i].pattern;
        shift = (uint8_t)(max_depth - len);
        c1 = (uint32_t)c << shift;
        c2 = (uint32_t)(c + 1) << shift;
        for (j = c1; j < c2; j++) {
            WT_ASSERT(session, code2symbol[j] == 0);
            code2symbol[j] = (uint8_t)i;
        }
    }
}

/*
 * recursive_free_node --
 *     Recursively free the huffman frequency tree's nodes.
 */
static void
recursive_free_node(WT_SESSION_IMPL *session, WT_FREQTREE_NODE *node)
{
    if (node != NULL) {
        recursive_free_node(session, node->left);
        recursive_free_node(session, node->right);
        __wt_free(session, node);
    }
}

/*
 * __wt_huffman_open --
 *     Take a frequency table and return a pointer to a descriptor object.
 */
int
__wt_huffman_open(
  WT_SESSION_IMPL *session, void *symbol_frequency_array, u_int symcnt, u_int numbytes, void *retp)
{
    INDEXED_SYMBOL *indexed_freqs, *sym;
    NODE_QUEUE *combined_nodes, *leaves;
    WT_DECL_RET;
    WT_FREQTREE_NODE *node, *node2, **refnode, *tempnode;
    WT_HUFFMAN_OBJ *huffman;
    u_int i;
    uint64_t w1, w2;

    indexed_freqs = NULL;
    combined_nodes = leaves = NULL;
    node = node2 = tempnode = NULL;

    WT_RET(__wt_calloc_one(session, &huffman));

    /*
     * The frequency table is 4B pairs of symbol and frequency. The symbol is either 1 or 2 bytes
     * and the frequency ranges from 1 to UINT32_MAX (a frequency of 0 means the value is never
     * expected to appear in the input). Validate the symbols are within range.
     */
    if (numbytes != 1 && numbytes != 2)
        WT_ERR_MSG(session, EINVAL, "illegal number of symbol bytes specified for a huffman table");

    if (symcnt == 0)
        WT_ERR_MSG(session, EINVAL, "illegal number of symbols specified for a huffman table");

    huffman->numSymbols = numbytes == 2 ? UINT16_MAX : UINT8_MAX;

    /*
     * Order the array by symbol and check for invalid symbols and duplicates.
     */
    sym = symbol_frequency_array;
    __wt_qsort(sym, symcnt, sizeof(INDEXED_SYMBOL), indexed_symbol_compare);
    for (i = 0; i < symcnt; ++i) {
        if (i > 0 && sym[i].symbol == sym[i - 1].symbol)
            WT_ERR_MSG(session, EINVAL,
              "duplicate symbol %" PRIu32 " (%#" PRIx32 ") specified in a huffman table",
              sym[i].symbol, sym[i].symbol);
        if (sym[i].symbol > huffman->numSymbols)
            WT_ERR_MSG(session, EINVAL,
              "out-of-range symbol %" PRIu32 " (%#" PRIx32 ") specified in a huffman table",
              sym[i].symbol, sym[i].symbol);
    }

    /*
     * Massage frequencies.
     */
    WT_ERR(__wt_calloc_def(session, 256, &indexed_freqs));

    /*
     * Minimum of frequency==1 so everybody gets a Huffman code, in case data evolves and we need to
     * represent this value.
     */
    for (i = 0; i < 256; i++) {
        sym = &indexed_freqs[i];
        sym->symbol = i;
        sym->frequency = 1;
    }
    /*
     * Avoid large tables by splitting UTF-16 frequencies into high byte and low byte.
     */
    for (i = 0; i < symcnt; i++) {
        sym = &((INDEXED_SYMBOL *)symbol_frequency_array)[i];
        indexed_freqs[sym->symbol & 0xff].frequency += sym->frequency;
        if (numbytes == 2)
            indexed_freqs[(sym->symbol >> 8) & 0xff].frequency += sym->frequency;
    }
    huffman->numSymbols = symcnt = 256;

    /*
     * The array must be sorted by frequency to be able to use a linear time construction algorithm.
     */
    __wt_qsort((void *)indexed_freqs, symcnt, sizeof(INDEXED_SYMBOL), indexed_freq_compare);

    /* We need two node queues to build the tree. */
    WT_ERR(__wt_calloc_one(session, &leaves));
    WT_ERR(__wt_calloc_one(session, &combined_nodes));

    /*
     * Adding the leaves to the queue.
     *
     * Discard symbols with a frequency of 0; this assumes these symbols never occur in the source
     * stream, and the purpose is to reduce the huffman tree's size.
     */
    for (i = 0; i < symcnt; ++i)
        if (indexed_freqs[i].frequency > 0) {
            WT_ERR(__wt_calloc_one(session, &tempnode));
            tempnode->symbol = (uint8_t)indexed_freqs[i].symbol;
            tempnode->weight = indexed_freqs[i].frequency;
            WT_ERR(node_queue_enqueue(session, leaves, tempnode));
            tempnode = NULL;
        }

    while (!node_queue_is_empty(leaves) || !node_queue_is_empty(combined_nodes)) {
        /*
         * We have to get the node with the smaller weight, examining both queues' first element. We
         * are collecting pairs of these items, by alternating between node and node2:
         */
        refnode = !node ? &node : &node2;

        /*
         * To decide which queue must be used, we get the weights of the first items from both:
         */
        w1 = node_queue_is_empty(leaves) ? UINT64_MAX : leaves->first->node->weight;
        w2 = node_queue_is_empty(combined_nodes) ? UINT64_MAX : combined_nodes->first->node->weight;

        /*
         * Based on the two weights we finally can dequeue the smaller element and place it to the
         * alternating target node pointer:
         */
        if (w1 < w2)
            node_queue_dequeue(session, leaves, refnode);
        else
            node_queue_dequeue(session, combined_nodes, refnode);

        /*
         * In every second run, we have both node and node2 initialized.
         */
        if (node != NULL && node2 != NULL) {
            WT_ERR(__wt_calloc_one(session, &tempnode));

            /* The new weight is the sum of the two weights. */
            tempnode->weight = node->weight + node2->weight;
            tempnode->left = node;
            tempnode->right = node2;

            /* Enqueue it to the combined nodes queue */
            WT_ERR(node_queue_enqueue(session, combined_nodes, tempnode));
            tempnode = NULL;

            /* Reset the state pointers */
            node = node2 = NULL;
        }
    }

    /*
     * The remaining node is in the node variable, this is the root of the tree. Calculate how many
     * bytes it takes to hold numSymbols bytes bits.
     */
    huffman->max_depth = 0;
    huffman->min_depth = MAX_CODE_LENGTH;
    (void)profile_tree(node, 0, &huffman->max_depth, &huffman->min_depth);
    if (huffman->max_depth > MAX_CODE_LENGTH)
        huffman->max_depth = MAX_CODE_LENGTH;

    WT_ERR(__wt_calloc_def(session, huffman->numSymbols, &huffman->codes));
    set_codes(node, huffman->codes, 0, 0);

    WT_ERR(__wt_calloc_def(session, (size_t)1U << huffman->max_depth, &huffman->code2symbol));
    make_table(
      session, huffman->code2symbol, huffman->max_depth, huffman->codes, huffman->numSymbols);

#if __HUFFMAN_DETAIL
    {
        uint8_t symbol;
        uint32_t weighted_length;

        printf("leaf depth %" PRIu16 "..%" PRIu16 ", memory use: codes %u# * %" WT_SIZET_FMT
               "B + code2symbol %u# * %" WT_SIZET_FMT "B\n",
          huffman->min_depth, huffman->max_depth, huffman->numSymbols, sizeof(WT_HUFFMAN_CODE),
          1U << huffman->max_depth, sizeof(uint16_t));

        /*
         * measure quality of computed Huffman codes, for different max bit lengths (say, 16 vs 24
         * vs 32)
         */
        weighted_length = 0;
        for (i = 0; i < symcnt; i++) {
            symbol = indexed_freqs[i].symbol;
            weighted_length += indexed_freqs[i].frequency * huffman->codes[symbol].length;
            printf("\t%" PRIu16 "->%" PRIu16 ". %" PRIu32 " * %" PRIu8 "\n", i, symbol,
              indexed_freqs[i].frequency, huffman->codes[symbol].length);
        }
        printf(
          "weighted length of all codes (the smaller the better): %" PRIu32 "\n", weighted_length);
    }
#endif

    *(void **)retp = huffman;

err:
    __wt_free(session, indexed_freqs);
    if (leaves != NULL)
        node_queue_close(session, leaves);
    if (combined_nodes != NULL)
        node_queue_close(session, combined_nodes);
    if (node != NULL)
        recursive_free_node(session, node);
    if (node2 != NULL)
        recursive_free_node(session, node2);
    __wt_free(session, tempnode);
    if (ret != 0)
        __wt_huffman_close(session, huffman);
    return (ret);
}

/*
 * __wt_huffman_close --
 *     Discard a Huffman descriptor object.
 */
void
__wt_huffman_close(WT_SESSION_IMPL *session, void *huffman_arg)
{
    WT_HUFFMAN_OBJ *huffman;

    huffman = huffman_arg;

    __wt_free(session, huffman->code2symbol);
    __wt_free(session, huffman->codes);
    __wt_free(session, huffman);
}

#if __HUFFMAN_DETAIL
/*
 * __wt_print_huffman_code --
 *     Prints a symbol's Huffman code.
 */
void
__wt_print_huffman_code(void *huffman_arg, uint16_t symbol)
{
    WT_HUFFMAN_CODE code;
    WT_HUFFMAN_OBJ *huffman;

    huffman = huffman_arg;

    if (symbol >= huffman->numSymbols)
        printf("symbol %" PRIu16 " out of range\n", symbol);
    else {
        code = huffman->codes[symbol];
        if (code.length == 0)
            printf("symbol %" PRIu16 " not defined -- 0 frequency\n", symbol);
        else
            /* should print code as binary */
            printf("%" PRIu16 " -> code pattern %" PRIx16 ", length %" PRIu8 "\n", symbol,
              code.pattern, code.length);
    }
}
#endif

/*
 * __wt_huffman_encode --
 *     Take a byte string, encode it into the target. Translation from symbol to Huffman code is a
 *     simple array lookup. WT_HUFFMAN_OBJ contains an array called 'codes' with one WT_HUFFMAN_CODE
 *     per symbol. Then, given a symbol: pattern = codes[symbol].pattern; length =
 *     codes[symbol].length; To encode byte-string, we iterate over the input symbols. For each
 *     symbol, look it up via table, shift bits onto a shift register (an int long enough to hold
 *     the longest code word + up to 7 bits remaining from the previous), then drain out full bytes.
 *     Finally, at the end flush remaining bits and write header bits.
 */
int
__wt_huffman_encode(WT_SESSION_IMPL *session, void *huffman_arg, const uint8_t *from_arg,
  size_t from_len, WT_ITEM *to_buf)
{
    WT_DECL_RET;
    WT_HUFFMAN_CODE code;
    WT_HUFFMAN_OBJ *huffman;
    WT_ITEM *tmp;
    size_t max_len, outlen, bytes;
    uint64_t bitpos;
    uint8_t len, *out, padding_info, symbol;
    const uint8_t *from;

    /*
     * Shift register to accumulate bits from input. Should be >= (MAX_CODE_LENGTH + 7), but also
     * efficient to shift bits and preferably in a machine register.
     */
    uint32_t bits;

    /* Count of bits in shift register ('bits' above). */
    uint8_t valid;

    huffman = huffman_arg;
    from = from_arg;
    tmp = NULL;

    /*
     * We don't want to find all of our callers and ensure they don't pass
     * 0-length byte strings, but there's no reason to do any work.
     */
    if (from_len == 0) {
        to_buf->size = 0;
        return (0);
    }

    /*
     * Compute the largest compressed output size, which is if all symbols are least frequent and so
     * have largest Huffman codes, and compressed output may be larger than the input size. This way
     * we don't have to worry about resizing the buffer during compression. Use the shared system
     * buffer while compressing, then allocate a new buffer of the right size and copy the result
     * into it.
     */
    max_len =
      (WT_HUFFMAN_HEADER + from_len * huffman->max_depth + 7 /* round up to full byte */) / 8;
    WT_ERR(__wt_scr_alloc(session, max_len, &tmp));

    /*
     * Leave the first 3 bits of the encoded value empty, it holds the number of bits actually used
     * in the last byte of the encoded value.
     */
    bits = 0;
    bitpos = WT_HUFFMAN_HEADER;
    valid = WT_HUFFMAN_HEADER;
    out = tmp->mem;
    for (bytes = 0; bytes < from_len; bytes++) {
        WT_ASSERT(session, WT_PTR_IN_RANGE(from, from_arg, from_len));

        symbol = *from++;

        /* Translate symbol into Huffman code and stuff into buffer. */
        code = huffman->codes[symbol];
        len = code.length;
        bits = (bits << len) | code.pattern;
        valid += len;
        bitpos += len;
        while (valid >= 8) {
            WT_ASSERT(session, WT_PTR_IN_RANGE(out, tmp->mem, tmp->memsize));
            *out++ = (uint8_t)(bits >> (valid - 8));
            valid -= 8;
        }
    }
    if (valid > 0) { /* Flush shift register. */
        WT_ASSERT(session, WT_PTR_IN_RANGE(out, tmp->mem, tmp->memsize));
        *out = (uint8_t)(bits << (8 - valid));
    }

    /*
     * At this point, bitpos is the total number of used bits (including the 3 bits at the beginning
     * of the buffer, which we'll set now to the number of bits used in the last byte). Note if the
     * number of bits used in the last byte is 8, we set the 3 bits to 0, in other words, the first
     * 3 bits of the encoded value are the number of bits used in the last byte, unless they're 0,
     * in which case there are 8 bits used in the last byte.
     */
    padding_info = (uint8_t)((bitpos % 8) << (8 - WT_HUFFMAN_HEADER));
    ((uint8_t *)tmp->mem)[0] |= padding_info;

    /* Copy result of exact known size into caller's buffer. */
    outlen = (uint32_t)((bitpos + 7) / 8);
    WT_ERR(__wt_buf_initsize(session, to_buf, outlen));
    memcpy(to_buf->mem, tmp->mem, outlen);

#if __HUFFMAN_DETAIL
    printf("encode: worst case %" PRIu32 " bytes -> actual %" PRIu32 "\n", max_len, outlen);
#endif

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_huffman_decode --
 *     Take a byte string, decode it into the target. Translation from Huffman code to symbol is a
 *     simple array lookup. WT_HUFFMAN_OBJ contains an array called 'code2symbol' indexed by code
 *     word and whose value is the corresponding symbol. From the symbol, we index into the 'codes'
 *     array to get the code length. When decoding a message, we don't know where the boundaries are
 *     between codes. The trick is that we collect enough bits for the longest code word, and
 *     construct the table such that for codes with fewer bits we flood the table with all of the
 *     bit patterns in the lower order bits. This works because the Huffman code is a unique prefix,
 *     and by the flooding we are treating bits beyond the unique prefix as don't care bits. For
 *     example, we have table of length 2^max_code_length (1<<max_code_length). For a code of
 *     length, max_code_length, the position code2symbol[code] = symbol. For a code word of
 *     (max_length - 1), we fill code2symbol[code << 1] = symbol, as well as code2symbol[(code << 1)
 *     | 1] = symbol. And so on, so in general we fill: code2symbol[(code) << shift inclusive ..
 *     (code+1) << shift exclusive]. To decode a message, we read in enough bits from input to fill
 *     the shift register with at least MAX_CODE_LENGTH bits. We look up in the table code2symbol to
 *     obtain the symbol. We look up the symbol in 'codes' to obtain the code length Finally,
 *     subtract off these bits from the shift register.
 */
int
__wt_huffman_decode(WT_SESSION_IMPL *session, void *huffman_arg, const uint8_t *from_arg,
  size_t from_len, WT_ITEM *to_buf)
{
    WT_DECL_RET;
    WT_HUFFMAN_OBJ *huffman;
    WT_ITEM *tmp;
    size_t from_bytes, len, max_len, outlen;
    uint64_t from_len_bits;
    uint32_t bits, mask, max;
    uint16_t pattern;
    uint8_t padding_info, symbol, *to, valid;
    const uint8_t *from;

    huffman = huffman_arg;
    from = from_arg;
    tmp = NULL;

    /*
     * We don't want to find all of our callers and ensure they don't pass
     * 0-length byte strings, but there's no reason to do any work.
     */
    if (from_len == 0) {
        to_buf->size = 0;
        return (0);
    }

    /*
     * The first 3 bits are the number of used bits in the last byte, unless they're 0, in which
     * case there are 8 bits used in the last byte.
     */
    padding_info = (*from & 0xE0) >> (8 - WT_HUFFMAN_HEADER);
    from_len_bits = from_len * 8;
    if (padding_info != 0)
        from_len_bits -= 8U - padding_info;

    /* Number of bits that have codes. */
    from_len_bits -= WT_HUFFMAN_HEADER;

    /*
     * Compute largest uncompressed output size, which is if all symbols are most frequent and so
     * have smallest Huffman codes and therefore largest expansion. Use the shared system buffer
     * while uncompressing, then allocate a new buffer of exactly the right size and copy the result
     * into it.
     */
    max_len = (uint32_t)(from_len_bits / huffman->min_depth);
    WT_ERR(__wt_scr_alloc(session, max_len, &tmp));
    to = tmp->mem;

    /* The first byte of input is a special case because of header bits. */
    bits = *from++;
    valid = 8 - WT_HUFFMAN_HEADER;
    from_bytes = from_len - 1;

    max = huffman->max_depth;
    mask = (1U << max) - 1;
    for (outlen = 0; from_len_bits > 0; outlen++) {
        while (valid < max && from_bytes > 0) {
            WT_ASSERT(session, WT_PTR_IN_RANGE(from, from_arg, from_len));
            bits = (bits << 8) | *from++;
            valid += 8;
            from_bytes--;
        }
        pattern = (uint16_t)(valid >= max ? /* short patterns near end */
            (bits >> (valid - max)) :
            (bits << (max - valid)));
        symbol = huffman->code2symbol[pattern & mask];
        len = huffman->codes[symbol].length;
        valid -= (uint8_t)len;

        /*
         * from_len_bits is the total number of input bits, reduced by the number of bits we consume
         * from input at each step. For all but the last step from_len_bits > len, then at the last
         * step from_len_bits == len (in other words, from_len_bits - len = 0 input bits remaining).
         * Generally, we cannot detect corruption during huffman decompression, this is one place
         * where that's not true.
         */
        if (from_len_bits < len) /* corrupted */
            WT_ERR_MSG(session, EINVAL, "huffman decompression detected input corruption");
        from_len_bits -= len;

        WT_ASSERT(session, WT_PTR_IN_RANGE(to, tmp->mem, tmp->memsize));
        *to++ = symbol;
    }

    /* Return the number of bytes used. */
    WT_ERR(__wt_buf_initsize(session, to_buf, outlen));
    memcpy(to_buf->mem, tmp->mem, outlen);

#if __HUFFMAN_DETAIL
    printf("decode: worst case %" PRIu32 " bytes -> actual %" PRIu32 "\n", max_len, outlen);
#endif

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * node_queue_close --
 *     Delete a queue from memory. It does not delete the pointed huffman tree nodes!
 */
static void
node_queue_close(WT_SESSION_IMPL *session, NODE_QUEUE *queue)
{
    NODE_QUEUE_ELEM *elem, *next_elem;

    /* Freeing each element of the queue's linked list. */
    for (elem = queue->first; elem != NULL; elem = next_elem) {
        next_elem = elem->next;
        __wt_free(session, elem);
    }

    /* Freeing the queue record itself. */
    __wt_free(session, queue);
}

/*
 * node_queue_enqueue --
 *     Push a tree node to the end of the queue.
 */
static int
node_queue_enqueue(WT_SESSION_IMPL *session, NODE_QUEUE *queue, WT_FREQTREE_NODE *node)
{
    NODE_QUEUE_ELEM *elem;

    /* Allocating a new linked list element */
    WT_RET(__wt_calloc_one(session, &elem));

    /* It holds the tree node, and has no next element yet */
    elem->node = node;
    elem->next = NULL;

    /* If the queue is empty, the first element will be the new one. */
    if (queue->first == NULL)
        queue->first = elem;

    /*
     * If the queue is not empty, the last element's next pointer must be updated.
     */
    if (queue->last != NULL)
        queue->last->next = elem;

    /* The last element is the new one */
    queue->last = elem;

    return (0);
}

/*
 * node_queue_dequeue --
 *     Removes a node from the beginning of the queue and copies the node's pointer to the location
 *     referred by the retp parameter.
 */
static void
node_queue_dequeue(WT_SESSION_IMPL *session, NODE_QUEUE *queue, WT_FREQTREE_NODE **retp)
{
    NODE_QUEUE_ELEM *first_elem;

    /*
     * Getting the first element of the queue and updating it to point to the next element as first.
     */
    first_elem = queue->first;
    *retp = first_elem->node;
    queue->first = first_elem->next;

    /*
     * If the last element was the dequeued element, we have to update it to NULL.
     */
    if (queue->last == first_elem)
        queue->last = NULL;

    /* Freeing the linked list element that has been dequeued */
    __wt_free(session, first_elem);
}
