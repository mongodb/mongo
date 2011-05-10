/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#define	HEADER 3

typedef struct __wt_freqtree_node {
	/*
	 * Data structure representing a node of the huffman tree. It holds a
	 * 32-bit weight and pointers to the left and right child nodes.  The
	 * node either has two child nodes or none.
	 */
	uint16_t symbol;			/* only used in leaf nodes */
	uint32_t weight;
	struct __wt_freqtree_node *left;	/* bit 0 */
	struct __wt_freqtree_node *right;	/* bit 1 */
} WT_FREQTREE_NODE;

typedef struct __wt_static_huffman_node {
	/*
	 * This data structure is used to represent the huffman tree in a static
	 * array, after it has been created (using a dynamic tree representation
	 * with WT_FREQTREE_NODE nodes).
	 *
	 * In the binary tree's array representation if a node's index is i,
	 * then its left child node is 2i+1 and its right child node is 2i+2.
	 */
	uint8_t valid;
	uint16_t symbol;
	uint16_t codeword_length;
} WT_STATIC_HUFFMAN_NODE;

typedef struct __wt_huffman_table_entry {
	uint16_t code;

	/*
	 * Lengths usually range from 1-to-teens, 32 is big, 255 is pathological
	 * worst case in which case you have other problems.
	 */
	uint8_t codeword_length;
} WT_HUFFMAN_TABLE_ENTRY;

typedef struct __wt_huffman_obj {
	SESSION *session;		/* Enclosing session */
	/*
	 * Data structure here defines specific instance of the encoder/decoder.
	 * This contains the frequency table (tree) used to produce optimal
	 * results.  This version of the encoder supports 1- and 2-byte symbols.
	 */
	uint32_t numSymbols;
	uint8_t  numBytes;		/* 1 or 2 */
					/* Tree in static array reprentation */
	WT_STATIC_HUFFMAN_NODE *nodes;
	uint16_t max_depth;

	/*
	 * Table-based Huffman:
	 * data structures:
	 *	symbols[0-to-(number of symbols - 1)]
 *
 * Tom: where is "symbols"?
 *
	 *	c2e[1 << max_depth] (code to entry in symbols)
	 *
	 * decoding use:
	 *	c2e[code] = index of struct with symbol and length
	 * encoding use:
	 *	code = symbols[symbol].code
	 *
	 * c2e[Huffman code] = entry index.  Always 16-bit for simplicity,
	 * if <=256 elements then waste 256 bytes, otherwise need them.
	 */
	uint16_t *c2e;
					/* Table entry array, packed tight */
	WT_HUFFMAN_TABLE_ENTRY *entries;
} WT_HUFFMAN_OBJ;

/*
 * Queue element data structure.
 *
 * Consists of a pointer to a huffman tree node, and a pointer to the next
 * element in the queue.
 */
typedef struct node_queue_elem {
	WT_FREQTREE_NODE *node;
	struct node_queue_elem *next;
} NODE_QUEUE_ELEM;

/*
 * Queue of huffman tree nodes.
 *
 * Contains a pointer to the beginning and the end of the queue, which is
 * implemented as a linked list.
 */
typedef struct node_queue {
	NODE_QUEUE_ELEM *first;
	NODE_QUEUE_ELEM *last;
} NODE_QUEUE;

/*
 * Internal data structure used to preserve the symbol when rearranging the
 * frequency array.
 */
typedef struct __indexed_byte {
	uint8_t frequency;
	uint16_t symbol;
} INDEXED_BYTE;

static int  indexed_byte_comparator(const void *, const void *);
static void make_table(uint16_t *, uint16_t, WT_HUFFMAN_TABLE_ENTRY *, u_int);
static void node_queue_close(SESSION *, NODE_QUEUE *);
static void node_queue_dequeue(SESSION *, NODE_QUEUE *, WT_FREQTREE_NODE **);
static int  node_queue_enqueue(SESSION *, NODE_QUEUE *, WT_FREQTREE_NODE *);
static void recursive_free_node(SESSION *, WT_FREQTREE_NODE *);
static void set_codes(WT_FREQTREE_NODE *,
	WT_HUFFMAN_TABLE_ENTRY *, uint32_t, uint16_t, uint16_t *);

#define	node_queue_is_empty(queue)					\
	(((queue) == NULL || (queue)->first == NULL) ? 1 : 0)

/*
 * Comparator function used by QuickSort to order the frequency table by
 * frequency (most frequent symbols will be at the end of the array).
 */
static int
indexed_byte_comparator(const void *elem1, const void *elem2)
{
	return (((INDEXED_BYTE *)
	    elem1)->frequency) - (((INDEXED_BYTE *)elem2)->frequency);
}

/*
 * set_codes --
 *	Tom: Please fill this in.
 */
static void
set_codes(WT_FREQTREE_NODE *node, WT_HUFFMAN_TABLE_ENTRY *entries,
    uint32_t code, uint16_t len, uint16_t *max_depth)
{
	WT_HUFFMAN_TABLE_ENTRY *entry;

	if (node->left == NULL && node->right == NULL) {
		entry = &entries[node->symbol];
		/*
		 * Tom:
		 * Casts from 32-bits to 16-bits, and 16-bits to 8-bits?
		 */
		entry->code = (uint16_t)code;
		entry->codeword_length = (uint8_t)len;
		if (*max_depth < len)
			*max_depth = len;
	} else {
		if (node->left != NULL)
			set_codes(node->left,
			    entries, (code << 1), len + 1, max_depth);
		if (node->right != NULL)
			set_codes(node->right,
			   entries, ((code << 1) | 1), len + 1, max_depth);
	}
}

/*
 * make_table --
 *	Tom: Please fill this in.
 */
static void
make_table(uint16_t *c2e,
    uint16_t max_depth, WT_HUFFMAN_TABLE_ENTRY *entries, u_int nbytes)
{
	uint16_t c, c1, c2, i, j, len, shift;

	/*
	 * Here's the magic: flood all bit patterns for lower-order bits to
	 * point to same entry.
	 */
	for (i = 0; i < nbytes; i++) {
		if ((len = entries[i].codeword_length) == 0)
			continue;

		/*
		 * Tom:
		 * is making all of these uint16_t types safe?
		 */
		c = entries[i].code;
		shift = max_depth - len;
		c1 = c << shift;
		c2 = (c + 1) << shift;
		for (j = c1; j < c2; j++)
			c2e[j] = i;		/* index which is also symbol */
	}
}

/*
 * recursive_free_node --
 *	Recursively free the huffman frequency tree's nodes.
 */
static void
recursive_free_node(SESSION *session, WT_FREQTREE_NODE *node)
{
	if (node != NULL) {
		recursive_free_node(session, node->left);
		recursive_free_node(session, node->right);
		__wt_free(session, node);
	}
}

/*
 * __wt_huffman_open --
 *	Take a frequency table and return a pointer to a descriptor object.
 *
 *  The frequency table must be the full range of valid values.  For 1 byte
 *  tables there are 256 values in 8 bits.  The highest rank is 255, and the
 * lowest rank is 1 (0 means the byte never appears in the input), so 1 byte
 * is needed to hold the rank and the input table must be 1 byte x 256 values.
 *
 *  For UTF-16 (nbytes == 2) the range is 0 - 65535 and the max rank is 65535.
 *  The table should be 2 bytes x 65536 values.
 */
int
__wt_huffman_open(SESSION *session,
    uint8_t const *byte_frequency_array, u_int nbytes, void *retp)
{
	INDEXED_BYTE *indexed_freqs;
	NODE_QUEUE *combined_nodes, *leaves;
	WT_FREQTREE_NODE *node, *node2, **refnode, *tempnode;
	WT_HUFFMAN_OBJ *huffman;
	uint32_t w1, w2;
	uint16_t i;
	int ret;

	indexed_freqs = NULL;
	combined_nodes = leaves = NULL;
	node = node2 = tempnode = NULL;
	ret = 0;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_HUFFMAN_OBJ), &huffman));
	WT_ERR(__wt_calloc(
	    session, (size_t)nbytes, sizeof(INDEXED_BYTE), &indexed_freqs));
	huffman->session = session;

	/*
	 * The frequency array must be sorted to be able to use linear time
	 * construction algorithm.
	 */
	for (i = 0; i < nbytes; ++i) {
		indexed_freqs[i].frequency = byte_frequency_array[i];
		indexed_freqs[i].symbol = i;
	}

	qsort(indexed_freqs,
	    nbytes, sizeof(INDEXED_BYTE), indexed_byte_comparator);

	/* We need two node queues to build the tree. */
	WT_ERR(__wt_calloc(session, 1, sizeof(NODE_QUEUE), &leaves));
	WT_ERR(__wt_calloc(session, 1, sizeof(NODE_QUEUE), &combined_nodes));

	/* Adding the leaves to the queue */
	for (i = 0; i < nbytes; ++i) {
		/*
		 * We are leaving out symbols with a frequency of 0.  This
		 * assumes these symbols will NEVER occur in the source stream,
		 * and the purpose is to reduce the huffman tree's size.
		 *
		 * NOTE: Even if this behavior is not desired, the frequencies
		 * should have a range between 1 - 255, otherwise the algorithm
		 * cannot produce well balanced tree; so this can be treated as
		 * an optional feature.
		 */
		if (indexed_freqs[i].frequency > 0) {
			WT_ERR(__wt_calloc(
			    session, 1, sizeof(WT_FREQTREE_NODE), &tempnode));
			tempnode->symbol = indexed_freqs[i].symbol;
			tempnode->weight = indexed_freqs[i].frequency;
			WT_ERR(node_queue_enqueue(session, leaves, tempnode));
			tempnode = NULL;
		}
	}

	while (!node_queue_is_empty(leaves) ||
	    !node_queue_is_empty(combined_nodes)) {
		/*
		 * We have to get the node with the smaller weight, examining
		 * both queues first element.  We are collecting pairs of these
		 * items, by alternating between node and node2:
		 */
		refnode = !node ? &node : &node2;

		/*
		 * To decide which queue must be used, we get the weights of
		 * the first items from both:
		 */
		w1 = node_queue_is_empty(leaves) ?
		    UINT32_MAX : leaves->first->node->weight;
		w2 = node_queue_is_empty(combined_nodes) ?
		    UINT32_MAX : combined_nodes->first->node->weight;

		/*
		 * Based on the two weights we finally can dequeue the smaller
		 * element and place it to the alternating target node pointer:
		 */
		if (w1 < w2)
			node_queue_dequeue(session, leaves, refnode);
		else
			node_queue_dequeue(session, combined_nodes, refnode);

		/*
		 * In every second run, we have both node and node2 initialized.
		 */
		if (node != NULL && node2 != NULL) {
			WT_ERR(__wt_calloc(
			    session, 1, sizeof(WT_FREQTREE_NODE), &tempnode));

			/* The new weight is the sum of the two weights. */
			tempnode->weight = node->weight + node2->weight;
			tempnode->left = node;
			tempnode->right = node2;

			/* Enqueue it to the combined nodes queue */
			WT_ERR(node_queue_enqueue(
			    session, combined_nodes, tempnode));
			tempnode = NULL;

			/* Reset the state pointers */
			node = node2 = NULL;
		}
	}

	/*
	 * The remaining node is in the node variable, this is the root of the
	 * tree.   Calculate the number of bytes it takes to hold nbytes bits.
	 */
	huffman->numSymbols = nbytes;
	huffman->numBytes = nbytes > 256 ? 2 : 1;

	WT_ERR(__wt_calloc(session,
	    nbytes, sizeof(WT_HUFFMAN_TABLE_ENTRY), &huffman->entries));
	set_codes(node, huffman->entries, 0, 0, &huffman->max_depth);

	WT_ERR(__wt_calloc(session,
	    1U << huffman->max_depth, sizeof(uint16_t), &huffman->c2e));
	make_table(huffman->c2e, huffman->max_depth, huffman->entries, nbytes);
#if 1
	printf("max depth = %d, memory use: entries %d * %u + c2e %d * %u\n",
	    huffman->max_depth, nbytes, sizeof(WT_HUFFMAN_TABLE_ENTRY),
	    (1U << huffman->max_depth), sizeof(uint16_t));
#endif

	*(void **)retp = huffman;

err:	if (leaves != NULL)
		node_queue_close(session, leaves);
	if (combined_nodes != NULL)
		node_queue_close(session, combined_nodes);
	if (indexed_freqs != NULL)
		__wt_free(session, indexed_freqs);
	if (node != NULL)
		recursive_free_node(session, node);
	if (node2 != NULL)
		recursive_free_node(session, node2);
	if (tempnode != NULL)
		__wt_free(session, tempnode);
	if (ret != 0) {
		if (huffman->nodes != NULL)
			__wt_free(session, huffman->nodes);
		__wt_free(session, huffman);
	}
	return (ret);
}

/*
 * __wt_huffman_close --
 *	Discard a Huffman descriptor object.
 */
void
__wt_huffman_close(SESSION *session, void *huffman_arg)
{
	WT_HUFFMAN_OBJ *huffman;

	huffman = huffman_arg;

	__wt_free(session, huffman->nodes);
	__wt_free(session, huffman->c2e);
	__wt_free(session, huffman->entries);
	__wt_free(session, huffman);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_print_huffman_code --
 *	Prints a symbol's huffman code. Can be used for debugging purposes.
 */
int
__wt_print_huffman_code(SESSION *session, void *huffman_arg, uint16_t symbol)
{
	WT_HUFFMAN_OBJ *huffman;
	WT_STATIC_HUFFMAN_NODE *node;
	u_int i, n;
	int p;
	char *buffer;

	huffman = huffman_arg;

	/* Check if the symbol is in valid range */
	if (symbol < huffman->numSymbols) {
		WT_RET(__wt_calloc(session, huffman->max_depth, 1, &buffer));

		node = NULL;
		for (i = 0, n = 1U << huffman->max_depth; i < n; ++i) {
			node = &huffman->nodes[i];
			if (node->valid &&
			    node->symbol == symbol && node->codeword_length > 0)
				break;
		}

		if (node != NULL) {
			/*
			 * We've got the leaf node, at index 'i'.  Now we fill
			 * the output buffer in back order.
			 */
			for (p = node->codeword_length - 1; p >= 0; --p) {
				buffer[p] = (i % 2) == 1 ? '0' : '1';
				i = (i - 1) / 2;
			}

			(void)printf("%s\n", buffer);
		} else {
			(void)printf(
			    "Symbol is not in the huffman tree: %x\n", symbol);
			return (WT_ERROR);
		}

		__wt_free(session, buffer);
	} else
		(void)printf("Symbol out of range: %lu >= %lu\n",
		    (u_long)symbol, (u_long)huffman->numSymbols);
	return (0);
}
#endif

/*
 * __wt_huffman_encode --
 *	Take a byte string, encode it into the target.
 */
int
__wt_huffman_encode(void *huffman_arg,
    const uint8_t *from, uint32_t from_len, WT_BUF *to_buf)
{
	SESSION *session;
	WT_HUFFMAN_OBJ *huffman;
	uint32_t bitpos, bytes;
	uint16_t symbol;
	uint8_t len, padding_info, *to, *out;
	int bits, code, valid;		/* Tom: "int" is correct? */

	huffman = huffman_arg;
	session = huffman->session;

	/*
	 * We don't want to find all of our callers and ensure they don't pass
	 * 0-length byte strings, but there's no reason to do any work.
	 */
	if (from_len == 0) {
		to_buf->size = 0;
		return (0);
	}

	/*
	 * We need N+1 bytes to encode N bytes, re-allocate as necessary.
	 *
	 * If the initial target pointer, or the initial target buffer length,
	 * aren't set, it's an allocation.   Clear the initial target pointer,
	 * our caller may have only set the initial target buffer length, not
	 * the initial pointer value.
	 */
	WT_RET(__wt_buf_setsize(session, to_buf, from_len + 1));
	to = to_buf->mem;
	memset(to, 0, from_len + 1);

	/*
	 * Leave the first 3 bits of the encoded value empty, it holds the
	 * number of bits actually used in the last byte of the encoded value.
	 */
	bits = 0;
	bitpos = HEADER;
	valid = HEADER;
	out = to;
	for (bytes = 0; bytes < from_len; bytes += huffman->numBytes) {
		/* Getting the next symbol, either 1 or 2 bytes */
		if (huffman->numBytes == 1)
			symbol = *from++;
		else {
			symbol = ((uint16_t)(*from++)) << 8;
			symbol |= *from++;
		}

		code = huffman->entries[symbol].code;
		len = huffman->entries[symbol].codeword_length;
		bits = (bits << len) | code;
		valid += len;
		bitpos += len;
		while (valid >= 8) {
			*out++ = (uint8_t)(bits >> (valid - 8));
			valid -= 8;
		}
	}
	if (valid > 0)
		*out = (uint8_t)(bits << (8 - valid));

	/*
	 * At this point, bitpos is the total number of used bits (including
	 * the 3 bits at the beginning of the buffer, which we'll set now to
	 * the number of bits used in the last byte).   Note if the number of
	 * bits used in the last byte is 8, we set the 3 bits to 0, in other
	 * words, the first 3 bits of the encoded value are the number of bits
	 * used in the last byte, unless they're 0, in which case there are 8
	 * bits used in the last byte.
	 */
	padding_info = (bitpos % 8) << (8 - HEADER);
	*to |= padding_info;

	to_buf->size = bitpos / 8 + ((bitpos % 8) ? 1 : 0);
	return (0);
}

/*
 * __wt_huffman_decode --
 *	Take a byte string, decode it into the target.
 */
int
__wt_huffman_decode(void *huffman_arg,
    const uint8_t *from, uint32_t from_len, WT_BUF *to_buf)
{
	SESSION *session;
	WT_HUFFMAN_OBJ *huffman;
	uint32_t bits, bytes, code, from_bits, from_len_bits, len, max;
	uint32_t mmask, out_bits, valid;
	uint16_t symbol;
	uint8_t padding_info, *to;

	huffman = huffman_arg;
	session = huffman->session;

	/*
	 * We don't want to find all of our callers and ensure they don't pass
	 * 0-length byte strings, but there's no reason to do any work.
	 */
	if (from_len == 0) {
		to_buf->size = 0;
		return (0);
	}

	/*
	 * We need 2N+1 bytes to decode N bytes, re-allocate as necessary.
	 *
	 * If the initial target pointer, or the initial target buffer length,
	 * aren't set, it's an allocation.   Clear the initial target pointer,
	 * our caller may have only set the initial target buffer length, not
	 * the initial pointer value.
	 */
	WT_RET(__wt_buf_setsize(session, to_buf, 2 * from_len + 1));
	to = to_buf->mem;

	/*
	 * The first 3 bits are the number of used bits in the last byte, unless
	 * they're 0, in which case there are 8 bits used in the last byte.
	 */
	padding_info = (*from & 0xE0) >> 5;
	from_len_bits = from_len * 8;
	if (padding_info != 0)
		from_len_bits -= 8 - padding_info;

	bits = *from++;
	valid = 8 - HEADER;
	from_bits = from_len_bits - valid;
	out_bits = from_len_bits - HEADER;
	max = huffman->max_depth;
	mmask = (1 << max) - 1;
	for (bytes = 0; out_bits > 0; bytes += huffman->numBytes) {
		while (valid < max && from_bits > 0) {
			bits = (bits << 8) | *from++;
			valid += 8;
			from_bits -= 8;
		}
		code = valid >= max ?		/* short codes near end */
		    (bits >> (valid - max)) : (bits << (max - valid));
		symbol = huffman->c2e[code & mmask];
		if (huffman->numBytes == 2)
			*to++ = (symbol >> 8);
		*to++ = (uint8_t)symbol;
		len = huffman->entries[symbol].codeword_length;
		valid -= len;
		out_bits -= len;
	}

	/* Return the number of bytes used. */
	to_buf->size = bytes;

	return (0);
}

/*
 * node_queue_close --
 *	Delete a queue from memory.
 *
 * It does not delete the pointed huffman tree nodes!
 */
static void
node_queue_close(SESSION *session, NODE_QUEUE *queue)
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
 *	Push a tree node to the end of the queue.
 */
static int
node_queue_enqueue(SESSION *session, NODE_QUEUE *queue, WT_FREQTREE_NODE *node)
{
	NODE_QUEUE_ELEM *elem;

	/* Allocating a new linked list element */
	WT_RET(__wt_calloc(session, 1, sizeof(NODE_QUEUE_ELEM), &elem));

	/* It holds the tree node, and has no next element yet */
	elem->node = node;
	elem->next = NULL;

	/* If the queue is empty, the first element will be the new one. */
	if (queue->first == NULL)
		queue->first = elem;

	/*
	 * If the queue is not empty, the last element's next pointer must be
	 * updated.
	 */
	if (queue->last != NULL)
		queue->last->next = elem;

	/* The last element is the new one */
	queue->last = elem;

	return (0);
}

/*
 * node_queue_dequeue --
 *	Removes a node from the beginning of the queue and copies the node's
 *	pointer to the location referred by the retp parameter.
 */
static void
node_queue_dequeue(SESSION *session, NODE_QUEUE *queue, WT_FREQTREE_NODE **retp)
{
	NODE_QUEUE_ELEM *first_elem;

	/*
	 * Getting the first element of the queue and updating it to point to
	 * the next element as first.
	 */
	first_elem = queue->first;
	*retp = first_elem->node;
	queue->first = first_elem->next;

	/*
	 * If the last element was the dequeued element, we have to update it
	 * to NULL.
	 */
	if (queue->last == first_elem)
		queue->last = NULL;

	/* Freeing the linked list element that has been dequeued */
	__wt_free(session, first_elem);
}
