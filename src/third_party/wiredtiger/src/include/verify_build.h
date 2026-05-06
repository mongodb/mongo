/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <assert.h>

/*
 * NOTE: If you see a compile failure in this file, your compiler is laying out structs in memory in
 * a way WiredTiger does not expect. Please refer to the build instructions in the documentation
 * (docs/html/install.html) for more information.
 */

/*
 * WiredTiger uses opaque pointers to expose only necessary data to end users. This public
 * information is stored in the iface field at the beginning of the struct, and private fields can
 * be accessed by casting the pointer to our internal type. Since this is just type casting and
 * pointer arithmetic the public fields must be the first fields in the struct.
 */
#define WT_VERIFY_OPAQUE_POINTER(type) \
    static_assert(offsetof(type, iface) == 0, #type " does not begin with the 'iface' field")

/* Check specific structures weren't padded. */
static_assert(sizeof(WT_BLOCK_DESC) == WT_BLOCK_DESC_SIZE,
  "size of WT_BLOCK_DESC did not match expected size WT_BLOCK_DESC_SIZE");
static_assert(
  sizeof(WT_REF) == WT_REF_SIZE, "size of WT_REF did not match expected size WT_REF_SIZE");

/*
 * WT_BLOCK_DISAGG shares its leading prefix with WT_BLOCK because instances of both are inserted
 * into conn->blockhash / conn->blockqh and traversed there through a WT_BLOCK * pointer. The shared
 * prefix ends at hashq; private disaggregated fields follow. Mismatched offsets in this prefix lead
 * to silent struct-aliasing bugs. The asserts below pin the offsets of every shared field so any
 * reordering or insertion in either struct breaks the build.
 */
static_assert(offsetof(WT_BLOCK_DISAGG, name) == offsetof(WT_BLOCK, name),
  "WT_BLOCK_DISAGG::name must alias WT_BLOCK::name");
static_assert(offsetof(WT_BLOCK_DISAGG, objectid) == offsetof(WT_BLOCK, objectid),
  "WT_BLOCK_DISAGG::objectid must alias WT_BLOCK::objectid");
static_assert(offsetof(WT_BLOCK_DISAGG, ref) == offsetof(WT_BLOCK, ref),
  "WT_BLOCK_DISAGG::ref must alias WT_BLOCK::ref");
static_assert(offsetof(WT_BLOCK_DISAGG, q) == offsetof(WT_BLOCK, q),
  "WT_BLOCK_DISAGG::q must alias WT_BLOCK::q");
static_assert(offsetof(WT_BLOCK_DISAGG, hashq) == offsetof(WT_BLOCK, hashq),
  "WT_BLOCK_DISAGG::hashq must alias WT_BLOCK::hashq");

/*
 * WT_UPDATE is special: we arrange fields to avoid padding within the structure but it could be
 * padded at the end depending on the timestamp size. Further check that the data field in the
 * update structure is where we expect it.
 */
static_assert(
  sizeof(WT_UPDATE) == WT_ALIGN(WT_UPDATE_SIZE, 8), "size of WT_UPDATE is not a multiple of 8");
static_assert(offsetof(WT_UPDATE, data) == WT_UPDATE_SIZE,
  "variable length array 'data' is not the last field in WT_UPDATE");

/*
 * WT_UPDATE: Validate expected sum of field sizes compared to compiler determined structure size.
 * If the fields WT_UPDATE these assertions should be revised to match the trailing padding of the
 * updated structure.
 */
static_assert(WT_UPDATE_SIZE_NOVALUE == sizeof(WT_UPDATE), "WT_UPDATE size mismatch");
static_assert((WT_UPDATE_SIZE_NOVALUE - WT_UPDATE_SIZE) == 0, "WT_UPDATE size mismatch");

/* Check specific structures were padded. */
#define WT_PADDING_CHECK(s)                                                                        \
    static_assert(sizeof(s) > WT_CACHE_LINE_ALIGNMENT || sizeof(s) % WT_CACHE_LINE_ALIGNMENT == 0, \
      #s " padding check failed")
WT_PADDING_CHECK(WT_TXN_SHARED);

/*
 * The btree code encodes key/value pairs in size_t's, and requires at least 8B size_t's.
 */
static_assert(sizeof(size_t) >= 8, "size_t is smaller than 8 bytes");

/*
 * We require a wt_off_t fit into an 8B chunk because 8B is the largest integral value we can encode
 * into an address cookie.
 *
 * WiredTiger has never been tested on a system with 4B file offsets, disallow them for now.
 */
static_assert(
  sizeof(wt_off_t) == 8, "WiredTiger is only supported on systems with an 8 byte file offset");

/*
 * We require a time_t be an integral type and fit into a uint64_t for simplicity.
 */
static_assert(sizeof(time_t) <= sizeof(uint64_t), "time_t must fit within a uint64_t");
