#ifndef DWARF_TSEARCH_H
#define DWARF_TSEARCH_H
/* Copyright (c) 2013-2023, David Anderson
All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*  The following interfaces follow tsearch (See the Single
    Unix Specification) but the implementation is
    written without reference to the source  code
    of any version of tsearch. Only uses
    of tsearch were examined, not tsearch source code.

    See https://www.prevanders.net/tsearch.html
    and https://www.prevanders.net/dwarf.html#tsearch
    for information about tsearch.

    We are matching the standard functional
    interface here, but to avoid interfering with
    libc implementations or code using libc
    implementations, we change all the names.

*/

/*  configure/cmake ensure uintptr_t defined, but if not,
    possibly  "-Duintptr_t=unsigned long" might help  */
#ifndef DW_TSHASHTYPE
#define DW_TSHASHTYPE uintptr_t
#endif

/*  The DW_VISIT values passed back to you through
    the callback function in dwarf_twalk();
*/
typedef enum
{
    dwarf_preorder,
    dwarf_postorder,
    dwarf_endorder,
    dwarf_leaf
}
DW_VISIT;

/* void * return values are actually
   void **key so you must dereference these
   once to get a key you passed in.
*/

/*  We rename these so there is no conflict with another version
    of the tsearch sources, such as is used in dwarfdump. */
#define dwarf_tsearch  _dwarf_tsearch
#define dwarf_tfind    _dwarf_tfind
#define dwarf_tdelete  _dwarf_tdelete
#define dwarf_twalk    _dwarf_twalk
#define dwarf_tdestroy _dwarf_tdestroy
#define dwarf_tdump    _dwarf_tdump
#define dwarf_initialize_search_hash _dwarf_initialize_search_hash

void *dwarf_tsearch(const void * /*key*/, void ** /*rootp*/,
    int (* /*compar*/)(const void *, const void *));

void *dwarf_tfind(const void * /*key*/, void *const * /*rootp*/,
    int (* /*compar*/)(const void *, const void *));

/*
    dwarf_tdelete() returns NULL if it cannot delete anything
        or if the tree is now empty (if empty, *rootp
        is set NULL by dwarf_tdelete()).
    If the delete succeeds and the tree is non-empty returns
        a pointer to the parent node of the deleted item,
        unless the deleted item was at the root, in which
        case the returned pointer relates to the new root.
*/
void *dwarf_tdelete(const void * /*key*/, void ** /*rootp*/,
    int (* /*compar*/)(const void *, const void *));

void dwarf_twalk(const void * /*root*/,
    void (* /*action*/)(const void * /*nodep*/,
    const DW_VISIT  /*which*/,
    const int  /*depth*/));

/*  dwarf_tdestroy() cannot set the root pointer NULL, you must do
    so on return from dwarf_tdestroy(). */
void dwarf_tdestroy(void * /*root*/,
    void (* /*free_node*/)(void * /*nodep*/));

/*  Prints  a simple tree representation to stdout. For debugging.
*/
void dwarf_tdump(const void*root,
    char *(* /*keyprint*/)(const void *),
    const char *msg);

/*  Returns NULL  and does nothing
    unless the implemenation used uses a hash tree. */
void * dwarf_initialize_search_hash( void **treeptr,
    DW_TSHASHTYPE (*hashfunc)(const void *key),
    unsigned long size_estimate);
#endif /* DWARF_TSEARCH_H */
