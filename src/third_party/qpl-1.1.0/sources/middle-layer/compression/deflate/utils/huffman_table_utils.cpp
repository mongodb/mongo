/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "huffman_table_utils.hpp"


#include "compression/deflate/utils/compression_defs.hpp"
#include "common/bit_reverse.hpp"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstack-usage=4096"
#endif

extern "C" {
    extern void build_heap(uint64_t * heap, uint64_t heap_size);
    extern uint32_t build_huff_tree(struct heap_tree *heap, uint64_t heap_size, uint64_t node_index);
}



namespace qpl::ml::compression {
static inline auto write_rl(rl_code *pout,
                            uint16_t last_length,
                            uint32_t run_length,
                            uint64_t *counts) noexcept -> rl_code * {
    if (last_length == 0) {
        while (run_length > 138) {
            pout->code = 18;
            pout->extra_bits = 138 - 11;
            pout++;
            run_length -= 138;
            counts[18]++;
        }
        // 1 <= run_length <= 138
        if (run_length > 10) {
            pout->code = 18;
            pout->extra_bits = run_length - 11;
            pout++;
            counts[18]++;
        } else if (run_length > 2) {
            pout->code = 17;
            pout->extra_bits = run_length - 3;
            pout++;
            counts[17]++;
        } else if (run_length == 1) {
            pout->code = 0;
            pout->extra_bits = 0;
            pout++;
            counts[0]++;
        } else {
            assert(run_length == 2);
            pout[0].code = 0;
            pout[0].extra_bits = 0;
            pout[1].code = 0;
            pout[1].extra_bits = 0;
            pout += 2;
            counts[0] += 2;
        }
    } else {
        // last_length != 0
        pout->code = static_cast<uint8_t>(last_length);
        pout->extra_bits = 0;
        pout++;
        counts[last_length]++;
        run_length--;
        if (run_length != 0) {
            while (run_length > 6) {
                pout->code = 16;
                pout->extra_bits = 6 - 3;
                pout++;
                run_length -= 6;
                counts[16]++;
            }
            // 1 <= run_length <= 6
            switch (run_length) {
            case 1:
                pout->code = static_cast<uint8_t>(last_length);
                pout->extra_bits = 0;
                pout++;
                counts[last_length]++;
                break;
            case 2:
                pout[0].code = static_cast<uint8_t>(last_length);
                pout[0].extra_bits = 0;
                pout[1].code = static_cast<uint8_t>(last_length);
                pout[1].extra_bits = 0;
                pout += 2;
                counts[last_length] += 2;
                break;
            default:    // 3...6
                pout->code = 16;
                pout->extra_bits = run_length - 3;
                pout++;
                counts[16]++;
            }
        }
    }
    return pout;
}

static inline auto fix_code_lens(heap_tree *heap_space,
                                 uint32_t root_node,
                                 uint32_t *bl_count,
                                 uint32_t max_code_len) noexcept -> uint32_t {
    tree_node *tree           = heap_space->tree;
    uint64_t  *code_len_count = heap_space->code_len_count;

    uint32_t i        = 0;
    uint32_t j        = root_node;
    uint32_t k        = 0;
    uint32_t child    = 0;
    uint32_t  depth   = 0;
    uint32_t code_len = 0;

    // compute code lengths and code length counts
    for (i = root_node; i <= HEAP_TREE_NODE_START; i++) {
        child = tree[i].child;
        if (child > MAX_HISTHEAP_SIZE) {
            depth = 1 + tree[i].depth;

            tree[child].depth = depth;
            tree[child - 1].depth = depth;
        } else {
            tree[j++] = tree[i];
            depth = tree[i].depth;
            while (code_len < depth) {
                code_len++;
                code_len_count[code_len] = 0;
            }
            code_len_count[depth]++;
        }
    }

    if (code_len > max_code_len) {
        while (code_len > max_code_len) {
            assert(code_len_count[code_len] > 1);
            for (i = max_code_len - 1; i != 0; i--) {
                if (code_len_count[i] != 0) {
                    break;
                }
            }

            assert(i != 0);
            code_len_count[i]--;
            code_len_count[i + 1] += 2;
            code_len_count[code_len - 1]++;
            code_len_count[code_len] -= 2;
            if (code_len_count[code_len] == 0) {
                code_len--;
            }
        }

        bl_count[0] = 0;
        for (i = 1; i <= code_len; i++) {
            bl_count[i] = static_cast<uint32_t>(code_len_count[i]);
        }
        for (; i <= max_code_len; i++) {
            bl_count[i] = 0;
        }

        for (k = 1; code_len_count[k] == 0; k++);
        for (i = root_node; i < j; i++) {
            tree[i].depth = k;
            code_len_count[k]--;
            for (; code_len_count[k] == 0; k++);
        }
    } else {
        bl_count[0] = 0;
        for (i = 1; i <= code_len; i++) {
            bl_count[i] = static_cast<uint32_t>(code_len_count[i]);
        }
        for (; i <= max_code_len; i++) {
            bl_count[i] = 0;
        }
    }

    return j;
}

/* Init heap with the histogram, and return the histogram size */
auto init_heap32(heap_tree *heap_space,
                 uint32_t * histogram,
                 uint32_t hist_size) noexcept -> uint32_t {
    uint32_t heap_size = 0;

    memset(heap_space, 0, sizeof(heap_tree));

    for (uint32_t i = 0; i < hist_size; i++) {
        if (histogram[i] != 0) {
            heap_space->heap[++heap_size] = ((static_cast<uint64_t>(histogram[i])) << FREQ_SHIFT) | i;
        }
    }

    // make sure heap has at least two elements in it
    if (heap_size < 2) {
        if (heap_size == 0) {
            heap_space->heap[1] = 1ULL << FREQ_SHIFT;
            heap_space->heap[2] = (1ULL << FREQ_SHIFT) | 1;
            heap_size = 2;
        } else {
            // heap size == 1
            if (histogram[0] == 0)
                heap_space->heap[2] = 1ULL << FREQ_SHIFT;
            else
                heap_space->heap[2] = (1ULL << FREQ_SHIFT) | 1;
            heap_size = 2;
        }
    }

    build_heap(heap_space->heap, heap_size);

    return heap_size;
}

static inline auto init_heap64(heap_tree *heap_space, uint64_t *histogram, uint64_t hist_size) noexcept -> uint32_t {
    uint32_t heap_size = 0;

    memset(heap_space, 0, sizeof(heap_tree));

    heap_size = 0;
    for (uint64_t i = 0; i < hist_size; i++) {
        if (histogram[i] != 0) {
            heap_space->heap[++heap_size] = ((histogram[i]) << FREQ_SHIFT) | i;
        }
    }

    // make sure heap has at least two elements in it
    if (heap_size < 2) {
        if (heap_size == 0) {
            heap_space->heap[1] = 1ULL << FREQ_SHIFT;
            heap_space->heap[2] = (1ULL << FREQ_SHIFT) | 1;
            heap_size = 2;
        } else {
            // heap size == 1
            if (histogram[0] == 0)
                heap_space->heap[2] = 1ULL << FREQ_SHIFT;
            else
                heap_space->heap[2] = (1ULL << FREQ_SHIFT) | 1;
            heap_size = 2;
        }
    }

    build_heap(heap_space->heap, heap_size);

    return heap_size;
}

auto rl_encode(uint16_t * codes,
               uint32_t num_codes,
               uint64_t * counts,
               rl_code *out) noexcept -> uint32_t {
    uint32_t run_length  = 1;
    uint16_t last_length = codes[0];
    uint16_t length      = 0;

    rl_code *pout = out;

    for (uint32_t i = 1; i < num_codes; i++) {
        length = codes[i];
        if (length == last_length) {
            run_length++;
            continue;
        }
        pout = write_rl(pout, last_length, run_length, counts);
        last_length = length;
        run_length = 1;
    }
    pout = write_rl(pout, last_length, run_length, counts);

    return static_cast<uint32_t>(pout - out);
}

void generate_huffman_codes(heap_tree *heap_space,
                            uint32_t heap_size,
                            uint32_t *bl_count,
                            huff_code *codes,
                            uint32_t codes_count,
                            uint32_t max_code_len) noexcept {
    tree_node *tree = heap_space->tree;

    uint32_t root_node = HEAP_TREE_NODE_START;
    uint32_t node_index  = 0;
    uint32_t end_node  = 0;

    root_node = build_huff_tree(heap_space, heap_size, root_node);
    end_node  = fix_code_lens(heap_space, root_node, bl_count, max_code_len);

    memset(codes, 0, codes_count * sizeof(*codes));
    for (node_index = root_node; node_index < end_node; node_index++) {
        codes[tree[node_index].child].length = tree[node_index].depth;
    }

}

auto set_huffman_codes(huff_code *huff_code_table,
                       int table_length,
                       uint32_t *count) noexcept -> uint32_t {
    /* Uses the algorithm mentioned in the deflate standard, Rfc 1951. */
    int      i        = 0;
    uint16_t code     = 0;
    uint32_t max_code = 0;

    uint16_t next_code[MAX_HUFF_TREE_DEPTH + 1];

    next_code[0] = code;

    for (i = 1; i < MAX_HUFF_TREE_DEPTH + 1; i++) {
        next_code[i] = (next_code[i - 1] + count[i - 1]) << 1;
    }

    for (i = 0; i < table_length; i++) {
        if (huff_code_table[i].length != 0) {
            huff_code_table[i].code = reverse_bits(next_code[huff_code_table[i].length],
                                                   huff_code_table[i].length);
            next_code[huff_code_table[i].length] += 1;
            max_code = i;
        }
    }

    return max_code;
}

auto set_dist_huff_codes(huff_code *codes, uint32_t * bl_count) noexcept -> uint32_t {
    uint32_t code     = 0;
    uint32_t code_len = 0;
    uint32_t bits     = 0;
    uint32_t i        = 0;
    uint32_t max_code = 0;

    const uint32_t num_codes = DIST_LEN;

    uint32_t next_code[MAX_DEFLATE_CODE_LEN + 1];

    bl_count[0] = 0;
    for (bits = 1; bits <= MAX_HUFF_TREE_DEPTH; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }
    for (i = 0; i < num_codes; i++) {
        code_len = codes[i].length;
        if (code_len != 0) {
            codes[i].code = reverse_bits(next_code[code_len], code_len);
            codes[i].extra_bit_count = distance_code_extra_bits[i];
            next_code[code_len] += 1;
            max_code = i;
        }
    }
    return max_code;
}

auto create_huffman_header(BitBuf2 *header_bitbuf,
                           huff_code *lookup_table,
                           rl_code *huffman_rep,
                           uint16_t huffman_rep_length,
                           uint32_t end_of_block,
                           uint32_t hclen,
                           uint32_t hlit,
                           uint32_t hdist) noexcept -> int {
    /* hlit, hdist, hclen are as defined in the deflate standard, head is the
     * first three deflate header bits.*/
    int       i = 0;
    huff_code huffman_value;

    const uint32_t extra_bits[3] = { 2, 3, 7 };

    uint64_t bit_count = buffer_bits_used(header_bitbuf);
    uint64_t data      = (end_of_block ? 5 : 4) | (hlit << 3) | (hdist << 8) | (hclen << 13);
    data |= ((lookup_table[code_length_code_order[0]].length) << DYN_HDR_START_LEN);
    write_bits(header_bitbuf, data, DYN_HDR_START_LEN + 3);
    data = 0;
    for (i = hclen + 3; i >= 1; i--) {
        data = (data << 3) | lookup_table[code_length_code_order[i]].length;
    }

    write_bits(header_bitbuf, data, (hclen + 3) * 3);

    for (i = 0; i < huffman_rep_length; i++) {
        huffman_value = lookup_table[huffman_rep[i].code];

        write_bits(header_bitbuf,
                   static_cast<uint64_t>(huffman_value.code),
                   static_cast<uint32_t>(huffman_value.length));

        if (huffman_rep[i].code > 15) {
            write_bits(header_bitbuf,
                       static_cast<uint64_t>(huffman_rep[i].extra_bits),
                       static_cast<uint32_t>(extra_bits[huffman_rep[i].code - 16]));
        }
    }
    bit_count = buffer_bits_used(header_bitbuf) - bit_count;

    return static_cast<int>(bit_count);
}

auto create_header(BitBuf2 *header_bitbuf,
                   rl_code *huffman_rep,
                   uint32_t length,
                   uint64_t * histogram,
                   uint32_t hlit,
                   uint32_t hdist,
                   uint32_t end_of_block) noexcept -> int {
    int      i         = 0;
    uint32_t heap_size = 0;
    heap_tree heap_space;

    uint32_t code_len_count[MAX_HUFF_TREE_DEPTH + 1];
    huff_code lookup_table[HUFF_LEN];

    constexpr uint32_t max_code_leghth = 7;

    /* hlit, hdist, and hclen are defined in RFC 1951 page 13 */
    uint32_t hclen     = 0;
    uint64_t bit_count = 0;

    /* Create a huffman tree to encode run length encoded representation. */
    heap_size = init_heap64(&heap_space, histogram, HUFF_LEN);
    generate_huffman_codes(&heap_space,
                           heap_size,
                           code_len_count,
                           reinterpret_cast<huff_code *>(lookup_table),
                           HUFF_LEN,
                           max_code_leghth);
    set_huffman_codes(lookup_table, HUFF_LEN, code_len_count);

    /* Calculate hclen */
    for (i = CODE_LEN_CODES - 1; i > 3; i--) {    /* i must be at least 4 */
        if (lookup_table[code_length_code_order[i]].length != 0) {
            break;
        }
    }

    hclen = i - 3;

    /* Generate actual header. */
    bit_count = create_huffman_header(header_bitbuf,
                                      lookup_table,
                                      huffman_rep,
                                      length,
                                      end_of_block,
                                      hclen,
                                      hlit,
                                      hdist);

    return static_cast<int>(bit_count);
}

} // namespace qpl::ml::compression

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
