// Copyright 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

// Generated on 2024-08-16T22:13:13

#ifndef BOOST_INTERPROCESS_INTERPROCESS_PRINTERS_HPP
#define BOOST_INTERPROCESS_INTERPROCESS_PRINTERS_HPP

#ifndef BOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS
#if defined(__ELF__)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
#endif
__asm__(".pushsection \".debug_gdb_scripts\", \"MS\",@progbits,1\n"
        ".ascii \"\\4gdb.inlined-script.BOOST_INTERPROCESS_INTERPROCESS_PRINTERS_HPP\\n\"\n"
        ".ascii \"import gdb.printing\\n\"\n"

        ".ascii \"class BoostInterprocessOffsetPtrPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = val\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        return f\\\"{BoostInterprocessOffsetPtrPrinter.get(self.val)}\\\"\\n\"\n"

        ".ascii \"    # This is a simplified and inlined version of `offset_ptr::get()`\\n\"\n"
        ".ascii \"    def get(offset_ptr):\\n\"\n"
        ".ascii \"        offset = offset_ptr[\\\"internal\\\"][\\\"m_offset\\\"]\\n\"\n"
        ".ascii \"        pointer = offset_ptr.type.template_argument(0).pointer()\\n\"\n"
        ".ascii \"        if offset == 1:\\n\"\n"
        ".ascii \"            return gdb.Value(0).cast(pointer) # nullptr\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            unsigned_char_pointer = gdb.lookup_type(\\\"unsigned char\\\").pointer()\\n\"\n"
        ".ascii \"            this = offset_ptr.address\\n\"\n"
        ".ascii \"            return (this.cast(unsigned_char_pointer) + offset).cast(pointer)\\n\"\n"

        ".ascii \"    def boost_to_address(offset_ptr):\\n\"\n"
        ".ascii \"        return BoostInterprocessOffsetPtrPrinter.get(offset_ptr)\\n\"\n"

        ".ascii \"    # This is a simplified and inlined version of `offset_ptr::operator+=()`\\n\"\n"
        ".ascii \"    def boost_next(raw_ptr, offset):\\n\"\n"
        ".ascii \"        unsigned_char_pointer = gdb.lookup_type(\\\"unsigned char\\\").pointer()\\n\"\n"
        ".ascii \"        pointer = raw_ptr.type\\n\"\n"
        ".ascii \"        aa = raw_ptr.cast(unsigned_char_pointer)\\n\"\n"
        ".ascii \"        bb = offset * pointer.target().sizeof\\n\"\n"
        ".ascii \"        return (aa + bb).cast(pointer)\\n\"\n"

        ".ascii \"def boost_interprocess_build_pretty_printer():\\n\"\n"
        ".ascii \"    pp = gdb.printing.RegexpCollectionPrettyPrinter(\\\"boost_interprocess\\\")\\n\"\n"

        ".ascii \"    pp.add_printer(\\\"boost::interprocess::offset_ptr\\\", \\\"^boost::interprocess::offset_ptr<.*>$\\\", BoostInterprocessOffsetPtrPrinter)\\n\"\n"

        ".ascii \"    return pp\\n\"\n"

        ".ascii \"gdb.printing.register_pretty_printer(gdb.current_objfile(), boost_interprocess_build_pretty_printer())\\n\"\n"

        ".byte 0\n"
        ".popsection\n");
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif // defined(__ELF__)
#endif // !defined(BOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS)

#endif // !defined(BOOST_INTERPROCESS_INTERPROCESS_PRINTERS_HPP)
