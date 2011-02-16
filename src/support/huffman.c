/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 *   Huffman Encoder/Decoder v1.0
 *   Author Brian Pollack <brian@brians.com>
 */

#include "wt_internal.h"

typedef struct __wt_freqtree_node {
	/*
	 * Data structure representing a node of the huffman tree. It holds a
	 * 32-bit weight and pointers to the left and right child nodes.
	 * The node either has two child nodes or none.
	 */
	uint16_t symbol;			/* only used in leaf nodes */
	uint32_t weight;
	uint16_t codeword_length;
	struct __wt_freqtree_node *left;	/* bit 0 */
	struct __wt_freqtree_node *right;	/* bit 1 */
} WT_FREQTREE_NODE;

typedef struct __wt_static_huffman_node {
	/*
	 * This data structure is used to represent the huffman tree in a
	 * static array, after it has been created (using a dynamic tree
	 * representation with WT_FREQTREE_NODE nodes).
	 *
	 * In the binary tree's array representation if a node's index is i,
	 * then its left child node is 2i+1 and its right child node is 2i+2.
	 */
	uint8_t valid;
	uint16_t symbol;
	uint16_t codeword_length;
} WT_STATIC_HUFFMAN_NODE;

typedef struct __wt_huffman_obj {
	ENV *env;		/* Enclosing environment */
	/*
	 * Data structure here defines specific instance of the encoder/decoder.
	 * This contains the frequency table (tree) used to produce optimal
	 * results.  This version of the encoder supports 1- and 2-byte symbols.
	 */
	uint32_t numSymbols;
	uint8_t  numBytes;	/* 1 or 2 */
				/* The tree in static array reprentation */
	WT_STATIC_HUFFMAN_NODE *nodes;
	uint16_t max_depth;
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

#define	node_queue_is_empty(queue)					\
	(((queue) == NULL || (queue)->first == NULL) ? 1 : 0)

static void node_queue_close(ENV *, NODE_QUEUE *);
static void node_queue_dequeue(ENV *, NODE_QUEUE *, WT_FREQTREE_NODE **);
static int  node_queue_enqueue(ENV *, NODE_QUEUE *, WT_FREQTREE_NODE *);
static void recursive_free_node(ENV *env, WT_FREQTREE_NODE *node);

/*
 * The following macros are used by the encoder to write the buffer with bit
 * addressing.
 */
#undef	SET_BIT
#define	SET_BIT(ptr, pos)						\
	*((ptr) + ((pos) / 8)) |= 1 << (7 - ((pos) % 8))
#undef	CLEAR_BIT
#define	CLEAR_BIT(ptr, pos)						\
	*((ptr) + ((pos) / 8)) &= ~(uint8_t)(1 << (7 - ((pos) % 8)))
#undef	MODIFY_BIT
#define	MODIFY_BIT(ptr, pos, bit)					\
	if (bit)							\
		SET_BIT(ptr, pos);					\
	else								\
		CLEAR_BIT(ptr, pos);

/*
 * Internal data structure used to preserve the symbol when rearranging the
 * frequency array.
 */
typedef struct __indexed_byte {
	uint8_t frequency;
	uint16_t symbol;
} INDEXED_BYTE;

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
 * traverse_tree --
 *	Recursive function with dual functionality:
 *	- It sets the codeword_length field of each leaf node to the
 *	  appropriate value.
 *	- It finds the maximum depth of the tree.
 */
static void
traverse_tree(
    WT_FREQTREE_NODE *node, uint16_t current_length, uint16_t *max_depth)
{
	/* Recursively traverse the tree */
	if (node->left != NULL)
		traverse_tree(node->left, current_length + 1, max_depth);
	if (node->right != NULL)
		traverse_tree(node->right, current_length + 1, max_depth);

	/* If this is a leaf: */
	if (node->left == NULL && node->right == NULL) {
		/*
		 * Setting the leaf's codeword length (for inner nodes, it
		 * is always 0!)
		 */
		node->codeword_length = current_length;

		/* Store the new maximal depth. */
		if (*max_depth < current_length + 1)
			*max_depth = current_length + 1;
	}
}

/*
 * fill_static_representation --
 *	Recursive function that converts the huffman tree from its dynamic
 * representation to static tree representation, to a preallocated array.
 *
 * To know the required size of the array the traverse_tree function can be
 * used, determining the maximum depth N. Then the required array size is 2^N.
 */
static void
fill_static_representation(
    WT_STATIC_HUFFMAN_NODE *target, WT_FREQTREE_NODE *node, int idx)
{
	WT_STATIC_HUFFMAN_NODE *current_target;

	current_target = &target[idx];
	current_target->symbol = node->symbol;
	current_target->codeword_length = node->codeword_length;
	current_target->valid = 1;

	if (node->left != NULL)
		fill_static_representation(target, node->left, idx * 2 + 1);
	if (node->right != NULL)
		fill_static_representation(target, node->right, idx * 2 + 2);
}

/*
 * recursive_free_node --
 *	Recursively free the huffman frequency tree's nodes.
 */
static void
recursive_free_node(ENV *env, WT_FREQTREE_NODE *node)
{
	if (node != NULL) {
		recursive_free_node(env, node->left);
		recursive_free_node(env, node->right);
		__wt_free(env, node, sizeof(WT_FREQTREE_NODE));
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
__wt_huffman_open(ENV *env,
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

	WT_RET(__wt_calloc(env, 1, sizeof(WT_HUFFMAN_OBJ), &huffman));
	WT_ERR(__wt_calloc(env, nbytes, sizeof(INDEXED_BYTE), &indexed_freqs));
	huffman->env = env;

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
	WT_ERR(__wt_calloc(env, 1, sizeof(NODE_QUEUE), &leaves));
	WT_ERR(__wt_calloc(env, 1, sizeof(NODE_QUEUE), &combined_nodes));

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
			    env, 1, sizeof(WT_FREQTREE_NODE), &tempnode));
			tempnode->symbol = indexed_freqs[i].symbol;
			tempnode->weight = indexed_freqs[i].frequency;
			WT_ERR(node_queue_enqueue(env, leaves, tempnode));
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
			node_queue_dequeue(env, leaves, refnode);
		else
			node_queue_dequeue(env, combined_nodes, refnode);

		/*
		 * In every second run, we have both node and node2 initialized.
		 */
		if (node != NULL && node2 != NULL) {
			WT_ERR(__wt_calloc(
			    env, 1, sizeof(WT_FREQTREE_NODE), &tempnode));

			/* The new weight is the sum of the two weights. */
			tempnode->weight = node->weight + node2->weight;
			tempnode->left = node;
			tempnode->right = node2;

			/* Enqueue it to the combined nodes queue */
			WT_ERR(
			    node_queue_enqueue(env, combined_nodes, tempnode));
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

	/* Traverse the tree and set the code word length for each node. */
	traverse_tree(node, 0, &huffman->max_depth);

	/* Converting the tree to a static array representation. */
	WT_ERR(__wt_calloc(env, 1 << huffman->max_depth,
	    sizeof(WT_STATIC_HUFFMAN_NODE), &huffman->nodes));
	fill_static_representation(huffman->nodes, node, 0);

	*(void **)retp = huffman;

err:	if (leaves != NULL)
		node_queue_close(env, leaves);
	if (combined_nodes != NULL)
		node_queue_close(env, combined_nodes);
	if (indexed_freqs != NULL)
		__wt_free(env, indexed_freqs, 0);
	if (node != NULL)
		recursive_free_node(env, node);
	if (node2 != NULL)
		recursive_free_node(env, node2);
	if (tempnode != NULL)
		__wt_free(env, tempnode, sizeof(WT_FREQTREE_NODE));
	if (ret != 0) {
		if (huffman->nodes != NULL)
			__wt_free(env, huffman->nodes, 0);
		__wt_free(env, huffman, sizeof(WT_HUFFMAN_OBJ));
	}
	return (ret);
}

/*
 * __wt_huffman_close --
 *	Discard a Huffman descriptor object.
 */
void
__wt_huffman_close(ENV *env, void *huffman_arg)
{
	WT_HUFFMAN_OBJ *huffman;

	huffman = huffman_arg;

	__wt_free(env, huffman->nodes, 0);
	__wt_free(env, huffman, sizeof(WT_HUFFMAN_OBJ));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_print_huffman_code --
 *	Prints a symbol's huffman code. Can be used for debugging purposes.
 */
int
__wt_print_huffman_code(ENV *env, void *huffman_arg, uint16_t symbol)
{
	WT_HUFFMAN_OBJ *huffman;
	WT_STATIC_HUFFMAN_NODE *node;
	u_int i, n;
	int p;
	char *buffer;

	huffman = huffman_arg;

	/* Check if the symbol is in valid range */
	if (symbol < huffman->numSymbols) {
		WT_RET(__wt_calloc(env, huffman->max_depth, 1, &buffer));

		node = NULL;
		for (i = 0, n = 1 << huffman->max_depth; i < n; ++i) {
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

		__wt_free(env, buffer, 0);
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
    uint8_t *from, uint32_t from_len,
    void *top, uint32_t *to_len, uint32_t *out_bytes_used)
{
	ENV *env;
	WT_HUFFMAN_OBJ *huffman;
	WT_STATIC_HUFFMAN_NODE *node;
	uint32_t bitpos, i, n, j;
	uint16_t symbol;
	uint8_t padding_info, *to;
	int p;

	huffman = huffman_arg;
	env = huffman->env;

	/*
	 * We need N+1 bytes to encode N bytes, re-allocate as necessary.
	 *
	 * If the initial target pointer, or the initial target buffer length,
	 * aren't set, it's an allocation.   Clear the initial target pointer,
	 * our caller may have only set the initial target buffer length, not
	 * the initial pointer value.
	 */
	if (to_len == NULL || *to_len < from_len + 1) {
		if (to_len == NULL)
			*(void **)top = NULL;
		WT_RET(__wt_realloc(env, to_len, from_len + 1, top));
	}

	to = *(uint8_t **)top;
	memset(to, 0, from_len + 1);

	/*
	 * Leave the first 3 bits of the encoded value empty, it holds the
	 * number of bits actually used in the last byte of the encoded value.
	 */
	bitpos = 3;
	n = 1 << huffman->max_depth;
	for (i = 0; i < from_len; i += huffman->numBytes) {
		/* Getting the next symbol, either 1 or 2 bytes */
		if (huffman->numBytes == 1)
			symbol = *from++;
		else {
			symbol = ((uint16_t)(*from++)) << 8;
			symbol |= *from++;
		}

		/* Getting the symbol's huffman code from the table */
		node = NULL;
		for (j = 0; j < n; ++j) {
			node = &huffman->nodes[j];
			if (node->valid &&
			    node->symbol == symbol && node->codeword_length > 0)
				break;
		}

		if (node != NULL) {
			/*
			 * We've got the leaf node, at index 'j'.  Now we fill
			 * the output buffer in back order.
			 */
			for (p = node->codeword_length - 1; p >= 0; --p) {
				MODIFY_BIT(to, bitpos + (u_int)p, (j % 2) ^ 1);
				j = (j - 1) / 2;
			}

			bitpos += node->codeword_length;
		} else {
			__wt_api_env_errx(NULL,
			    "Huffman compression: there was a symbol in the "
			    "source originally declared with zero frequency; "
			    "undefined source symbol: %lu", (u_long)symbol);
			return (WT_ERROR);
		}
	}

	/*
	 * At this point, bitpos is the total number of used bits (including
	 * the 3 bits at the beginning of the buffer, which we'll set now to
	 * the number of bits used in the last byte).   Note if the number of
	 * bits used in the last byte is 8, we set the 3 bits to 0, in other
	 * words, the first 3 bits of the encoded value are the number of bits
	 * used in the last byte, unless they're 0, in which case there are 8
	 * bits used in the last byte.
	 */
	padding_info = (bitpos % 8) << 5;
	*to |= padding_info;

	*out_bytes_used = bitpos / 8 + ((bitpos % 8) ? 1 : 0);

	return (0);
}

/*
 * __wt_huffman_decode --
 *	Take a byte string, decode it into the target.
 */
int
__wt_huffman_decode(void *huffman_arg,
    uint8_t *from, uint32_t from_len,
    void *top, uint32_t *to_len, uint32_t *out_bytes_used)
{
	ENV *env;
	WT_HUFFMAN_OBJ *huffman;
	WT_STATIC_HUFFMAN_NODE* node;
	uint32_t bytes, i, from_len_bits, node_idx;
	uint8_t bitpos, mask, bit, padding_info, *to;

	huffman = huffman_arg;
	env = huffman->env;

	/*
	 * We need 2N+1 bytes to decode N bytes, re-allocate as necessary.
	 *
	 * If the initial target pointer, or the initial target buffer length,
	 * aren't set, it's an allocation.   Clear the initial target pointer,
	 * our caller may have only set the initial target buffer length, not
	 * the initial pointer value.
	 */
	if (to_len == NULL || *to_len < 2 * from_len + 1) {
		if (to_len == NULL)
			*(void **)top = NULL;
		WT_RET(__wt_realloc(env, to_len, 2 * from_len + 1, top));
	}

	to = *(uint8_t **)top;

	bitpos = 4;			/* Skipping the first 3 bits. */
	bytes = 0;
	node_idx = 0;

	/*
	 * The first 3 bits are the number of used bits in the last byte, unless
	 * they're 0, in which case there are 8 bits used in the last byte.
	 */
	padding_info = (*from & 0xE0) >> 5;
	from_len_bits = from_len * 8;
	if (padding_info != 0)
		from_len_bits -= 8 - padding_info;

	/*
	 * The loop will go through each bit of the source stream, its length
	 * is given in BITS!
	 */
	for (i = 3; i < from_len_bits; i++) {
		/* Extracting the current bit */
		mask = (uint8_t)(1 << bitpos);
		bit = (*from & mask);

		/*
		 * As we go through the bits, we also make steps in the huffman
		 * tree, originated from the root, toward the leaves.
		 */
		if (bit)
			node_idx = (node_idx * 2) + 2;
		else
			node_idx = (node_idx * 2) + 1;

		node = &huffman->nodes[node_idx];

		/* If this is a leaf, we've found a complete symbol. */
		if (node->valid && node->codeword_length > 0) {
			if (huffman->numBytes == 1)
				*to++ = (uint8_t)node->symbol;
			else {
				*to++ = (node->symbol & 0xFF00) >> 8;
				*to++ = node->symbol & 0xFF;
			}

			bytes += huffman->numBytes;
			node_idx = 0;
		}

		/* Moving forward one bit in the source stream. */
		if (bitpos > 0)
			bitpos--;
		else {
			bitpos = 7;
			from++;
		}
	}

	/* Return the number of bytes used. */
	*out_bytes_used = bytes;

	return (0);
}

/*
 * node_queue_close --
 *	Delete a queue from memory.
 *
 * It does not delete the pointed huffman tree nodes!
 */
static void
node_queue_close(ENV *env, NODE_QUEUE *queue)
{
	NODE_QUEUE_ELEM *elem, *next_elem;

	/* Freeing each element of the queue's linked list. */
	for (elem = queue->first; elem != NULL; elem = next_elem) {
		next_elem = elem->next;
		__wt_free(env, elem, sizeof(NODE_QUEUE_ELEM));
	}

	/* Freeing the queue record itself. */
	__wt_free(env, queue, sizeof(NODE_QUEUE));
}

/*
 * node_queue_enqueue --
 *	Push a tree node to the end of the queue.
 */
static int
node_queue_enqueue(ENV *env, NODE_QUEUE *queue, WT_FREQTREE_NODE *node)
{
	NODE_QUEUE_ELEM *elem;

	/* Allocating a new linked list element */
	WT_RET(__wt_calloc(env, 1, sizeof(NODE_QUEUE_ELEM), &elem));

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
node_queue_dequeue(ENV *env, NODE_QUEUE *queue, WT_FREQTREE_NODE **retp)
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
	__wt_free(env, first_elem, sizeof(NODE_QUEUE_ELEM));
}
