/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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

typedef struct __wt_huffman_table_entry {
	uint16_t code;	/* length of field's type in bits >= max_code_length */

	/*
	 * Lengths usually range from 1-to-teens, 32 is big, 255 is pathological
	 * worst case, in which case you have other problems.
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
	uint16_t max_depth, min_depth;

	/*
	 * Table-based Huffman:
	 * data structures (memory use):
	 *	entries[0-to-(number of symbols - 1)]
	 *	c2e[1 << max_depth] (code to entry in symbols)
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
	WT_HUFFMAN_TABLE_ENTRY *, uint16_t, uint8_t, uint16_t *, uint16_t *);

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
 *	Computes Huffman code for each symbol in tree, in standard way in the
 * literature.
 */
static void
set_codes(WT_FREQTREE_NODE *node, WT_HUFFMAN_TABLE_ENTRY *entries,
	uint16_t code, uint8_t len, uint16_t *max_depth, uint16_t *min_depth)
{
	WT_HUFFMAN_TABLE_ENTRY *entry;

	if (node->left == NULL && node->right == NULL) {
		entry = &entries[node->symbol];
		entry->code = code;
		entry->codeword_length = (uint8_t)len;
		if (*max_depth < len)
			*max_depth = len;
		if (*min_depth > len)
			*min_depth = len;
	} else {
		if (node->left != NULL)
			set_codes(node->left, entries,
				(code << 1), len + 1, max_depth, min_depth);
		if (node->right != NULL)
			set_codes(node->right, entries,
				(code << 1) | 1, len + 1, max_depth, min_depth);
	}
}

/*
 * make_table --
 *	Computes Huffman table used for subsequent lookups in encoding and
 * decoding.  With the table, encoding from a symbol to Huffman code and
 * decoding from a code to a symbol are simple array lookups.
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
		 * The size of the array index should be enough to hold largest
		 * index into symbol table.  Pre-existing symbols were packed
		 * 0-255 or 0-0xffff, so 16 bits is enough.  Don't want to make
		 * it larger than necessary, we allocate (2 ^ max-code-length)
		 * of them.
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
	uint32_t memuse, w1, w2;
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

	/*
	 * 1000, or ((1 << bits-in-codeword) - 1), but if >1000 you've got
	 * other problems.
	 */
	huffman->min_depth = 1000;
	set_codes(node,
	    huffman->entries, 0, 0, &huffman->max_depth, &huffman->min_depth);

	/*
	 * It's potentially possible, but vanishingly unlikely, that a table
	 * will be too large; before big memory allocation, see if would use
	 * too much memory.
	 */
#define	WT_HUFFMAN_MAX_MEMORY	 (256 * 1024)
	memuse = nbytes * WT_SIZEOF32(WT_HUFFMAN_TABLE_ENTRY) +
	    (1U << huffman->max_depth) * WT_SIZEOF32(uint16_t);
	if (memuse > WT_HUFFMAN_MAX_MEMORY) {
		__wt_errx(session,
		    "Huffman table would allocate too much memory, perhaps due "
		    "to a very long code (maximum depth is %lu)",
		    (u_long)huffman->max_depth);
		goto err;
	}
#if 0
	if (huffman->max_depth > sizeof(__wt_huffman_table_entry.code))
		printf("ERROR: code word %d > field in "
		    "WT_HUFFMAN_TABLE_ENTRY %d\n",
		    huffman->max_depth,
		    sizeof(__wt_huffman_table_entry.code));
#endif

	WT_ERR(__wt_calloc(session,
	    1U << huffman->max_depth, sizeof(uint16_t), &huffman->c2e));
	make_table(huffman->c2e, huffman->max_depth, huffman->entries, nbytes);
#if 0
	printf("max depth = %d, min_depth = %d, "
	    "memory use: entries %d * %u + c2e %d * %u\n",
	    huffman->max_depth, huffman->min_depth, nbytes,
	    sizeof(WT_HUFFMAN_TABLE_ENTRY), (1U << huffman->max_depth),
	    sizeof(uint16_t));
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
	if (ret != 0)
		__wt_free(session, huffman);
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

	__wt_free(session, huffman->c2e);
	__wt_free(session, huffman->entries);
	__wt_free(session, huffman);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_print_huffman_code --
 *	Print a symbol's Huffman code.
 */
void
__wt_print_huffman_code(void *huffman_arg, uint16_t symbol)
{
	WT_HUFFMAN_OBJ *huffman;
	WT_HUFFMAN_TABLE_ENTRY *e;

	huffman = huffman_arg;

	if (symbol >= huffman->numSymbols) {
		printf("symbol %d out of range\n", symbol);
	} else {
		e = &huffman->entries[symbol];
		if (e->codeword_length == 0)
			printf(
			    "symbol %d not defined -- 0 frequency\n", symbol);
		else
			/* Should translate code to binary. */
			printf("%d -> code %x, length %d\n",
			    symbol, e->code, e->codeword_length);
	}
}
#endif

/*
 * __wt_huffman_encode --
 *	Take a byte string, encode it into the target.
 *
 * Translation from symbol to Huffman code is a simple array lookup.
 *
 * WT_HUFFMAN_OBJ contains an array called entries with a WT_HUFFMAN_TABLE_ENTRY
 * per symbol.  Then, given a symbol:
 *
 *	code = entries[symbol].code;
 *	length = entries[symbol].codeword_length;
 *
 * To encode a byte-string message, iterate over the input symbols.  For each
 * symbol, look it up via table, shift bits onto a shift register (an int long
 * enough to hold the longest code word + up to 7 bits remaining from the
 * previous), then drain out full bytes.  Finally, at the end flush remaining
 * bits and write header bits.
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
	uint32_t code;

	/*
	 * Shift register to accumulate bits from input.  Should be >= length
	 * of max_code_word + 7, but also efficient to shift bits and preferably
	 * in a register.
	 */
	uint32_t bits;

	/* Number of valid bits in shift register ('bits'). */
	uint8_t valid;

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
	 * XXX
	 * THIS IS WRONG.  If the input consists entirely of the least frequent
	 * symbol (longest code word), we need:
	 *
	 *	max-code-length * symbols-in-input
	 *
	 * bits.
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
#define	WT_HUFFMAN_HEADER 3
	bits = 0;
	bitpos = WT_HUFFMAN_HEADER;
	valid = WT_HUFFMAN_HEADER;
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
	padding_info = (bitpos % 8) << (8 - WT_HUFFMAN_HEADER);
	*to |= padding_info;

	to_buf->size = bitpos / 8 + ((bitpos % 8) ? 1 : 0);
	return (0);
}

/*
 * __wt_huffman_decode --
 *	Take a byte string, decode it into the target.
 *
 * Translation from Huffman code to symbol is a simple array lookup.
 *
 * WT_HUFFMAN_OBJ contains an array called 'c2e' indexed by code word and whose
 * value is the corresponding symbol.  From the symbol, index into the entries
 * array to get the codeword length.
 *
 * When decoding a message, we don't know where the boundaries are between
 * codewords.  The trick is that we collect enough bits for the longest code
 * word, and construct the table such that for codes with fewer bits we flood
 * the table with all of the bit patterns in the lower order bits.  This works
 * because the Huffman code is a unique prefix, and by the flooding we are
 * treating bits beyond the unique prefix as don't care bits.
 *
 * For example, we have table of length:
 *	(2 ^ max-code-length (1 << max-code-length)
 *
 * For a code of length, max-code-length, the position c2e[code] = symbol.
 * For a code word of (max-length - 1), we fill c2e[code << 1] = symbol, as
 * well as c2e[(code << 1) | 1] = symbol.  And so on, so in general we will
 * fill c2e[(code) << shift inclusive ... (code + 1) << shift exclusive].
 *
 * To decode a message, we read in enough bits from input to fill the shift
 * register with at least max-code-length bits.  We look up in the table c2e.
 * This gives the symbol.  We look up the symbol in 'entries' to get the
 * codeword length, and subtract off these bits from the shift register.
 */
int
__wt_huffman_decode(void *huffman_arg,
    const uint8_t *from, uint32_t from_len, WT_BUF *to_buf)
{
	SESSION *session;
	WT_HUFFMAN_OBJ *huffman;
	uint32_t bits, bytes, code, from_bits, from_len_bits, len, max;
	uint32_t mmask, out_bits;
	uint8_t valid;
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
	 * XXX
	 * THIS IS WRONG.  If the input consists entirely of the shortest code
	 * word (more frequent symbol), we need a maximum of:
	 *
	 *	(bytes-per-symbol / min-code-length) * length-input
	 *
	 * bytes.
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
	valid = 8 - WT_HUFFMAN_HEADER;
	from_bits = from_len_bits - valid;
	out_bits = from_len_bits - WT_HUFFMAN_HEADER;
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
