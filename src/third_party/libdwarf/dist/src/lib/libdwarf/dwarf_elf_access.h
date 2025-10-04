#ifndef _DWARF_ELF_PORT_H
#define _DWARF_ELF_PORT_H
/*

Copyright (C) 2008-2023 David Anderson. All rights reserved.
Portions Copyright 2008-2010 Arxan Technologies, Inc. All rights reserved.

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

/*  libelf) object access for the generic
    object file interface */

int
dwarf_elf_object_access_init(void *  elf ,
    int libdwarf_owns_elf,
    Dwarf_Obj_Access_Interface**  ret_obj,
    int *err );

void
dwarf_elf_object_access_finish(Dwarf_Obj_Access_Interface*  obj );

/* End ELF object access for the generic object file interface */

#endif
