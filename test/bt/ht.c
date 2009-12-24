#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wiredtiger.h"

int
main()
{
	return (test1() == 0 && test2() == 0 ? 0 : 1);
}

/******************************************************************
 * Test #1:
 *
 *  Looking at this page:
 *  http://en.wikipedia.org/wiki/Huffman_coding
 *
 *  We can see an example tree on the right showing the correct breakdown
 *  of the text 'this is an exmaple of a huffman tree'
 *
 *  The example program re-produces that demo using the embeddable huffman
 *  lib code from Brian Pollack
 *
 ******************************************************************/
char* text = "this is an example of a huffman tree";

void
print_binary(u_int8_t byte)
{
     putchar((byte >> 7) & 1 ? '1' : '0');
     putchar((byte >> 6) & 1 ? '1' : '0');
     putchar((byte >> 5) & 1 ? '1' : '0');
     putchar((byte >> 4) & 1 ? '1' : '0');
     putchar((byte >> 3) & 1 ? '1' : '0');
     putchar((byte >> 2) & 1 ? '1' : '0');
     putchar((byte >> 1) & 1 ? '1' : '0');
     putchar(byte & 1 ? '1' : '0');
}

int
test1()
{
	void *huffman;
	u_int8_t* freqs;
	u_int8_t* encoded;
	u_int8_t* decoded;
	u_int8_t* source;
	u_int32_t sl, bytecount, bytecount2;
	int i;

	/* In your code you must allocate a buffer large enough for the frequency table 
	* Which is 256 bytes for simple ASCII */
	freqs = (u_int8_t*)calloc(256, 1);
	freqs[' '] = 7;
	freqs['a'] = 4; 
	freqs['e'] = 4; 
	freqs['f'] = 4; 
	freqs['h'] = 4;
	freqs['i'] = 2; 
	freqs['m'] = 2; 
	freqs['n'] = 2; 
	freqs['s'] = 2; 
	freqs['t'] = 2; 
	freqs['l'] = 1; 
	freqs['o'] = 1; 
	freqs['p'] = 1; 
	freqs['r'] = 1; 
	freqs['u'] = 1; 
	freqs['x'] = 1; 

	/* Open the huffman library */
	__wt_huffman_open(NULL, freqs, 256, &huffman);

	/* Prove a point by outputing the tree values 
	that can be directly checked against the table on the Wikipedia page */
	for (i = 0; i < 256; ++i)
	{
		if (freqs[i])
		{
			printf("%c (%02x) ", (char)i, (unsigned char)i);
			(void)__wt_print_huffman_code(NULL, huffman, i);
		}
	}

	/*  The largest possible output is input size + 1 so allocate 
	buffers large enough now.  These are write-only values (const) so
	we don't know actual usage information from them. */
	sl      = strlen(text);
	encoded = (u_int8_t*)malloc(sl);
	source  = (u_int8_t*)malloc(sl);
	decoded = (u_int8_t*)malloc(sl+1);
	memcpy(source, text, sl);

	/* the output buffer (encoded) is the length of the string (sl) and 
	the total bytes used is bytecount.*/
	if (__wt_huffman_encode(huffman, source, sl, encoded, sl, &bytecount))
	{
		printf("Failed to encode\n");
		exit(1);
	}

	printf("Encoded; original bit length: %u; encoded bit length (with padding): %u\n", sl*8, bytecount*8);
	
	/*  Next we demo the decoder.   This takes the input buffer (encoded) which may be 
	any size and decodes the necessary number of bits.  The result is stored in the decoded
	buffer which we made the size of the initial string.   In your database code you
	need to make this sufficiently large enough for the largest possible value you support.*/
	
	__wt_huffman_decode(huffman, encoded, bytecount, decoded, sl, &bytecount2);
	
	/* Not really required but we know this is string data so terminate it */
	decoded[bytecount2] = 0;

	printf("Original:'%s'\n", text);
	printf("Encoded: ");

	for (i = 0; i < bytecount; ++i)
	{
		print_binary(encoded[i]);
	}
	putchar('\n');
	printf("Decoded: '%s' (bytes used: %u; original: %u)\n", decoded, bytecount2, sl);
     
	/* Don't forget to cleanup the library */
	__wt_huffman_close(NULL, huffman);

	free(freqs);
	return 0;
}

/******************************************************************
 * Test #2:
 *
 *  This is a simple example that encodes a series of phone numbers
 *
 ******************************************************************/
int
test2()
{
	void* huffman;
	u_int8_t* freqTablePhones;
	u_int8_t* encoded;
	u_int8_t* decoded;
	u_int8_t* source;
	u_int32_t sl;
	u_int32_t totalRaw = 0;
	u_int32_t totalEncoded = 0;
	u_int32_t bytecount = 0;
	int i, j;
	
	srand(time(0));

	/* Numbers and symbols for a phone number with guess values for frequency.  Overall
	the entrophy of phone number digits is not likely to be the same for each digit */
	freqTablePhones = (u_int8_t*)calloc(256, 1);
	freqTablePhones['0'] = 1;
	freqTablePhones['1'] = 2; 
	freqTablePhones['2'] = 2; 
	freqTablePhones['3'] = 2; 
	freqTablePhones['4'] = 2;
	freqTablePhones['5'] = 2; 
	freqTablePhones['6'] = 2; 
	freqTablePhones['7'] = 2; 
	freqTablePhones['8'] = 2; 
	freqTablePhones['9'] = 2; 
	freqTablePhones['('] = 1; 
	freqTablePhones[')'] = 1; 
	freqTablePhones[' '] = 1; 
	freqTablePhones['-'] = 3; 
	freqTablePhones['+'] = 1; 
 

	/* Open the huffman library */
	__wt_huffman_open(NULL, freqTablePhones, 256, &huffman);

	/* Preallocate buffers for phone numbers */
	sl      = 15; 
	encoded = (u_int8_t*)malloc(sl);
	source  = (u_int8_t*)malloc(sl);
  	decoded = (u_int8_t*)malloc(sl+1);
	
	for (i=0; i<100; i++)
	{
		/* Generate a random phone number */
		for (j=0; j<14; j++)
		{
			source[j] = (rand() % 10) + '0';
		}
	
		source[0] = '(';
		source[4] = ')';
		source[5] = ' ';
		source[9] = '-';
		source[14] = 0;
		
		/* Encode the phone number (notice we pass in only the 
		bytes used not the null terminator on the string*/
		if (__wt_huffman_encode(huffman, source, 14, encoded, sl, &bytecount))
		{
			printf("Failed to encode\n");
			exit(1);
		}
		
		printf("Source: %s, Encoded: %d\n", source, bytecount*8);		
		
		/* Write to the database file 2 bytes bytecount
		bytes from encoded */
		totalRaw += 15;
		totalEncoded += bytecount;				
	}
	    
	printf("Raw bytes: %d, Total used: %d (Saved=%0.02f%%)\n",
		   totalRaw, totalEncoded/8, 100.0 * ((float)totalEncoded / (float)(totalRaw * 8.0)));
	
	/* Don't forget to cleanup the library */
	__wt_huffman_close(NULL, huffman);

	free(freqTablePhones);
	return 0;
}
