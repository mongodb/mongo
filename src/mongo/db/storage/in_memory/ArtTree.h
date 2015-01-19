//@file ArtTree.h
/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

//
// This is derived code.   The original code is:
//
//	ARTful5: Adaptive Radix Trie key-value store
//	Author: Karl Malbrain, malbrain@cal.berkeley.edu
//	Date:   13 JAN 15
//

typedef unsigned long int ulong;
typedef unsigned char uchar;
typedef unsigned int uint;

enum NodeType {
	UnusedNode = 0,     // node is not yet in use
	LeafSlot,		    // node slot contains leaf offset
	ValueSlot,		    // node slot contains ARTval offset
	SpanNode,		    // node contains up to 8 key bytes and leaf element
	Array4,			    // node contains 4 radix slots & leaf element
	Array16,		    // node contains 16 radix slots & leaf element
	Array48,		    // node contains 48 radix slots & leaf element
	Array256		    // node contains 256 radix slots & leaf element
};

typedef union {
  struct {
	ulong off:45;		// offset to node sub-contents
	uchar type:3;		// type of radix node
	uchar mutex[1];		// update/write synchronization
	uchar nslot:7;		// number of slots of node in use
	uchar dead:1;		// node is no longer in the tree
  };
  ulong bits;
} ARTslot;

//  a node is broken down into two parts:
//  the node proper and its pointer slot.

//
//  the first few fields are generic to all nodes:
//
typedef struct {
    ARTslot value[1];	// slot to a leaf value that ended before this node.
} ARTgeneric;

//
//  radix node with four slots and their key bytes:
//
typedef struct {
	ARTslot value[1];	// slot to a leaf value that ended before this node.
	uchar keys[4];
	ARTslot radix[4];
} ARTnode4;

//
//  radix node with sixteen slots and their key bytes:
//
typedef struct {
	ARTslot value[1];	// slot to a leaf value that ended before this node.
	uchar keys[16];
	ARTslot radix[16];
} ARTnode16;

//
//  radix node with sixty-four slots and a 256 key byte array:
//
typedef struct {
	ARTslot value[1];	// slot to a leaf value that ended before this node.
	uchar keys[256];
	ARTslot radix[48];
} ARTnode48;

//
//  radix node all two hundred fifty six slots
//
typedef struct {
	ARTslot value[1];	// slot to a leaf value that ended before this node.
	ARTslot radix[256];
} ARTnode256;

//
//	Span node containing up to 16 consecutive key bytes
//
typedef struct {
	ARTslot value[1];	// slot to a leaf value that ended before this node.
	uchar bytes[8];
	ARTslot next[1];	// next node under span
} ARTspan;

//
//	the ARTful trie containing the root node slot
//	and the heap storage management.
//
typedef struct {
	ARTslot root[1];
	ulong arena_size;	// size of Arena File
	ulong arena_next;	// next available offset
	uchar arena_mutex[1];
} ARTtrie;

//
//	the ARTful trie value string in the heap
//
typedef struct {
	uchar len;			// this can be changed to a ushort or uint
	uchar value[0];
} ARTval;

typedef union {
	ARTspan *span;
	ARTnode4 *radix4;
	ARTnode16 *radix16;
	ARTnode48 *radix48;
	ARTnode256 *radix256;
	ARTgeneric *generic;
} ARTfan;

//
//	cursor stack element
//
typedef struct {
	ARTslot *slot;		// current slot
	uint off;			// offset within key
	int idx;			// current index within slot
} ARTstack;

//
//	cursor control
//
typedef struct {
	uint maxdepth;		// maximum depth of ARTful trie
	uint depth;			// current depth of cursor
	ARTval *value;		// current leaf node
	ARTstack stack[0];	// cursor stack
} ARTcursor;

//
//	Each thread gets one of these structures
//
typedef struct {
	ulong base;			// base of arena chunk assigned to thread
	ulong offset;		// next offset of this chunk to allocate
	ARTtrie *trie;		// ARTful trie
	ARTcursor *cursor;	// thread cursor
} ARTthread;

