/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include "igzip_lib.h"
#include "huffman.h"
#include "huff_codes.h"
#include "encode_df.h"
#include "igzip_level_buf_structs.h"
#include "unaligned.h"

/* Avoid getting warnings on unused variables which might be used later */
#define MAYBE_UNUSED(x) ((void)x)

static inline void write_deflate_icf(struct deflate_icf *icf, uint32_t lit_len,
				     uint32_t lit_dist, uint32_t extra_bits)
{
	icf->lit_len = lit_len;
	icf->lit_dist = lit_dist;
	icf->dist_extra = extra_bits;
}

static inline void update_state(struct isal_zstream *stream, uint8_t * start_in,
				uint8_t * next_in, uint8_t * end_in,
				struct deflate_icf *start_out, struct deflate_icf *next_out,
				struct deflate_icf *end_out)
{
        MAYBE_UNUSED(start_out);
	struct level_buf *level_buf = (struct level_buf *)stream->level_buf;

	if (next_in - start_in > 0)
		stream->internal_state.has_hist = IGZIP_HIST;

	stream->next_in = next_in;
	stream->total_in += next_in - start_in;
	stream->internal_state.block_end = stream->total_in;
	stream->avail_in = end_in - next_in;

	level_buf->icf_buf_next = next_out;
	level_buf->icf_buf_avail_out = end_out - next_out;
}

void isal_deflate_icf_body_hash_hist_base(struct isal_zstream *stream)
{
	uint32_t literal, hash;
	uint8_t *start_in, *next_in, *end_in, *end, *next_hash;
	struct deflate_icf *start_out, *next_out, *end_out;
	uint16_t match_length;
	uint32_t dist;
	uint32_t code, code2, extra_bits;
	struct isal_zstate *state = &stream->internal_state;
	struct level_buf *level_buf = (struct level_buf *)stream->level_buf;
	uint16_t *last_seen = level_buf->hash_hist.hash_table;
	uint8_t *file_start = (uint8_t *) ((uintptr_t) stream->next_in - stream->total_in);
	uint32_t hist_size = state->dist_mask;
	uint32_t hash_mask = state->hash_mask;

	if (stream->avail_in == 0) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_FLUSH_READ_BUFFER;
		return;
	}

	start_in = stream->next_in;
	end_in = start_in + stream->avail_in;
	next_in = start_in;

	start_out = ((struct level_buf *)stream->level_buf)->icf_buf_next;
	end_out =
	    start_out + ((struct level_buf *)stream->level_buf)->icf_buf_avail_out /
	    sizeof(struct deflate_icf);
	next_out = start_out;

	while (next_in + ISAL_LOOK_AHEAD < end_in) {

		if (next_out >= end_out) {
			state->state = ZSTATE_CREATE_HDR;
			update_state(stream, start_in, next_in, end_in, start_out, next_out,
				     end_out);
			return;
		}

		literal = load_u32(next_in);
		hash = compute_hash(literal) & hash_mask;
		dist = (next_in - file_start - last_seen[hash]) & 0xFFFF;
		last_seen[hash] = (uint64_t) (next_in - file_start);

		/* The -1 are to handle the case when dist = 0 */
		if (dist - 1 < hist_size) {
			assert(dist != 0);

			match_length = compare258(next_in - dist, next_in, 258);

			if (match_length >= SHORTEST_MATCH) {
				next_hash = next_in;
#ifdef ISAL_LIMIT_HASH_UPDATE
				end = next_hash + 3;
#else
				end = next_hash + match_length;
#endif
				next_hash++;

				for (; next_hash < end; next_hash++) {
					literal = load_u32(next_hash);
					hash = compute_hash(literal) & hash_mask;
					last_seen[hash] = (uint64_t) (next_hash - file_start);
				}

				get_len_icf_code(match_length, &code);
				get_dist_icf_code(dist, &code2, &extra_bits);

				level_buf->hist.ll_hist[code]++;
				level_buf->hist.d_hist[code2]++;

				write_deflate_icf(next_out, code, code2, extra_bits);
				next_out++;
				next_in += match_length;

				continue;
			}
		}

		get_lit_icf_code(literal & 0xFF, &code);
		level_buf->hist.ll_hist[code]++;
		write_deflate_icf(next_out, code, NULL_DIST_SYM, 0);
		next_out++;
		next_in++;
	}

	update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);

	assert(stream->avail_in <= ISAL_LOOK_AHEAD);
	if (stream->end_of_stream || stream->flush != NO_FLUSH)
		state->state = ZSTATE_FLUSH_READ_BUFFER;

	return;

}

void isal_deflate_icf_finish_hash_hist_base(struct isal_zstream *stream)
{
	uint32_t literal = 0, hash;
	uint8_t *start_in, *next_in, *end_in, *end, *next_hash;
	struct deflate_icf *start_out, *next_out, *end_out;
	uint16_t match_length;
	uint32_t dist;
	uint32_t code, code2, extra_bits;
	struct isal_zstate *state = &stream->internal_state;
	struct level_buf *level_buf = (struct level_buf *)stream->level_buf;
	uint16_t *last_seen = level_buf->hash_hist.hash_table;
	uint8_t *file_start = (uint8_t *) ((uintptr_t) stream->next_in - stream->total_in);
	uint32_t hist_size = state->dist_mask;
	uint32_t hash_mask = state->hash_mask;

	start_in = stream->next_in;
	end_in = start_in + stream->avail_in;
	next_in = start_in;

	start_out = ((struct level_buf *)stream->level_buf)->icf_buf_next;
	end_out = start_out + ((struct level_buf *)stream->level_buf)->icf_buf_avail_out /
	    sizeof(struct deflate_icf);
	next_out = start_out;

	if (stream->avail_in == 0) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_CREATE_HDR;
		return;
	}

	while (next_in + 3 < end_in) {
		if (next_out >= end_out) {
			state->state = ZSTATE_CREATE_HDR;
			update_state(stream, start_in, next_in, end_in, start_out, next_out,
				     end_out);
			return;
		}

		literal = load_u32(next_in);
		hash = compute_hash(literal) & hash_mask;
		dist = (next_in - file_start - last_seen[hash]) & 0xFFFF;
		last_seen[hash] = (uint64_t) (next_in - file_start);

		if (dist - 1 < hist_size) {	/* The -1 are to handle the case when dist = 0 */
			match_length = compare258(next_in - dist, next_in, end_in - next_in);

			if (match_length >= SHORTEST_MATCH) {
				next_hash = next_in;
#ifdef ISAL_LIMIT_HASH_UPDATE
				end = next_hash + 3;
#else
				end = next_hash + match_length;
#endif
				next_hash++;

				for (; next_hash < end - 3; next_hash++) {
					literal = load_u32(next_hash);
					hash = compute_hash(literal) & hash_mask;
					last_seen[hash] = (uint64_t) (next_hash - file_start);
				}

				get_len_icf_code(match_length, &code);
				get_dist_icf_code(dist, &code2, &extra_bits);

				level_buf->hist.ll_hist[code]++;
				level_buf->hist.d_hist[code2]++;

				write_deflate_icf(next_out, code, code2, extra_bits);

				next_out++;
				next_in += match_length;

				continue;
			}
		}

		get_lit_icf_code(literal & 0xFF, &code);
		level_buf->hist.ll_hist[code]++;
		write_deflate_icf(next_out, code, NULL_DIST_SYM, 0);
		next_out++;
		next_in++;

	}

	while (next_in < end_in) {
		if (next_out >= end_out) {
			state->state = ZSTATE_CREATE_HDR;
			update_state(stream, start_in, next_in, end_in, start_out, next_out,
				     end_out);
			return;
		}

		literal = *next_in;
		get_lit_icf_code(literal & 0xFF, &code);
		level_buf->hist.ll_hist[code]++;
		write_deflate_icf(next_out, code, NULL_DIST_SYM, 0);
		next_out++;
		next_in++;

	}

	if (next_in == end_in) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_CREATE_HDR;
	}

	update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);

	return;
}

void isal_deflate_icf_finish_hash_map_base(struct isal_zstream *stream)
{
	uint32_t literal = 0, hash;
	uint8_t *start_in, *next_in, *end_in, *end, *next_hash;
	struct deflate_icf *start_out, *next_out, *end_out;
	uint16_t match_length;
	uint32_t dist;
	uint32_t code, code2, extra_bits;
	struct isal_zstate *state = &stream->internal_state;
	struct level_buf *level_buf = (struct level_buf *)stream->level_buf;
	uint16_t *last_seen = level_buf->hash_map.hash_table;
	uint8_t *file_start = (uint8_t *) ((uintptr_t) stream->next_in - stream->total_in);
	uint32_t hist_size = state->dist_mask;
	uint32_t hash_mask = state->hash_mask;

	start_in = stream->next_in;
	end_in = start_in + stream->avail_in;
	next_in = start_in;

	start_out = level_buf->icf_buf_next;
	end_out = start_out + level_buf->icf_buf_avail_out / sizeof(struct deflate_icf);
	next_out = start_out;

	if (stream->avail_in == 0) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_CREATE_HDR;
		return;
	}

	while (next_in + 3 < end_in) {
		if (next_out >= end_out) {
			state->state = ZSTATE_CREATE_HDR;
			update_state(stream, start_in, next_in, end_in, start_out, next_out,
				     end_out);
			return;
		}

		literal = load_u32(next_in);
		hash = compute_hash_mad(literal) & hash_mask;
		dist = (next_in - file_start - last_seen[hash]) & 0xFFFF;
		last_seen[hash] = (uint64_t) (next_in - file_start);

		if (dist - 1 < hist_size) {	/* The -1 are to handle the case when dist = 0 */
			match_length = compare258(next_in - dist, next_in, end_in - next_in);

			if (match_length >= SHORTEST_MATCH) {
				next_hash = next_in;
#ifdef ISAL_LIMIT_HASH_UPDATE
				end = next_hash + 3;
#else
				end = next_hash + match_length;
#endif
				next_hash++;

				for (; next_hash < end - 3; next_hash++) {
					literal = load_u32(next_hash);
					hash = compute_hash_mad(literal) & hash_mask;
					last_seen[hash] = (uint64_t) (next_hash - file_start);
				}

				get_len_icf_code(match_length, &code);
				get_dist_icf_code(dist, &code2, &extra_bits);

				level_buf->hist.ll_hist[code]++;
				level_buf->hist.d_hist[code2]++;

				write_deflate_icf(next_out, code, code2, extra_bits);

				next_out++;
				next_in += match_length;

				continue;
			}
		}

		get_lit_icf_code(literal & 0xFF, &code);
		level_buf->hist.ll_hist[code]++;
		write_deflate_icf(next_out, code, NULL_DIST_SYM, 0);
		next_out++;
		next_in++;

	}

	while (next_in < end_in) {
		if (next_out >= end_out) {
			state->state = ZSTATE_CREATE_HDR;
			update_state(stream, start_in, next_in, end_in, start_out, next_out,
				     end_out);
			return;
		}

		literal = *next_in;
		get_lit_icf_code(literal & 0xFF, &code);
		level_buf->hist.ll_hist[code]++;
		write_deflate_icf(next_out, code, NULL_DIST_SYM, 0);
		next_out++;
		next_in++;

	}

	if (next_in == end_in) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_CREATE_HDR;
	}

	update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);

	return;
}

void isal_deflate_hash_mad_base(uint16_t * hash_table, uint32_t hash_mask,
				uint32_t current_index, uint8_t * dict, uint32_t dict_len)
{
	uint8_t *next_in = dict;
	uint8_t *end_in = dict + dict_len - SHORTEST_MATCH;
	uint32_t literal;
	uint32_t hash;
	uint16_t index = current_index - dict_len;

	while (next_in <= end_in) {
		literal = load_u32(next_in);
		hash = compute_hash_mad(literal) & hash_mask;
		hash_table[hash] = index;
		index++;
		next_in++;
	}
}
