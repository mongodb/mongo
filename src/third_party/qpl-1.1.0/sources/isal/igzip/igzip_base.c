/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include "igzip_lib.h"
#include "huffman.h"
#include "huff_codes.h"
#include "bitbuf2.h"

extern const struct isal_hufftables hufftables_default;

static inline void update_state(struct isal_zstream *stream, uint8_t * start_in,
				uint8_t * next_in, uint8_t * end_in)
{
	struct isal_zstate *state = &stream->internal_state;
	uint32_t bytes_written;

	if (next_in - start_in > 0)
		state->has_hist = IGZIP_HIST;

	stream->next_in = next_in;
	stream->total_in += next_in - start_in;
	stream->avail_in = end_in - next_in;

	bytes_written = buffer_used(&state->bitbuf);
	stream->total_out += bytes_written;
	stream->next_out += bytes_written;
	stream->avail_out -= bytes_written;

}

void isal_deflate_body_base(struct isal_zstream *stream)
{
	uint32_t literal, hash;
	uint8_t *start_in, *next_in, *end_in, *end, *next_hash;
	uint16_t match_length;
	uint32_t dist;
	uint64_t code, code_len, code2, code_len2;
	struct isal_zstate *state = &stream->internal_state;
	uint16_t *last_seen = state->head;
	uint8_t *file_start = (uint8_t *) ((uintptr_t) stream->next_in - stream->total_in);
	uint32_t hist_size = state->dist_mask;
	uint32_t hash_mask = state->hash_mask;

	if (stream->avail_in == 0) {
		if (stream->end_of_stream || stream->flush != NO_FLUSH)
			state->state = ZSTATE_FLUSH_READ_BUFFER;
		return;
	}

	set_buf(&state->bitbuf, stream->next_out, stream->avail_out);

	start_in = stream->next_in;
	end_in = start_in + stream->avail_in;
	next_in = start_in;

	while (next_in + ISAL_LOOK_AHEAD < end_in) {

		if (is_full(&state->bitbuf)) {
			update_state(stream, start_in, next_in, end_in);
			return;
		}

		literal = load_u32(next_in);
		hash = compute_hash(literal) & hash_mask;
		dist = (next_in - file_start - last_seen[hash]) & 0xFFFF;
		last_seen[hash] = (uint64_t) (next_in - file_start);

		/* The -1 are to handle the case when dist = 0 */
#if defined(QPL_LIB)
		if ((dist - 1 < hist_size) && (dist < state->max_dist)) {
#else
		if (dist - 1 < hist_size) {
#endif
			assert(dist != 0);

			match_length = compare258(next_in - dist, next_in, 258);
#if defined(QPL_LIB)
            if(match_length > state->mb_mask + 1 - state->max_dist)
                match_length = (state->mb_mask + 1 - state->max_dist);
#endif
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

				get_len_code(stream->hufftables, match_length, &code,
					     &code_len);
				get_dist_code(stream->hufftables, dist, &code2, &code_len2);

				code |= code2 << code_len;
				code_len += code_len2;

				write_bits(&state->bitbuf, code, code_len);

				next_in += match_length;
#if defined(QPL_LIB)
                state->max_dist += match_length;
                state->max_dist &= state->mb_mask;
#endif

				continue;
			}
		}

		get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
		write_bits(&state->bitbuf, code, code_len);
		next_in++;
#if defined(QPL_LIB)
        state->max_dist++;
        state->max_dist &= state->mb_mask;
#endif
	}

	update_state(stream, start_in, next_in, end_in);

	assert(stream->avail_in <= ISAL_LOOK_AHEAD);
	if (stream->end_of_stream || stream->flush != NO_FLUSH)
		state->state = ZSTATE_FLUSH_READ_BUFFER;

	return;

}

void isal_deflate_finish_base(struct isal_zstream *stream)
{
	uint32_t literal = 0, hash;
	uint8_t *start_in, *next_in, *end_in, *end, *next_hash;
	uint16_t match_length;
	uint32_t dist;
	uint64_t code, code_len, code2, code_len2;
	struct isal_zstate *state = &stream->internal_state;
	uint16_t *last_seen = state->head;
	uint8_t *file_start = (uint8_t *) ((uintptr_t) stream->next_in - stream->total_in);
	uint32_t hist_size = state->dist_mask;
	uint32_t hash_mask = state->hash_mask;

	set_buf(&state->bitbuf, stream->next_out, stream->avail_out);

	start_in = stream->next_in;
	end_in = start_in + stream->avail_in;
	next_in = start_in;

	if (stream->avail_in != 0) {
		while (next_in + 3 < end_in) {
			if (is_full(&state->bitbuf)) {
				update_state(stream, start_in, next_in, end_in);
				return;
			}

			literal = load_u32(next_in);
			hash = compute_hash(literal) & hash_mask;
			dist = (next_in - file_start - last_seen[hash]) & 0xFFFF;
			last_seen[hash] = (uint64_t) (next_in - file_start);

#if defined(QPL_LIB)
			if ((dist - 1 < hist_size) && (dist < state->max_dist)) {	/* The -1 are to handle the case when dist = 0 */
#else
			if (dist - 1 < hist_size) {	/* The -1 are to handle the case when dist = 0 */
#endif
				match_length =
				    compare258(next_in - dist, next_in, end_in - next_in);
#if defined(QPL_LIB)
                if (match_length > state->mb_mask + 1 - state->max_dist)
                    match_length = (state->mb_mask + 1 - state->max_dist);
#endif

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
						last_seen[hash] =
						    (uint64_t) (next_hash - file_start);
					}

					get_len_code(stream->hufftables, match_length, &code,
						     &code_len);
					get_dist_code(stream->hufftables, dist, &code2,
						      &code_len2);

					code |= code2 << code_len;
					code_len += code_len2;

					write_bits(&state->bitbuf, code, code_len);

					next_in += match_length;
#if defined(QPL_LIB)
                    state->max_dist += match_length;
                    state->max_dist &= state->mb_mask;
#endif

					continue;
				}
			}

			get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
			write_bits(&state->bitbuf, code, code_len);
			next_in++;
#if defined(QPL_LIB)
            state->max_dist++;
            state->max_dist &= state->mb_mask;
#endif

		}

		while (next_in < end_in) {
			if (is_full(&state->bitbuf)) {
				update_state(stream, start_in, next_in, end_in);
				return;
			}

			literal = *next_in;
			get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
			write_bits(&state->bitbuf, code, code_len);
			next_in++;
#if defined(QPL_LIB)
            state->max_dist++;
            state->max_dist &= state->mb_mask;
#endif
		}
	}

#if defined(QPL_LIB)
    if (!is_full(&state->bitbuf) &&
        stream->flush != QPL_PARTIAL_FLUSH &&
        stream->huffman_only_flag != 2u)
    {
#else
    if (!is_full(&state->bitbuf)) {
#endif
		get_lit_code(stream->hufftables, 256, &code, &code_len);
		write_bits(&state->bitbuf, code, code_len);
		state->has_eob = 1;

		if (stream->end_of_stream == 1)
			state->state = ZSTATE_TRL;
		else
			state->state = ZSTATE_SYNC_FLUSH;
	}
#if defined(QPL_LIB)
    else
        state->state = ZSTATE_BODY;
#endif

	update_state(stream, start_in, next_in, end_in);

	return;
}

void isal_deflate_hash_base(uint16_t * hash_table, uint32_t hash_mask,
			    uint32_t current_index, uint8_t * dict, uint32_t dict_len)
{
	uint8_t *next_in = dict;
	uint8_t *end_in = dict + dict_len - SHORTEST_MATCH;
	uint32_t literal;
	uint32_t hash;
	uint16_t index = current_index - dict_len;

	while (next_in <= end_in) {
		literal = load_u32(next_in);
		hash = compute_hash(literal) & hash_mask;
		hash_table[hash] = index;
		index++;
		next_in++;
	}
}
