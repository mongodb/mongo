/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "igzip_lib.h"
#include "huff_codes.h"
#include "unaligned.h"

static inline void heapify(uint64_t * heap, uint64_t heap_size, uint64_t index)
{
	uint64_t child = 2 * index, tmp;
	while (child <= heap_size) {
		child = (heap[child] <= heap[child + 1]) ? child : child + 1;

		if (heap[index] > heap[child]) {
			tmp = heap[index];
			heap[index] = heap[child];
			heap[child] = tmp;
			index = child;
			child = 2 * index;
		} else
			break;
	}
}

void build_heap(uint64_t * heap, uint64_t heap_size)
{
	uint64_t i;
	heap[heap_size + 1] = -1;
	for (i = heap_size / 2; i > 0; i--)
		heapify(heap, heap_size, i);

}

uint32_t build_huff_tree(struct heap_tree *heap_space, uint64_t heap_size, uint64_t node_ptr)
{
	uint64_t *heap = (uint64_t *) heap_space;
	uint64_t h1, h2;

	while (heap_size > 1) {
		h1 = heap[1];
		heap[1] = heap[heap_size];
		heap[heap_size--] = -1;

		heapify(heap, heap_size, 1);

		h2 = heap[1];
		heap[1] = ((h1 + h2) & ~0xFFFFull) | node_ptr;

		heapify(heap, heap_size, 1);

		store_u16((uint8_t *) & heap[node_ptr], h1);
		store_u16((uint8_t *) & heap[node_ptr - 1], h2);
		node_ptr -= 2;

	}
	h1 = heap[1];
	store_u16((uint8_t *) & heap[node_ptr], h1);
	return node_ptr;
}
