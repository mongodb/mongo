/*
**
**   Huffman Encoder/Decoder v1.0
**   gcc 4.0 - no external libraries requires
**   Author Brian Pollack <brian@brians.com>
**
*/

#ifndef __HUFFMAN_H
#define __HUFFMAN_H

#include <sys/types.h>
#include <stdlib.h>

typedef struct __wt_freqtree_node {
	/*
	  Data structure representing a node of the huffman tree. It holds a 32 bit
	  weight and pointers to the left and right child nodes. 
	  The node either has two child nodes or none.
	 */

	u_int16_t symbol; /* only used in leaf nodes */
	u_int32_t weight;
	u_int16_t codeword_length;
	struct __wt_freqtree_node* left; /* bit 0 */
	struct __wt_freqtree_node* right; /* bit 1 */
} WT_FREQTREE_NODE;

typedef struct __wt_static_huffman_node {
	/* 
	   This data structure is used to represent the huffman tree in a static
	   array, after it has been created (using a dynamic tree representation
	   with WT_FREQTREE_NODE nodes).
	   In the binary tree's array representation if a node's index is i,
	   then its left child node is 2i+1 and its right child node is 2i+2. 
	*/

	u_int8_t valid;
	u_int16_t symbol;
	u_int16_t codeword_length;
} WT_STATIC_HUFFMAN_NODE;

typedef struct __wt_huffman_obj {
	/* 
	Data structure here defines a specific instance of the encoder/decoder.  This 
	contains the frequency table (tree) used to produce optimal results.  This
	version of the encoder supports muilti-byte patterns. 
	*/

	u_int32_t	numSymbols;
	u_int8_t    numBytes;
    
	WT_STATIC_HUFFMAN_NODE* nodes; /* The tree in static array reprentation */
	u_int16_t max_depth;
} WT_HUFFMAN_OBJ;

/*
 * __wt_huffman_open --
 *	Take a frequency table and return a pointer to a descriptor object.
 *	Return 0 on success, 1 on error.
 *
 *  The frequency table must be the full range of valid values.  For 1 byte
 *  tables there are 256 values in 8 bits.  The highest rank is 256 so 1 byte
 *  is needed to hold the rank so the input table must be 
 *  1 byte x 256 values 
 *
 *  For unicode (nbytes == 2) the range is 0-65536 and the max rank is
 *  65536.   The table should be 2 bytes * 65536 values
 *
 */
int
__wt_huffman_open(u_int8_t *byte_frequency_array, int nbytes, WT_HUFFMAN_OBJ **retp);

/*
 * __wt_huffman_close --
 *	Discard a Huffman descriptor object.
 */
int
__wt_huffman_close(WT_HUFFMAN_OBJ *huffman);

/*
 * __wt_huffman_encode --
 *	Take a byte string, encode it into the target.
 *	Return 0 on success, 1 on error.
 */
int
__wt_huffman_encode(WT_HUFFMAN_OBJ *huffman,
		    u_int8_t *from, u_int32_t len_from, u_int8_t *to, u_int32_t len_to, u_int32_t *out_bytes_used);

/*
 *	Take a byte string, decode it into the target.
 *	Return 0 on success, 1 on error.
 * 
 *  the len_from value is also in bytes!  Encoded strings may be 0 padded 
 */
int
__wt_huffman_decode(WT_HUFFMAN_OBJ *huffman,
		    u_int8_t *from, u_int16_t len_from, u_int8_t *to, u_int32_t len_to, u_int32_t *out_bytes_used);

/*
 * Prints a symbol's huffman code. Can be used for debugging purposes.
 */
void
print_huffman_code(WT_HUFFMAN_OBJ *huffman, u_int16_t symbol);

#endif /* __HUFFMAN_H */

#ifndef __NODE_QUEUE_H
#define __NODE_QUEUE_H

#include "huffman.h"

/*
 * Queue element data structure.
 * 
 * Consists of a pointer to a huffman tree node, and
 * a pointer to the next element in the queue.
 */
typedef struct __node_queue_elem
{
	WT_FREQTREE_NODE* node;
	struct __node_queue_elem* next;
} NODE_QUEUE_ELEM;

/*
 * Queue of huffman tree nodes. 
 *
 * Contains a pointer to the beginning and the end
 * of the queue, which is implemented as a linked
 * list.
 */
typedef struct __node_queue
{
	NODE_QUEUE_ELEM* first;
	NODE_QUEUE_ELEM* last;
} NODE_QUEUE;

/* 
 * Creates a new queue.
 *
 * Returns 0 if succeeded, 1 if could not allocate memory.
 */
int 
__node_queue_open(NODE_QUEUE** retp);

/*
 * Delete a queue from memory. 
 * 
 * It does not delete the pointed huffman tree nodes!
 *
 * Returns 0 if succeeded.
 */
int
__node_queue_close(NODE_QUEUE* queue);

/*
 * Push a tree node to the end of the queue.
 *
 * Returns 0 if succeeded
 *         1 if failed to allocate memory
 *         2 if the given queue is NULL
 */
int 
__node_queue_enqueue(NODE_QUEUE* queue, WT_FREQTREE_NODE* node);

/*
 * Removes a node from the beginning of the queue
 * and copies the node's pointer to the location
 * referred by the retp parameter.
 *
 * Returns 0 if succeeded
           2 if the queue or the retp parameters are NULL
          -1 if the queue is empty
*/
int
__node_queue_dequeue(NODE_QUEUE* queue, WT_FREQTREE_NODE** retp);

/*
 * Non-zero if the queue is valid and has at least one element.
 */
int
__node_queue_is_empty(NODE_QUEUE* queue);

/*
 * Gets the first item in the queue without removing it.
 *
 * Returns 0 if succeeded
           2 if the queue or the retp parameters are NULL
          -1 if the queue is empty.
*/
int
__node_queue_get_first(NODE_QUEUE* queue, WT_FREQTREE_NODE** retp);

#endif /* __NODE_QUEUE_H */
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <memory.h>
#include "huffman.h"
#include "node_queue.h"

/* The following macros are used by the encoder to write the buffer
   with bit addressing */
#define SET_BIT(ptr, pos) *((ptr) + ((pos) / 8)) |= 1 << (7 - ((pos) % 8))
#define CLEAR_BIT(ptr, pos) *((ptr) + ((pos) / 8)) &= ~(1 << (7 - ((pos) % 8)))
#define MODIFY_BIT(ptr, pos, bit) if (bit) SET_BIT(ptr, pos); else CLEAR_BIT(ptr, pos);

/* Internal data structure used to preserve the
   symbol when rearranging the frequency array */
typedef struct __indexed_byte 
{
	u_int8_t frequency;
	u_int16_t symbol;
} INDEXED_BYTE;

/* Comparator function used by QuickSort to order
 * the frequency table by frequency (most frequent
 * symbols will be at the end of the array)
 */
int 
indexed_byte_comparator(const void* elem1, const void* elem2)
{
	return (((INDEXED_BYTE*)elem1)->frequency) - (((INDEXED_BYTE*)elem2)->frequency);
}


/* 
Recursive function with dual functionality:
- It sets the codeword_length field of each leaf node to the approrpriate value
- It finds the maximum depth of the tree
*/
void
traverse_tree(WT_FREQTREE_NODE* node, u_int16_t current_length, u_int16_t* max_depth)
{
	/* Recursively traverse the tree */
	if (node->left)
		traverse_tree(node->left, current_length+1, max_depth);
	if (node->right)
		traverse_tree(node->right, current_length+1, max_depth);

	/* If this is a leaf */
	if (!node->left && !node->right)
	{
		/* Setting the leaf's codeword length (for inner nodes, it is always 0!) */
		node->codeword_length = current_length;

		/* And storing the new maximal depth */
		if (*max_depth < (current_length+1))
			*max_depth = current_length + 1;
	}
}

/*
  Recursive function that converts the huffman tree from its dynamic representation to
  static tree representation, to a preallocated array. 

  To know the required size of the array the traverse_tree function can be used,
  determining the maximum depth N. Then the required array size is 2^N.
 */
void
fill_static_representation(WT_STATIC_HUFFMAN_NODE* target, WT_FREQTREE_NODE* node, int idx)
{
	WT_STATIC_HUFFMAN_NODE* current_target = &(target[idx]);
	current_target->symbol = node->symbol;
	current_target->codeword_length = node->codeword_length;
	current_target->valid = 1;
    
	if (node->left)
		fill_static_representation(target, node->left, idx*2+1);
	if (node->right)
		fill_static_representation(target, node->right, idx*2+2);
}

/* Recursively free the huffman frequency tree's nodes. */
void
recursive_free_node(WT_FREQTREE_NODE* node)
{
	if (node)
	{
		recursive_free_node(node->left);
		recursive_free_node(node->right);
		free(node);
	}
}

int
__wt_huffman_open(
    u_int8_t *byte_frequency_array, int nbytes, WT_HUFFMAN_OBJ **retp)
{
	WT_HUFFMAN_OBJ *huffman = calloc(1, sizeof(WT_HUFFMAN_OBJ));
	NODE_QUEUE* leaves;
	NODE_QUEUE* combined_nodes;
	WT_FREQTREE_NODE* node;
	WT_FREQTREE_NODE* node2;
	WT_FREQTREE_NODE** refnode;
	WT_FREQTREE_NODE* tempnode;
	INDEXED_BYTE* indexed_freqs;
	u_int32_t w1, w2;
	u_int32_t i;

	/* The frequency array must be sorted to be able to use linear time construction
	   algorithm 
	*/
	indexed_freqs = (INDEXED_BYTE*)calloc(nbytes, sizeof(INDEXED_BYTE));
	for (i = 0; i < nbytes; ++i)
	{
	     indexed_freqs[i].frequency = byte_frequency_array[i];
	     indexed_freqs[i].symbol = i;
	}

	qsort(indexed_freqs, nbytes, sizeof(INDEXED_BYTE), indexed_byte_comparator);

	/* We need two node queues to build the tree */
	__node_queue_open(&leaves);
	__node_queue_open(&combined_nodes);

	/* Adding the leaves to the queue */
	for (i = 0; i < nbytes; ++i)
	{
	     /* We are leaving out symbols that's frequency is 0.
		This assumes that these symbols will NEVER occur in the
		source stream, and the purpose is to reduce the huffman
		tree's size. 
		
		NOTE: Even if this behavior is not desired, the frequencies
		      should have a range between 1..255, otherwise the
		      algorithm cannot produce well balanced tree;
		      So this can be treated as an optional feature.
	     */
		if (indexed_freqs[i].frequency > 0)
		{
			node = (WT_FREQTREE_NODE*)calloc(1, sizeof(WT_FREQTREE_NODE));
			node->symbol = indexed_freqs[i].symbol;
			node->weight = indexed_freqs[i].frequency;

			__node_queue_enqueue(leaves, node);	     
		}
	}

	node = NULL;
	node2 = NULL;

	while (!__node_queue_is_empty(leaves) ||
	       !__node_queue_is_empty(combined_nodes))
	{
		/* We have to get the node with the smaller weight, examining
		both queues first element. We are collecting pairs of these
		items, by alternating between node and node2:*/
		refnode = !node ? &node : &node2;

		/* To decide which queue must be used, we get the weights of
		the first items from both:
		*/
		w1 = ULONG_MAX;
		w2 = ULONG_MAX;

		if (!__node_queue_is_empty(leaves))
		{
			__node_queue_get_first(leaves, &tempnode);
			w1 = tempnode->weight;
		}

		if (!__node_queue_is_empty(combined_nodes))
		{
			__node_queue_get_first(combined_nodes, &tempnode);
			w2 = tempnode->weight;
		}

		/* Based on the two weights we finally can dequeue the smaller element
		and place it to the alternating target node pointer:
		*/
		if (w1 < w2)
			__node_queue_dequeue(leaves, refnode);
		else
			__node_queue_dequeue(combined_nodes, refnode);

		/* In every second run, we have both node and node2 initialized */
		if (node && node2)
		{
			tempnode = (WT_FREQTREE_NODE*)calloc(1, sizeof(WT_FREQTREE_NODE));		  
			tempnode->weight = node->weight + node2->weight; /* The new weight is the sum of the two weights */
			tempnode->left = node;
			tempnode->right = node2;

			/* Enqueue it to the combined nodes queue */
			__node_queue_enqueue(combined_nodes, tempnode);

			/* Reset the state pointers */
			node = NULL;
			node2 = NULL;
		}
	}
	
	/* The remaining node is in the node variable, this is the root of the tree: */
	huffman->numSymbols = nbytes;
	huffman->numBytes = ceil((log(nbytes)/log(2)/8));

	/* Freeing the queues */
	__node_queue_close(leaves);
	__node_queue_close(combined_nodes);

	free(indexed_freqs);

	/* Traversing the tree and setting the code word length for each node */
	traverse_tree(node, 0, &huffman->max_depth);

	/* Converting the tree to a static array representation */	
	huffman->nodes = (WT_STATIC_HUFFMAN_NODE*)calloc(pow(2, huffman->max_depth), sizeof(WT_STATIC_HUFFMAN_NODE));
	fill_static_representation(huffman->nodes, node, 0);

	/* Freeing the dynamic tree representation */
	recursive_free_node(node);

	*retp = huffman;
	return (0);
}

int
__wt_huffman_close(WT_HUFFMAN_OBJ *huffman)
{
	free(huffman->nodes);
	free(huffman);
	return (0);
}

void
print_huffman_code(WT_HUFFMAN_OBJ *huffman, u_int16_t symbol)
{
	WT_STATIC_HUFFMAN_NODE* node;
	char* buffer;
	int n;
	int i, p;

	/* Check if the symbol is in valid range */
	if (symbol < huffman->numSymbols)
	{
		buffer = (char*)calloc(huffman->max_depth, 1);
		n = pow(2, huffman->max_depth);

		node = NULL;
		for (i = 0; i < n; ++i)
		{
			node = &(huffman->nodes[i]);

			if (node->valid && node->symbol == symbol)
				break;
		}

		if (node && node->symbol == symbol)
		{
			/* We've got the leaf node, at index 'i'.
			   Now we fill the output buffer in back order*/

			p = node->codeword_length-1;

			while (p >= 0)
			{
				buffer[p] = (i % 2) == 1 ? '0' : '1';
				i = (i-1) / 2;
				p--;
			}

			printf(buffer);
			putchar('\n');
		}
		else
		{
			printf("Symbol is not in the huffman tree: %x\n", symbol);
		}

		free(buffer);	
     }
     else
     {
	     printf("Symbol out of range: %u >= %u\n", symbol, huffman->numSymbols);
     }
}

int
__wt_huffman_encode(WT_HUFFMAN_OBJ *huffman,
    u_int8_t *from, u_int32_t len_from, u_int8_t *to, u_int32_t len_to, u_int32_t *out_bytes_used)
{
	u_int32_t i, n, j;
	int p;
	u_int16_t symbol;
	WT_STATIC_HUFFMAN_NODE* node;
	u_int32_t bitpos;
	u_int8_t padding_info;

	n = pow(2, huffman->max_depth);
	bitpos = 3;
	memset(to, 0, len_to);

	for (i = 0; i < len_from; i += huffman->numBytes)
	{
     		/* Getting the next symbol, either 1 or 2 bytes */
     		if (huffman->numBytes == 1)
     		{
     			symbol = *from++;
     		}
     		else
     		{
     			symbol = ((u_int16_t)(*from++)) << 8;
     			symbol |= *from++;
     		}
	  
     		/* Getting the symbol's huffman code from the table */
		node = NULL;
		for (j = 0; j < n; ++j)
		{
			node = &(huffman->nodes[j]);

			if (node->valid && node->symbol == symbol)
				break;
		}

		if (node && node->symbol == symbol)
		{
			/* We've got the leaf node, at index 'j'.
			   Now we fill the output buffer in back order*/

			p = node->codeword_length-1;

			while (p >= 0)
			{
				MODIFY_BIT(to, (bitpos+p), ((j%2) ^ 1));

				j = (j-1) / 2;
				p--;
			}

			bitpos += node->codeword_length;
		}
		else
		{
     			printf("Error:  there was an undefined source symbol: %d\n", symbol);
     			return 2; /* There was a symbol in the source buffer that was defined with zero freq */
     		}
	 
     	}

	/* At this point, bitpos is the total number of used bits (including the 3 bit 
	   at the beginning of the buffer, which we'll set now to the number of bits
	   used in the last byte:
	*/
	padding_info = (bitpos % 8) << 5;
	*to |= padding_info;

     	if (out_bytes_used != NULL)
     	{
     		*out_bytes_used = bitpos / 8 + ((bitpos % 8) ? 1 : 0);
     	}

	return (0);
}

int
__wt_huffman_decode(WT_HUFFMAN_OBJ *huffman,
    u_int8_t *from, u_int16_t len_from, u_int8_t *to, u_int32_t len_to, u_int32_t *out_bytes_used)
{
	WT_STATIC_HUFFMAN_NODE* node;
	u_int32_t node_idx;
	u_int32_t bytes, i, len_from_bits;
	u_int8_t bitpos, mask, bit, padding_info;

	bitpos = 4; /* skipping the first 3 bits */
	bytes = 0;
	node_idx = 0;
	padding_info = (*from & 0xE0) >> 5; /* this is the number of used bits in the last byte */
	len_from_bits = ((len_from-1) * 8) + padding_info;

	/* The loop will go through each bit of the source stream,
	its length is given in BITS! */
	for (i = 3; i < len_from_bits; i++)
	{
		/* Extracting the current bit */
		mask = 1 << bitpos;
		bit = (*from & mask);

		/* As we go through the bits, we also make steps in
		the huffman tree, originated from the root, toward
		the leaves. */
		if (bit)
			node_idx = (node_idx * 2) + 2;
		else
			node_idx = (node_idx * 2) + 1;

		node = &(huffman->nodes[node_idx]);

		/* if this is a leaf, we've found a complete symbol */
		if (node->valid && node->codeword_length > 0)
		{
			if ((bytes + huffman->numBytes) > len_to)
			{
				/* Target buffer overrun */
				return 1;
			}

			if (huffman->numBytes == 1)
			{
				*to++ = node->symbol;
			} else {
				*to++ = (node->symbol & 0xFF00) >> 8;
				*to++ = node->symbol & 0xFF;
			}

			bytes += huffman->numBytes;
			node_idx = 0;
		}

		/* moving forward one bit in the source stream */
		if (bitpos > 0)
		{
			bitpos--;
		} else {
			bitpos = 7;
			from++;
		}
	}

	/* return the number of bytes used */
	if (out_bytes_used != NULL)
	{
		*out_bytes_used = bytes;
	}
      
	return (0);
}


#include <stdlib.h>
#include "huffman.h"
#include "node_queue.h"

int 
__node_queue_open(NODE_QUEUE** retp)
{
	/* Allocating the queue data record and returning to the user */
	NODE_QUEUE* queue = (NODE_QUEUE*)calloc(1, sizeof(NODE_QUEUE));
	if (!queue)
		return 1;	

	*retp = queue;

	return 0;
}

int
__node_queue_close(NODE_QUEUE* queue)
{
	NODE_QUEUE_ELEM* next_elem;
	NODE_QUEUE_ELEM* elem;

	/* Freeing each element of the queue's linked list */
	elem = queue->first;

	while (elem)
	{
		next_elem = elem->next;
		free(elem);
		elem = next_elem;
	}

	/* Freeing the queue record itself */
	free(queue);
	return 0;
}

int 
__node_queue_enqueue(NODE_QUEUE* queue, WT_FREQTREE_NODE* node)
{
	NODE_QUEUE_ELEM* elem;

	if (!queue)
		return 2;

	/* Allocating a new linked list element */
	elem = (NODE_QUEUE_ELEM*)malloc(sizeof(NODE_QUEUE_ELEM));
	if (!elem)
		return 1;

	/* It holds the tree node, and has no next element yet */
	elem->node = node;
	elem->next = NULL;

	/* If the queue is empty, the first element will be the new one*/
	if (!queue->first)
		queue->first = elem;

	/* If the queue is not empty, the last element's next
	pointer must be updated */
	if (queue->last)
		queue->last->next = elem;

	/* The last element is the new one */
	queue->last = elem;

	return 0;
}

int
__node_queue_dequeue(NODE_QUEUE* queue, WT_FREQTREE_NODE** retp)
{
	NODE_QUEUE_ELEM* first_elem;

	/* Performing some checks... */
	if (!queue || !retp)
		return 2;

	if (!queue->first)
		return -1;

	/* Getting the first element of the queue 
	and updating it to point to the next element as first. */
	first_elem = queue->first;
	*retp = first_elem->node;
	queue->first = first_elem->next;

	/* If the last element was the dequeued element, we
	have to update it to NULL */
	if (queue->last == first_elem)
		queue->last = NULL;

	/* Freeing the linked list element that has been dequeued */
	free(first_elem);
	return 0;
}

int
__node_queue_is_empty(NODE_QUEUE* queue)
{
	return queue == NULL || !queue->first;
}

int
__node_queue_get_first(NODE_QUEUE* queue, WT_FREQTREE_NODE** retp)
{
	if (!queue || !retp)
		return 2;

	if (!queue->first)
		return -1;

	*retp = queue->first->node;
	return 0;
}
