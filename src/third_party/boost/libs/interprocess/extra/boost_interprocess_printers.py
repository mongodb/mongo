# Copyright 2024 Braden Ganetsky
# Distributed under the Boost Software License, Version 1.0.
# https://www.boost.org/LICENSE_1_0.txt

import gdb.printing

class BoostInterprocessOffsetPtrPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return f"{BoostInterprocessOffsetPtrPrinter.get(self.val)}"

    # This is a simplified and inlined version of `offset_ptr::get()`
    def get(offset_ptr):
        offset = offset_ptr["internal"]["m_offset"]
        pointer = offset_ptr.type.template_argument(0).pointer()
        if offset == 1:
            return gdb.Value(0).cast(pointer) # nullptr
        else:
            unsigned_char_pointer = gdb.lookup_type("unsigned char").pointer()
            this = offset_ptr.address
            return (this.cast(unsigned_char_pointer) + offset).cast(pointer)

    def boost_to_address(offset_ptr):
        return BoostInterprocessOffsetPtrPrinter.get(offset_ptr)
        
    # This is a simplified and inlined version of `offset_ptr::operator+=()`
    def boost_next(raw_ptr, offset):
        unsigned_char_pointer = gdb.lookup_type("unsigned char").pointer()
        pointer = raw_ptr.type
        aa = raw_ptr.cast(unsigned_char_pointer)
        bb = offset * pointer.target().sizeof
        return (aa + bb).cast(pointer)

def boost_interprocess_build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("boost_interprocess")

    pp.add_printer("boost::interprocess::offset_ptr", "^boost::interprocess::offset_ptr<.*>$", BoostInterprocessOffsetPtrPrinter)

    return pp

gdb.printing.register_pretty_printer(gdb.current_objfile(), boost_interprocess_build_pretty_printer())
