# Copyright 2024 Braden Ganetsky
# Distributed under the Boost Software License, Version 1.0.
# https://www.boost.org/LICENSE_1_0.txt

import gdb.printing
import gdb.xmethod
import re
import math

class BoostUnorderedHelpers:
    def maybe_unwrap_atomic(n):
        if f"{n.type.strip_typedefs()}".startswith("std::atomic<"):
            underlying_type = n.type.template_argument(0)
            return n.cast(underlying_type)
        else:
            return n
            
    def maybe_unwrap_foa_element(e):
        if f"{e.type.strip_typedefs()}".startswith("boost::unordered::detail::foa::element_type<"):
            return e["p"]
        else:
            return e

    def maybe_unwrap_reference(value):
        if value.type.code == gdb.TYPE_CODE_REF:
            return value.referenced_value()
        else:
            return value

    def countr_zero(n):
        for i in range(32):
            if (n & (1 << i)) != 0:
                return i
        return 32

class BoostUnorderedPointerCustomizationPoint:
    def __init__(self, any_ptr):
        vis = gdb.default_visualizer(any_ptr)
        if vis is None:
            self.to_address = lambda ptr: ptr
            self.next = lambda ptr, offset: ptr + offset
        else:
            self.to_address = lambda ptr: ptr if (ptr.type.code == gdb.TYPE_CODE_PTR) else type(vis).boost_to_address(ptr)
            self.next = lambda ptr, offset: type(vis).boost_next(ptr, offset)

class BoostUnorderedFcaPrinter:
    def __init__(self, val):
        self.val = BoostUnorderedHelpers.maybe_unwrap_reference(val)
        self.name = f"{self.val.type.strip_typedefs()}".split("<")[0]
        self.name = self.name.replace("boost::unordered::", "boost::")
        self.is_map = self.name.endswith("map")
        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val["table_"]["buckets_"]["buckets"])

    def to_string(self):
        size = self.val["table_"]["size_"]
        return f"{self.name} with {size} elements"

    def display_hint(self):
        return "map"

    def children(self):
        def generator():
            grouped_buckets = self.val["table_"]["buckets_"]

            size = grouped_buckets["size_"]
            buckets = grouped_buckets["buckets"]
            bucket_index = 0

            count = 0
            while bucket_index != size:
                current_bucket = self.cpo.next(self.cpo.to_address(buckets), bucket_index)
                node = self.cpo.to_address(current_bucket.dereference()["next"])
                while node != 0:
                    value = node.dereference()["buf"]["t_"]
                    if self.is_map:
                        first = value["first"]
                        second = value["second"]
                        yield "", first
                        yield "", second
                    else:
                        yield "", count
                        yield "", value
                    count += 1
                    node = self.cpo.to_address(node.dereference()["next"])
                bucket_index += 1
        
        return generator()

class BoostUnorderedFcaIteratorPrinter:
    def __init__(self, val):
        self.val = val
        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val["p"])

    def to_string(self):
        if self.valid():
            value = self.cpo.to_address(self.val["p"]).dereference()["buf"]["t_"]
            return f"iterator = {{ {value} }}"
        else:
            return "iterator = { end iterator }"

    def valid(self):
        return (self.cpo.to_address(self.val["p"]) != 0) and (self.cpo.to_address(self.val["itb"]["p"]) != 0)

class BoostUnorderedFoaTableCoreCumulativeStatsPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "[stats]"

    def display_hint(self):
        return "map"

    def children(self):
        def generator():
            members = ["insertion", "successful_lookup", "unsuccessful_lookup"]
            for member in members:
                yield "", member
                yield "", self.val[member]
        return generator()
    
class BoostUnorderedFoaCumulativeStatsPrinter:
    def __init__(self, val):
        self.val = val
        self.n = self.val["n"]
        self.N = self.val.type.template_argument(0)

    def display_hint(self):
        return "map"
    
    def children(self):
        def generator():
            yield "", "count"
            yield "", self.n

            sequence_stats_data = gdb.lookup_type("boost::unordered::detail::foa::sequence_stats_data")
            data = self.val["data"]
            arr = data.address.reinterpret_cast(sequence_stats_data.pointer())
            def build_string(idx):
                entry = arr[idx]
                avg = float(entry["m"])
                var = float(entry["s"] / self.n) if (self.n != 0) else 0.0
                dev = math.sqrt(var)
                return f"{{avg = {avg}, var = {var}, dev = {dev}}}"

            if self.N > 0:
                yield "", "probe_length"
                yield "", build_string(0)
            if self.N > 1:
                yield "", "num_comparisons"
                yield "", build_string(1)

        return generator()

class BoostUnorderedFoaPrinter:
    def __init__(self, val):
        self.val = BoostUnorderedHelpers.maybe_unwrap_reference(val)
        self.name = f"{self.val.type.strip_typedefs()}".split("<")[0]
        self.name = self.name.replace("boost::unordered::", "boost::")
        self.is_map = self.name.endswith("map")
        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val["table_"]["arrays"]["groups_"])

    def to_string(self):
        size = BoostUnorderedHelpers.maybe_unwrap_atomic(self.val["table_"]["size_ctrl"]["size"])
        return f"{self.name} with {size} elements"

    def display_hint(self):
        return "map"

    def is_regular_layout(self, group):
        typename = group["m"].type.strip_typedefs()
        array_size = typename.sizeof // typename.target().sizeof
        if array_size == 16:
            return True
        elif array_size == 2:
            return False

    def match_occupied(self, group):
        m = group["m"]
        at = lambda b: BoostUnorderedHelpers.maybe_unwrap_atomic(m[b]["n"])

        if self.is_regular_layout(group):
            bits = [1 << b for b in range(16) if at(b) == 0]
            return 0x7FFF & ~sum(bits)
        else:
            xx = at(0) | at(1)
            yy = xx | (xx >> 32)
            return 0x7FFF & (yy | (yy >> 16))

    def is_sentinel(self, group, pos):
        m = group["m"]
        at = lambda b: BoostUnorderedHelpers.maybe_unwrap_atomic(m[b]["n"])

        N = group["N"]
        sentinel_ = group["sentinel_"]
        if self.is_regular_layout(group):
            return pos == N-1 and at(N-1) == sentinel_
        else:
            return pos == N-1 and (at(0) & 0x4000400040004000) == 0x4000 and (at(1) & 0x4000400040004000) == 0

    def children(self):
        def generator():
            table = self.val["table_"]
            groups = self.cpo.to_address(table["arrays"]["groups_"])
            elements = self.cpo.to_address(table["arrays"]["elements_"])

            pc_ = groups.cast(gdb.lookup_type("unsigned char").pointer())
            p_ = elements
            first_time = True
            mask = 0
            n0 = 0
            n = 0

            count = 0
            while p_ != 0:
                # This if block mirrors the condition in the begin() call
                if (not first_time) or (self.match_occupied(groups.dereference()) & 1):
                    pointer = BoostUnorderedHelpers.maybe_unwrap_foa_element(p_)
                    value = self.cpo.to_address(pointer).dereference()
                    if self.is_map:
                        first = value["first"]
                        second = value["second"]
                        yield "", first
                        yield "", second
                    else:
                        yield "", count
                        yield "", value
                    count += 1
                first_time = False

                n0 = pc_.cast(gdb.lookup_type("uintptr_t")) % groups.dereference().type.sizeof
                pc_ = self.cpo.next(pc_, -n0)

                mask = (self.match_occupied(pc_.cast(groups.type).dereference()) >> (n0+1)) << (n0+1)
                while mask == 0:
                    pc_ = self.cpo.next(pc_, groups.dereference().type.sizeof)
                    p_ = self.cpo.next(p_, groups.dereference()["N"])
                    mask = self.match_occupied(pc_.cast(groups.type).dereference())
                
                n = BoostUnorderedHelpers.countr_zero(mask)
                if self.is_sentinel(pc_.cast(groups.type).dereference(), n):
                    p_ = 0
                else:
                    pc_ = self.cpo.next(pc_, n)
                    p_ = self.cpo.next(p_, n - n0)

        return generator()

class BoostUnorderedFoaIteratorPrinter:
    def __init__(self, val):
        self.val = val
        self.cpo = BoostUnorderedPointerCustomizationPoint(self.val["p_"])

    def to_string(self):
        if self.valid():
            element = self.cpo.to_address(self.val["p_"])
            pointer = BoostUnorderedHelpers.maybe_unwrap_foa_element(element)
            value = self.cpo.to_address(pointer).dereference()
            return f"iterator = {{ {value} }}"
        else:
            return "iterator = { end iterator }"

    def valid(self):
        return (self.cpo.to_address(self.val["p_"]) != 0) and (self.cpo.to_address(self.val["pc_"]) != 0)

def boost_unordered_build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("boost_unordered")
    add_template_printer = lambda name, printer: pp.add_printer(name, f"^{name}<.*>$", printer)
    add_concrete_printer = lambda name, printer: pp.add_printer(name, f"^{name}$", printer)

    add_template_printer("boost::unordered::unordered_map", BoostUnorderedFcaPrinter)
    add_template_printer("boost::unordered::unordered_multimap", BoostUnorderedFcaPrinter)
    add_template_printer("boost::unordered::unordered_set", BoostUnorderedFcaPrinter)
    add_template_printer("boost::unordered::unordered_multiset", BoostUnorderedFcaPrinter)

    add_template_printer("boost::unordered::detail::iterator_detail::iterator", BoostUnorderedFcaIteratorPrinter)
    add_template_printer("boost::unordered::detail::iterator_detail::c_iterator", BoostUnorderedFcaIteratorPrinter)

    add_template_printer("boost::unordered::unordered_flat_map", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::unordered_flat_set", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::unordered_node_map", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::unordered_node_set", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::concurrent_flat_map", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::concurrent_flat_set", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::concurrent_node_map", BoostUnorderedFoaPrinter)
    add_template_printer("boost::unordered::concurrent_node_set", BoostUnorderedFoaPrinter)
    
    add_template_printer("boost::unordered::detail::foa::table_iterator", BoostUnorderedFoaIteratorPrinter)

    add_concrete_printer("boost::unordered::detail::foa::table_core_cumulative_stats", BoostUnorderedFoaTableCoreCumulativeStatsPrinter)
    add_template_printer("boost::unordered::detail::foa::cumulative_stats", BoostUnorderedFoaCumulativeStatsPrinter)
    add_template_printer("boost::unordered::detail::foa::concurrent_cumulative_stats", BoostUnorderedFoaCumulativeStatsPrinter)

    return pp

gdb.printing.register_pretty_printer(gdb.current_objfile(), boost_unordered_build_pretty_printer())



# https://sourceware.org/gdb/current/onlinedocs/gdb.html/Writing-an-Xmethod.html
class BoostUnorderedFoaGetStatsMethod(gdb.xmethod.XMethod):
    def __init__(self):
        gdb.xmethod.XMethod.__init__(self, "get_stats")
 
    def get_worker(self, method_name):
        if method_name == "get_stats":
            return BoostUnorderedFoaGetStatsWorker()

class BoostUnorderedFoaGetStatsWorker(gdb.xmethod.XMethodWorker):
    def get_arg_types(self):
        return None

    def get_result_type(self, obj):
        return gdb.lookup_type("boost::unordered::detail::foa::table_core_cumulative_stats")
 
    def __call__(self, obj):
        try:
            return obj["table_"]["cstats"]
        except gdb.error:
            print("Error: Binary was compiled without stats. Recompile with `BOOST_UNORDERED_ENABLE_STATS` defined.")
            return
 
class BoostUnorderedFoaMatcher(gdb.xmethod.XMethodMatcher):
    def __init__(self):
        gdb.xmethod.XMethodMatcher.__init__(self, 'BoostUnorderedFoaMatcher')
        self.methods = [BoostUnorderedFoaGetStatsMethod()]
 
    def match(self, class_type, method_name):
        template_name = f"{class_type.strip_typedefs()}".split("<")[0]
        regex = "^boost::unordered::(unordered|concurrent)_(flat|node)_(map|set)$"
        if not re.match(regex, template_name):
            return None

        workers = []
        for method in self.methods:
            if method.enabled:
                worker = method.get_worker(method_name)
                if worker:
                    workers.append(worker)
        return workers

gdb.xmethod.register_xmethod_matcher(None, BoostUnorderedFoaMatcher())



""" Fancy pointer support """

"""
To allow your own fancy pointer type to interact with Boost.Unordered GDB pretty-printers,
create a pretty-printer for your own type with the following additional methods.

(Note, this is assuming the presence of a type alias `pointer` for the underlying
raw pointer type, Substitute whichever name is applicable in your case.)

`boost_to_address(fancy_ptr)`
    * A static method, but `@staticmethod` is not required
    * Parameter `fancy_ptr` of type `gdb.Value`
        * Its `.type` will be your fancy pointer type
    * Returns a `gdb.Value` with the raw pointer equivalent to your fancy pointer
        * This method should be equivalent to calling `operator->()` on your fancy pointer in C++

`boost_next(raw_ptr, offset)`
    * Parameter `raw_ptr` of type `gdb.Value`
        * Its `.type` will be `pointer`
    * Parameter `offset`
        * Either has integer type, or is of type `gdb.Value` with an underlying integer
    * Returns a `gdb.Value` with the raw pointer equivalent to your fancy pointer, as if you did the following operations
        1. Convert the incoming raw pointer to your fancy pointer
        2. Use operator+= to add the offset to the fancy pointer
        3. Convert back to the raw pointer
    * Note, you will not actually do these operations as stated. You will do equivalent lower-level operations that emulate having done the above
        * Ultimately, it will be as if you called `operator+()` on your fancy pointer in C++, but using only raw pointers

Example
```
class MyFancyPtrPrinter:
    ...

    # Equivalent to `operator->()`
    def boost_to_address(fancy_ptr):
        ...
        return ...
        
    # Equivalent to `operator+()`
    def boost_next(raw_ptr, offset):
        ...
        return ...
    
    ...
```
"""
