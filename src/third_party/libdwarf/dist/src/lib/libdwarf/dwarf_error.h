/*

Copyright (C) 2000 Silicon Graphics, Inc.  All Rights Reserved.
Portions Copyright (C) 2011-2023 David Anderson. All Rights Reserved.

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

#ifndef DWARF_ERROR_H
#define DWARF_ERROR_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void _dwarf_error(Dwarf_Debug dbg, Dwarf_Error * error,
    Dwarf_Signed errval);
void _dwarf_error_string(Dwarf_Debug dbg, Dwarf_Error * error,
    Dwarf_Signed errval, char *msg);

#define DWARF_DBG_ERROR(dbg,errval,retval) \
    _dwarf_error((dbg), error, (errval)); return(retval);

#define DE_STANDARD 0 /* Normal alloc attached to dbg. */
#define DE_STATIC 1   /* Using global static var */
#define DE_MALLOC 2   /* Using malloc space */
struct Dwarf_Error_s {
    Dwarf_Signed er_errval;
    void       * er_msg;

    /*  If non-zero the Dwarf_Error_s struct is not malloc'd.
        To aid when malloc returns NULL.
        If zero a normal dwarf_dealloc will work.
        er_static_alloc only accessed by dwarf_alloc.c.

        If er_static_alloc is 1 in a Dwarf_Error_s
        struct (set by libdwarf) and client code accidentally
        turns that 0 to zero through a wild
        pointer reference (the field is hidden
        from clients...) then chaos will
        eventually follow.
    */
    int er_static_alloc;
};
extern struct Dwarf_Error_s _dwarf_failsafe_error;

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARF_ERROR_H */
