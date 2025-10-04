// Copyright 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

// Generated on 2024-08-25T17:48:54

#ifndef BOOST_UNORDERED_UNORDERED_PRINTERS_HPP
#define BOOST_UNORDERED_UNORDERED_PRINTERS_HPP

#ifndef BOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS
#if defined(__ELF__)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
#endif
__asm__(".pushsection \".debug_gdb_scripts\", \"MS\",%progbits,1\n"
        ".ascii \"\\4gdb.inlined-script.BOOST_UNORDERED_UNORDERED_PRINTERS_HPP\\n\"\n"
        ".ascii \"import gdb.printing\\n\"\n"
        ".ascii \"import gdb.xmethod\\n\"\n"
        ".ascii \"import re\\n\"\n"
        ".ascii \"import math\\n\"\n"

        ".ascii \"class BoostUnorderedHelpers:\\n\"\n"
        ".ascii \"    def maybe_unwrap_atomic(n):\\n\"\n"
        ".ascii \"        if f\\\"{n.type.strip_typedefs()}\\\".startswith(\\\"std::atomic<\\\"):\\n\"\n"
        ".ascii \"            underlying_type = n.type.template_argument(0)\\n\"\n"
        ".ascii \"            return n.cast(underlying_type)\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return n\\n\"\n"

        ".ascii \"    def maybe_unwrap_foa_element(e):\\n\"\n"
        ".ascii \"        if f\\\"{e.type.strip_typedefs()}\\\".startswith(\\\"boost::unordered::detail::foa::element_type<\\\"):\\n\"\n"
        ".ascii \"            return e[\\\"p\\\"]\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return e\\n\"\n"

        ".ascii \"    def maybe_unwrap_reference(value):\\n\"\n"
        ".ascii \"        if value.type.code == gdb.TYPE_CODE_REF:\\n\"\n"
        ".ascii \"            return value.referenced_value()\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return value\\n\"\n"

        ".ascii \"    def countr_zero(n):\\n\"\n"
        ".ascii \"        for i in range(32):\\n\"\n"
        ".ascii \"            if (n & (1 << i)) != 0:\\n\"\n"
        ".ascii \"                return i\\n\"\n"
        ".ascii \"        return 32\\n\"\n"

        ".ascii \"class BoostUnorderedPointerCustomizationPoint:\\n\"\n"
        ".ascii \"    def __init__(self, any_ptr):\\n\"\n"
        ".ascii \"        vis = gdb.default_visualizer(any_ptr)\\n\"\n"
        ".ascii \"        if vis is None:\\n\"\n"
        ".ascii \"            self.to_address = lambda ptr: ptr\\n\"\n"
        ".ascii \"            self.next = lambda ptr, offset: ptr + offset\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            self.to_address = lambda ptr: ptr if (ptr.type.code == gdb.TYPE_CODE_PTR) else type(vis).boost_to_address(ptr)\\n\"\n"
        ".ascii \"            self.next = lambda ptr, offset: type(vis).boost_next(ptr, offset)\\n\"\n"

        ".ascii \"class BoostUnorderedFcaPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = BoostUnorderedHelpers.maybe_unwrap_reference(val)\\n\"\n"
        ".ascii \"        self.name = f\\\"{self.val.type.strip_typedefs()}\\\".split(\\\"<\\\")[0]\\n\"\n"
        ".ascii \"        self.name = self.name.replace(\\\"boost::unordered::\\\", \\\"boost::\\\")\\n\"\n"
        ".ascii \"        self.is_map = self.name.endswith(\\\"map\\\")\\n\"\n"
        ".ascii \"        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val[\\\"table_\\\"][\\\"buckets_\\\"][\\\"buckets\\\"])\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        size = self.val[\\\"table_\\\"][\\\"size_\\\"]\\n\"\n"
        ".ascii \"        return f\\\"{self.name} with {size} elements\\\"\\n\"\n"

        ".ascii \"    def display_hint(self):\\n\"\n"
        ".ascii \"        return \\\"map\\\"\\n\"\n"

        ".ascii \"    def children(self):\\n\"\n"
        ".ascii \"        def generator():\\n\"\n"
        ".ascii \"            grouped_buckets = self.val[\\\"table_\\\"][\\\"buckets_\\\"]\\n\"\n"

        ".ascii \"            size = grouped_buckets[\\\"size_\\\"]\\n\"\n"
        ".ascii \"            buckets = grouped_buckets[\\\"buckets\\\"]\\n\"\n"
        ".ascii \"            bucket_index = 0\\n\"\n"

        ".ascii \"            count = 0\\n\"\n"
        ".ascii \"            while bucket_index != size:\\n\"\n"
        ".ascii \"                current_bucket = self.cpo.next(self.cpo.to_address(buckets), bucket_index)\\n\"\n"
        ".ascii \"                node = self.cpo.to_address(current_bucket.dereference()[\\\"next\\\"])\\n\"\n"
        ".ascii \"                while node != 0:\\n\"\n"
        ".ascii \"                    value = node.dereference()[\\\"buf\\\"][\\\"t_\\\"]\\n\"\n"
        ".ascii \"                    if self.is_map:\\n\"\n"
        ".ascii \"                        first = value[\\\"first\\\"]\\n\"\n"
        ".ascii \"                        second = value[\\\"second\\\"]\\n\"\n"
        ".ascii \"                        yield \\\"\\\", first\\n\"\n"
        ".ascii \"                        yield \\\"\\\", second\\n\"\n"
        ".ascii \"                    else:\\n\"\n"
        ".ascii \"                        yield \\\"\\\", count\\n\"\n"
        ".ascii \"                        yield \\\"\\\", value\\n\"\n"
        ".ascii \"                    count += 1\\n\"\n"
        ".ascii \"                    node = self.cpo.to_address(node.dereference()[\\\"next\\\"])\\n\"\n"
        ".ascii \"                bucket_index += 1\\n\"\n"

        ".ascii \"        return generator()\\n\"\n"

        ".ascii \"class BoostUnorderedFcaIteratorPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = val\\n\"\n"
        ".ascii \"        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val[\\\"p\\\"])\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        if self.valid():\\n\"\n"
        ".ascii \"            value = self.cpo.to_address(self.val[\\\"p\\\"]).dereference()[\\\"buf\\\"][\\\"t_\\\"]\\n\"\n"
        ".ascii \"            return f\\\"iterator = {{ {value} }}\\\"\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return \\\"iterator = { end iterator }\\\"\\n\"\n"

        ".ascii \"    def valid(self):\\n\"\n"
        ".ascii \"        return (self.cpo.to_address(self.val[\\\"p\\\"]) != 0) and (self.cpo.to_address(self.val[\\\"itb\\\"][\\\"p\\\"]) != 0)\\n\"\n"

        ".ascii \"class BoostUnorderedFoaTableCoreCumulativeStatsPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = val\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        return \\\"[stats]\\\"\\n\"\n"

        ".ascii \"    def display_hint(self):\\n\"\n"
        ".ascii \"        return \\\"map\\\"\\n\"\n"

        ".ascii \"    def children(self):\\n\"\n"
        ".ascii \"        def generator():\\n\"\n"
        ".ascii \"            members = [\\\"insertion\\\", \\\"successful_lookup\\\", \\\"unsuccessful_lookup\\\"]\\n\"\n"
        ".ascii \"            for member in members:\\n\"\n"
        ".ascii \"                yield \\\"\\\", member\\n\"\n"
        ".ascii \"                yield \\\"\\\", self.val[member]\\n\"\n"
        ".ascii \"        return generator()\\n\"\n"

        ".ascii \"class BoostUnorderedFoaCumulativeStatsPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = val\\n\"\n"
        ".ascii \"        self.n = self.val[\\\"n\\\"]\\n\"\n"
        ".ascii \"        self.N = self.val.type.template_argument(0)\\n\"\n"

        ".ascii \"    def display_hint(self):\\n\"\n"
        ".ascii \"        return \\\"map\\\"\\n\"\n"

        ".ascii \"    def children(self):\\n\"\n"
        ".ascii \"        def generator():\\n\"\n"
        ".ascii \"            yield \\\"\\\", \\\"count\\\"\\n\"\n"
        ".ascii \"            yield \\\"\\\", self.n\\n\"\n"

        ".ascii \"            sequence_stats_data = gdb.lookup_type(\\\"boost::unordered::detail::foa::sequence_stats_data\\\")\\n\"\n"
        ".ascii \"            data = self.val[\\\"data\\\"]\\n\"\n"
        ".ascii \"            arr = data.address.reinterpret_cast(sequence_stats_data.pointer())\\n\"\n"
        ".ascii \"            def build_string(idx):\\n\"\n"
        ".ascii \"                entry = arr[idx]\\n\"\n"
        ".ascii \"                avg = float(entry[\\\"m\\\"])\\n\"\n"
        ".ascii \"                var = float(entry[\\\"s\\\"] / self.n) if (self.n != 0) else 0.0\\n\"\n"
        ".ascii \"                dev = math.sqrt(var)\\n\"\n"
        ".ascii \"                return f\\\"{{avg = {avg}, var = {var}, dev = {dev}}}\\\"\\n\"\n"

        ".ascii \"            if self.N > 0:\\n\"\n"
        ".ascii \"                yield \\\"\\\", \\\"probe_length\\\"\\n\"\n"
        ".ascii \"                yield \\\"\\\", build_string(0)\\n\"\n"
        ".ascii \"            if self.N > 1:\\n\"\n"
        ".ascii \"                yield \\\"\\\", \\\"num_comparisons\\\"\\n\"\n"
        ".ascii \"                yield \\\"\\\", build_string(1)\\n\"\n"

        ".ascii \"        return generator()\\n\"\n"

        ".ascii \"class BoostUnorderedFoaPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = BoostUnorderedHelpers.maybe_unwrap_reference(val)\\n\"\n"
        ".ascii \"        self.name = f\\\"{self.val.type.strip_typedefs()}\\\".split(\\\"<\\\")[0]\\n\"\n"
        ".ascii \"        self.name = self.name.replace(\\\"boost::unordered::\\\", \\\"boost::\\\")\\n\"\n"
        ".ascii \"        self.is_map = self.name.endswith(\\\"map\\\")\\n\"\n"
        ".ascii \"        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val[\\\"table_\\\"][\\\"arrays\\\"][\\\"groups_\\\"])\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        size = BoostUnorderedHelpers.maybe_unwrap_atomic(self.val[\\\"table_\\\"][\\\"size_ctrl\\\"][\\\"size\\\"])\\n\"\n"
        ".ascii \"        return f\\\"{self.name} with {size} elements\\\"\\n\"\n"

        ".ascii \"    def display_hint(self):\\n\"\n"
        ".ascii \"        return \\\"map\\\"\\n\"\n"

        ".ascii \"    def is_regular_layout(self, group):\\n\"\n"
        ".ascii \"        typename = group[\\\"m\\\"].type.strip_typedefs()\\n\"\n"
        ".ascii \"        array_size = typename.sizeof // typename.target().sizeof\\n\"\n"
        ".ascii \"        if array_size == 16:\\n\"\n"
        ".ascii \"            return True\\n\"\n"
        ".ascii \"        elif array_size == 2:\\n\"\n"
        ".ascii \"            return False\\n\"\n"

        ".ascii \"    def match_occupied(self, group):\\n\"\n"
        ".ascii \"        m = group[\\\"m\\\"]\\n\"\n"
        ".ascii \"        at = lambda b: BoostUnorderedHelpers.maybe_unwrap_atomic(m[b][\\\"n\\\"])\\n\"\n"

        ".ascii \"        if self.is_regular_layout(group):\\n\"\n"
        ".ascii \"            bits = [1 << b for b in range(16) if at(b) == 0]\\n\"\n"
        ".ascii \"            return 0x7FFF & ~sum(bits)\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            xx = at(0) | at(1)\\n\"\n"
        ".ascii \"            yy = xx | (xx >> 32)\\n\"\n"
        ".ascii \"            return 0x7FFF & (yy | (yy >> 16))\\n\"\n"

        ".ascii \"    def is_sentinel(self, group, pos):\\n\"\n"
        ".ascii \"        m = group[\\\"m\\\"]\\n\"\n"
        ".ascii \"        at = lambda b: BoostUnorderedHelpers.maybe_unwrap_atomic(m[b][\\\"n\\\"])\\n\"\n"

        ".ascii \"        N = group[\\\"N\\\"]\\n\"\n"
        ".ascii \"        sentinel_ = group[\\\"sentinel_\\\"]\\n\"\n"
        ".ascii \"        if self.is_regular_layout(group):\\n\"\n"
        ".ascii \"            return pos == N-1 and at(N-1) == sentinel_\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return pos == N-1 and (at(0) & 0x4000400040004000) == 0x4000 and (at(1) & 0x4000400040004000) == 0\\n\"\n"

        ".ascii \"    def children(self):\\n\"\n"
        ".ascii \"        def generator():\\n\"\n"
        ".ascii \"            table = self.val[\\\"table_\\\"]\\n\"\n"
        ".ascii \"            groups = self.cpo.to_address(table[\\\"arrays\\\"][\\\"groups_\\\"])\\n\"\n"
        ".ascii \"            elements = self.cpo.to_address(table[\\\"arrays\\\"][\\\"elements_\\\"])\\n\"\n"

        ".ascii \"            pc_ = groups.cast(gdb.lookup_type(\\\"unsigned char\\\").pointer())\\n\"\n"
        ".ascii \"            p_ = elements\\n\"\n"
        ".ascii \"            first_time = True\\n\"\n"
        ".ascii \"            mask = 0\\n\"\n"
        ".ascii \"            n0 = 0\\n\"\n"
        ".ascii \"            n = 0\\n\"\n"

        ".ascii \"            count = 0\\n\"\n"
        ".ascii \"            while p_ != 0:\\n\"\n"
        ".ascii \"                # This if block mirrors the condition in the begin() call\\n\"\n"
        ".ascii \"                if (not first_time) or (self.match_occupied(groups.dereference()) & 1):\\n\"\n"
        ".ascii \"                    pointer = BoostUnorderedHelpers.maybe_unwrap_foa_element(p_)\\n\"\n"
        ".ascii \"                    value = self.cpo.to_address(pointer).dereference()\\n\"\n"
        ".ascii \"                    if self.is_map:\\n\"\n"
        ".ascii \"                        first = value[\\\"first\\\"]\\n\"\n"
        ".ascii \"                        second = value[\\\"second\\\"]\\n\"\n"
        ".ascii \"                        yield \\\"\\\", first\\n\"\n"
        ".ascii \"                        yield \\\"\\\", second\\n\"\n"
        ".ascii \"                    else:\\n\"\n"
        ".ascii \"                        yield \\\"\\\", count\\n\"\n"
        ".ascii \"                        yield \\\"\\\", value\\n\"\n"
        ".ascii \"                    count += 1\\n\"\n"
        ".ascii \"                first_time = False\\n\"\n"

        ".ascii \"                n0 = pc_.cast(gdb.lookup_type(\\\"uintptr_t\\\")) % groups.dereference().type.sizeof\\n\"\n"
        ".ascii \"                pc_ = self.cpo.next(pc_, -n0)\\n\"\n"

        ".ascii \"                mask = (self.match_occupied(pc_.cast(groups.type).dereference()) >> (n0+1)) << (n0+1)\\n\"\n"
        ".ascii \"                while mask == 0:\\n\"\n"
        ".ascii \"                    pc_ = self.cpo.next(pc_, groups.dereference().type.sizeof)\\n\"\n"
        ".ascii \"                    p_ = self.cpo.next(p_, groups.dereference()[\\\"N\\\"])\\n\"\n"
        ".ascii \"                    mask = self.match_occupied(pc_.cast(groups.type).dereference())\\n\"\n"

        ".ascii \"                n = BoostUnorderedHelpers.countr_zero(mask)\\n\"\n"
        ".ascii \"                if self.is_sentinel(pc_.cast(groups.type).dereference(), n):\\n\"\n"
        ".ascii \"                    p_ = 0\\n\"\n"
        ".ascii \"                else:\\n\"\n"
        ".ascii \"                    pc_ = self.cpo.next(pc_, n)\\n\"\n"
        ".ascii \"                    p_ = self.cpo.next(p_, n - n0)\\n\"\n"

        ".ascii \"        return generator()\\n\"\n"

        ".ascii \"class BoostUnorderedFoaIteratorPrinter:\\n\"\n"
        ".ascii \"    def __init__(self, val):\\n\"\n"
        ".ascii \"        self.val = val\\n\"\n"
        ".ascii \"        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val[\\\"p_\\\"])\\n\"\n"

        ".ascii \"    def to_string(self):\\n\"\n"
        ".ascii \"        if self.valid():\\n\"\n"
        ".ascii \"            element = self.cpo.to_address(self.val[\\\"p_\\\"])\\n\"\n"
        ".ascii \"            pointer = BoostUnorderedHelpers.maybe_unwrap_foa_element(element)\\n\"\n"
        ".ascii \"            value = self.cpo.to_address(pointer).dereference()\\n\"\n"
        ".ascii \"            return f\\\"iterator = {{ {value} }}\\\"\\n\"\n"
        ".ascii \"        else:\\n\"\n"
        ".ascii \"            return \\\"iterator = { end iterator }\\\"\\n\"\n"

        ".ascii \"    def valid(self):\\n\"\n"
        ".ascii \"        return (self.cpo.to_address(self.val[\\\"p_\\\"]) != 0) and (self.cpo.to_address(self.val[\\\"pc_\\\"]) != 0)\\n\"\n"

        ".ascii \"def boost_unordered_build_pretty_printer():\\n\"\n"
        ".ascii \"    pp = gdb.printing.RegexpCollectionPrettyPrinter(\\\"boost_unordered\\\")\\n\"\n"
        ".ascii \"    add_template_printer = lambda name, printer: pp.add_printer(name, f\\\"^{name}<.*>$\\\", printer)\\n\"\n"
        ".ascii \"    add_concrete_printer = lambda name, printer: pp.add_printer(name, f\\\"^{name}$\\\", printer)\\n\"\n"

        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_map\\\", BoostUnorderedFcaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_multimap\\\", BoostUnorderedFcaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_set\\\", BoostUnorderedFcaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_multiset\\\", BoostUnorderedFcaPrinter)\\n\"\n"

        ".ascii \"    add_template_printer(\\\"boost::unordered::detail::iterator_detail::iterator\\\", BoostUnorderedFcaIteratorPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::detail::iterator_detail::c_iterator\\\", BoostUnorderedFcaIteratorPrinter)\\n\"\n"

        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_flat_map\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_flat_set\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_node_map\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::unordered_node_set\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::concurrent_flat_map\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::concurrent_flat_set\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::concurrent_node_map\\\", BoostUnorderedFoaPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::concurrent_node_set\\\", BoostUnorderedFoaPrinter)\\n\"\n"

        ".ascii \"    add_template_printer(\\\"boost::unordered::detail::foa::table_iterator\\\", BoostUnorderedFoaIteratorPrinter)\\n\"\n"

        ".ascii \"    add_concrete_printer(\\\"boost::unordered::detail::foa::table_core_cumulative_stats\\\", BoostUnorderedFoaTableCoreCumulativeStatsPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::detail::foa::cumulative_stats\\\", BoostUnorderedFoaCumulativeStatsPrinter)\\n\"\n"
        ".ascii \"    add_template_printer(\\\"boost::unordered::detail::foa::concurrent_cumulative_stats\\\", BoostUnorderedFoaCumulativeStatsPrinter)\\n\"\n"

        ".ascii \"    return pp\\n\"\n"

        ".ascii \"gdb.printing.register_pretty_printer(gdb.current_objfile(), boost_unordered_build_pretty_printer())\\n\"\n"



        ".ascii \"# https://sourceware.org/gdb/current/onlinedocs/gdb.html/Writing-an-Xmethod.html\\n\"\n"
        ".ascii \"class BoostUnorderedFoaGetStatsMethod(gdb.xmethod.XMethod):\\n\"\n"
        ".ascii \"    def __init__(self):\\n\"\n"
        ".ascii \"        gdb.xmethod.XMethod.__init__(self, \\\"get_stats\\\")\\n\"\n"

        ".ascii \"    def get_worker(self, method_name):\\n\"\n"
        ".ascii \"        if method_name == \\\"get_stats\\\":\\n\"\n"
        ".ascii \"            return BoostUnorderedFoaGetStatsWorker()\\n\"\n"

        ".ascii \"class BoostUnorderedFoaGetStatsWorker(gdb.xmethod.XMethodWorker):\\n\"\n"
        ".ascii \"    def get_arg_types(self):\\n\"\n"
        ".ascii \"        return None\\n\"\n"

        ".ascii \"    def get_result_type(self, obj):\\n\"\n"
        ".ascii \"        return gdb.lookup_type(\\\"boost::unordered::detail::foa::table_core_cumulative_stats\\\")\\n\"\n"

        ".ascii \"    def __call__(self, obj):\\n\"\n"
        ".ascii \"        try:\\n\"\n"
        ".ascii \"            return obj[\\\"table_\\\"][\\\"cstats\\\"]\\n\"\n"
        ".ascii \"        except gdb.error:\\n\"\n"
        ".ascii \"            print(\\\"Error: Binary was compiled without stats. Recompile with `BOOST_UNORDERED_ENABLE_STATS` defined.\\\")\\n\"\n"
        ".ascii \"            return\\n\"\n"

        ".ascii \"class BoostUnorderedFoaMatcher(gdb.xmethod.XMethodMatcher):\\n\"\n"
        ".ascii \"    def __init__(self):\\n\"\n"
        ".ascii \"        gdb.xmethod.XMethodMatcher.__init__(self, 'BoostUnorderedFoaMatcher')\\n\"\n"
        ".ascii \"        self.methods = [BoostUnorderedFoaGetStatsMethod()]\\n\"\n"

        ".ascii \"    def match(self, class_type, method_name):\\n\"\n"
        ".ascii \"        template_name = f\\\"{class_type.strip_typedefs()}\\\".split(\\\"<\\\")[0]\\n\"\n"
        ".ascii \"        regex = \\\"^boost::unordered::(unordered|concurrent)_(flat|node)_(map|set)$\\\"\\n\"\n"
        ".ascii \"        if not re.match(regex, template_name):\\n\"\n"
        ".ascii \"            return None\\n\"\n"

        ".ascii \"        workers = []\\n\"\n"
        ".ascii \"        for method in self.methods:\\n\"\n"
        ".ascii \"            if method.enabled:\\n\"\n"
        ".ascii \"                worker = method.get_worker(method_name)\\n\"\n"
        ".ascii \"                if worker:\\n\"\n"
        ".ascii \"                    workers.append(worker)\\n\"\n"
        ".ascii \"        return workers\\n\"\n"

        ".ascii \"gdb.xmethod.register_xmethod_matcher(None, BoostUnorderedFoaMatcher())\\n\"\n"



        ".ascii \"\\\"\\\"\\\" Fancy pointer support \\\"\\\"\\\"\\n\"\n"

        ".ascii \"\\\"\\\"\\\"\\n\"\n"
        ".ascii \"To allow your own fancy pointer type to interact with Boost.Unordered GDB pretty-printers,\\n\"\n"
        ".ascii \"create a pretty-printer for your own type with the following additional methods.\\n\"\n"

        ".ascii \"(Note, this is assuming the presence of a type alias `pointer` for the underlying\\n\"\n"
        ".ascii \"raw pointer type, Substitute whichever name is applicable in your case.)\\n\"\n"

        ".ascii \"`boost_to_address(fancy_ptr)`\\n\"\n"
        ".ascii \"    * A static method, but `@staticmethod` is not required\\n\"\n"
        ".ascii \"    * Parameter `fancy_ptr` of type `gdb.Value`\\n\"\n"
        ".ascii \"        * Its `.type` will be your fancy pointer type\\n\"\n"
        ".ascii \"    * Returns a `gdb.Value` with the raw pointer equivalent to your fancy pointer\\n\"\n"
        ".ascii \"        * This method should be equivalent to calling `operator->()` on your fancy pointer in C++\\n\"\n"

        ".ascii \"`boost_next(raw_ptr, offset)`\\n\"\n"
        ".ascii \"    * Parameter `raw_ptr` of type `gdb.Value`\\n\"\n"
        ".ascii \"        * Its `.type` will be `pointer`\\n\"\n"
        ".ascii \"    * Parameter `offset`\\n\"\n"
        ".ascii \"        * Either has integer type, or is of type `gdb.Value` with an underlying integer\\n\"\n"
        ".ascii \"    * Returns a `gdb.Value` with the raw pointer equivalent to your fancy pointer, as if you did the following operations\\n\"\n"
        ".ascii \"        1. Convert the incoming raw pointer to your fancy pointer\\n\"\n"
        ".ascii \"        2. Use operator+= to add the offset to the fancy pointer\\n\"\n"
        ".ascii \"        3. Convert back to the raw pointer\\n\"\n"
        ".ascii \"    * Note, you will not actually do these operations as stated. You will do equivalent lower-level operations that emulate having done the above\\n\"\n"
        ".ascii \"        * Ultimately, it will be as if you called `operator+()` on your fancy pointer in C++, but using only raw pointers\\n\"\n"

        ".ascii \"Example\\n\"\n"
        ".ascii \"```\\n\"\n"
        ".ascii \"class MyFancyPtrPrinter:\\n\"\n"
        ".ascii \"    ...\\n\"\n"

        ".ascii \"    # Equivalent to `operator->()`\\n\"\n"
        ".ascii \"    def boost_to_address(fancy_ptr):\\n\"\n"
        ".ascii \"        ...\\n\"\n"
        ".ascii \"        return ...\\n\"\n"

        ".ascii \"    # Equivalent to `operator+()`\\n\"\n"
        ".ascii \"    def boost_next(raw_ptr, offset):\\n\"\n"
        ".ascii \"        ...\\n\"\n"
        ".ascii \"        return ...\\n\"\n"

        ".ascii \"    ...\\n\"\n"
        ".ascii \"```\\n\"\n"
        ".ascii \"\\\"\\\"\\\"\\n\"\n"

        ".byte 0\n"
        ".popsection\n");
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif // defined(__ELF__)
#endif // !defined(BOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS)

#endif // !defined(BOOST_UNORDERED_UNORDERED_PRINTERS_HPP)
