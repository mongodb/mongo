/*

  Copyright (C) 2015-2023 David Anderson. All Rights Reserved.

  This program is free software; you can redistribute it
  and/or modify it under the terms of version 2.1 of the
  GNU Lesser General Public License as published by the Free
  Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/

#define HASHSEARCH

#ifdef HASHSEARCH
/* Only needed for hash based search in a tsearch style. */
#define INITTREE(x,y) (x) = dwarf_initialize_search_hash(&(x),(y),0)
#else
#define INITTREE(x,y)
#endif /* HASHSEARCH */

/*  Contexts are in a list in a dbg and
    do not move once established.
    So saving one is ok. as long as the dbg
    exists. */
struct Dwarf_Tied_Entry_s {
    Dwarf_Sig8 dt_key;
    Dwarf_CU_Context dt_context;
};

int _dwarf_tied_compare_function(const void *l, const void *r);
void * _dwarf_tied_make_entry(Dwarf_Sig8 *key, Dwarf_CU_Context val);
DW_TSHASHTYPE _dwarf_tied_data_hashfunc(const void *keyp);
