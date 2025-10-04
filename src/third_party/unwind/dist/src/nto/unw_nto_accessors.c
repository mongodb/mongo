/*
 * Copyright 2020, 2022-2023 QNX Blackberry Limited.
 *
 * This file is part of libunwind.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "libunwind-nto.h"


/**
 * Instance of the NTO accessor struct.
 */
unw_accessors_t unw_nto_accessors =
{
  .find_proc_info          = unw_nto_find_proc_info,
  .put_unwind_info         = unw_nto_put_unwind_info,
  .get_dyn_info_list_addr  = unw_nto_get_dyn_info_list_addr,
  .access_mem              = unw_nto_access_mem,
  .access_reg              = unw_nto_access_reg,
  .access_fpreg            = unw_nto_access_fpreg,
  .resume                  = unw_nto_resume,
  .get_proc_name           = unw_nto_get_proc_name,
  .get_proc_ip_range       = unw_nto_get_proc_ip_range,
  .get_elf_filename        = unw_nto_get_elf_filename,
};

