"""GDB Pretty-printers for MongoDB
"""
from __future__ import print_function

import gdb.printing
import struct
import sys

try:
    import bson
    import bson.json_util
    import collections
    from bson.codec_options import CodecOptions
except ImportError as e:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr"""
    return obj["_M_t"]['_M_head_impl']



###################################################################################################
#
# Pretty-Printers
#
###################################################################################################


class StatusPrinter(object):
    """Pretty-printer for mongo::Status"""
    OK = 0  # ErrorCodes::OK

    def __init__(self, val):
        self.val = val

    def to_string(self):
        if not self.val['_error']:
            return 'Status::OK()'

        code = self.val['_error']['code']
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_error'].dereference()
        location = info['location']
        reason = info['reason']
        if location:
            return 'Status(%s, %s, %s)' % (code, reason, location)
        else:
            return 'Status(%s, %s)' % (code, reason)


class StatusWithPrinter:
    """Pretty-printer for mongo::StatusWith<>"""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if not self.val['_status']['_error']:
            return 'StatusWith(OK, %s)' % (self.val['_t'])

        code = self.val['_status']['_error']['code']

        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_status']['_error'].dereference()
        location = info['location']
        reason = info['reason']
        if location:
            return 'StatusWith(%s, %s, %s)' % (code, reason, location)
        else:
            return 'StatusWith(%s, %s)' % (code, reason)


class StringDataPrinter:
    """Pretty-printer for mongo::StringData"""

    def __init__(self, val):
        self.val = val

    def display_hint(self):
        return 'string'

    def to_string(self):
        size = self.val["_size"]
        if size == -1:
            return self.val['_data'].lazy_string()
        else:
            return self.val['_data'].lazy_string(length=size)


class BSONObjPrinter:
    """Pretty-printer for mongo::BSONObj"""

    def __init__(self, val):
        self.val = val
        self.ptr = self.val['_objdata'].cast(gdb.lookup_type('void').pointer())
        # Handle the endianness of the BSON object size, which is represented as a 32-bit integer
        # in little-endian format.
        inferior = gdb.selected_inferior()
        self.size = struct.unpack('<I', inferior.read_memory(self.ptr, 4))[0]

    def display_hint(self):
        return 'map'

    def children(self):
        # Do not decode a BSONObj with an invalid size.
        if not bson or self.size < 5 or self.size > 17 * 1024 * 1024:
            return

        inferior = gdb.selected_inferior()
        buf = bytes(inferior.read_memory(self.ptr, self.size))
        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = bson.BSON.decode(buf, codec_options=options)

        for k, v in bsondoc.items():
            yield 'key', k
            yield 'value', bson.json_util.dumps(v)

    def to_string(self):
        ownership = "owned" if self.val['_ownedBuffer']['_buffer']['_holder']['px'] else "unowned"

        size = self.size
        # Print an invalid BSONObj size in hex.
        if size < 5 or size > 17 * 1024 * 1024:
            size = hex(size)

        if size == 5:
            return "%s empty BSONObj @ %s" % (ownership, self.ptr)
        else:
            return "%s BSONObj %s bytes @ %s" % (ownership, size, self.ptr)


class UnorderedFastKeyTablePrinter:
    """Pretty-printer for mongo::UnorderedFastKeyTable<>"""

    def __init__(self, val):
        self.val = val

        # Get the value_type by doing a type lookup
        valueTypeName = val.type.strip_typedefs().name + "::value_type"
        valueType = gdb.lookup_type(valueTypeName).target()
        self.valueTypePtr = valueType.pointer()

    def display_hint(self):
        return 'map'

    def to_string(self):
        return "UnorderedFastKeyTablePrinter<%s> with %s elems " % (
            self.val.type.template_argument(0), self.val["_size"])

    def children(self):
        cap = self.val["_area"]["_hashMask"] + 1
        it = get_unique_ptr(self.val["_area"]["_entries"])
        end = it + cap

        if it == 0:
            return

        while it != end:
            elt = it.dereference()
            it += 1
            if not elt['_used']:
                continue

            value = elt['_data']["__data"].cast(self.valueTypePtr).dereference()

            yield ('key', value['first'])
            yield ('value', value['second'])


class DecorablePrinter:
    """Pretty-printer for mongo::Decorable<>"""

    def __init__(self, val):
        self.val = val

        decl_vector = val["_decorations"]["_registry"]["_decorationInfo"]
        # TODO: abstract out navigating a std::vector
        self.start = decl_vector["_M_impl"]["_M_start"]
        finish = decl_vector["_M_impl"]["_M_finish"]
        decinfo_t = gdb.lookup_type('mongo::DecorationRegistry::DecorationInfo')
        self.count = int((int(finish) - int(self.start)) / decinfo_t.sizeof)

    def display_hint(self):
        return 'map'

    def to_string(self):
        return "Decorable<%s> with %s elems " % (self.val.type.template_argument(0),
                                                 self.count)

    def children(self):
        decorationData = get_unique_ptr(self.val["_decorations"]["_decorationData"])

        for index in range(self.count):
            descriptor = self.start[index]
            dindex = int(descriptor["descriptor"]["_index"])

            # In order to get the type stored in the decorable, we examine the type of its
            # constructor, and do some string manipulations.
            # TODO: abstract out navigating a std::function
            type_name = str(descriptor["constructor"]["_M_functor"]["_M_unused"]["_M_object"])
            type_name = type_name[0:len(type_name) - 1]
            type_name = type_name[0: type_name.rindex(">")]
            type_name = type_name[type_name.index("constructAt<"):].replace("constructAt<", "")

            # If the type is a pointer type, strip the * at the end.
            if type_name.endswith('*'):
                type_name = type_name[0:len(type_name) - 1]
            type_name = type_name.rstrip()

            # Cast the raw char[] into the actual object that is stored there.
            type_t = gdb.lookup_type(type_name)
            obj = decorationData[dindex].cast(type_t)

            yield ('key', "%d:%s:%s" % (index, obj.address, type_name))
            yield ('value', obj)


def find_match_brackets(search, opening='<', closing='>'):
    """Returns the index of the closing bracket that matches the first opening bracket.
       Returns -1 if no last matching bracket is found, i.e. not a template.

       Example:
         'Foo<T>::iterator<U>''
         returns 5
    """
    index = search.find(opening)
    if index == -1:
        return -1

    start = index + 1
    count = 1
    str_len = len(search)
    for index in range(start, str_len):
        c = search[index]

        if c == opening:
            count += 1
        elif c == closing:
            count -= 1

        if count == 0:
            return index

    return -1


class MongoSubPrettyPrinter(gdb.printing.SubPrettyPrinter):
    """Sub pretty printer managed by the pretty-printer collection"""

    def __init__(self, name, prefix, is_template, printer):
        super(MongoSubPrettyPrinter, self).__init__(name)
        self.prefix = prefix
        self.printer = printer
        self.is_template = is_template


class MongoPrettyPrinterCollection(gdb.printing.PrettyPrinter):
    """MongoDB-specific printer printer collection that ignores subtypes.
    It will match 'HashTable<T> but not 'HashTable<T>::iterator' when asked for 'HashTable'.
    """

    def __init__(self):
        super(MongoPrettyPrinterCollection, self).__init__("mongo", [])

    def add(self, name, prefix, is_template, printer):
        self.subprinters.append(MongoSubPrettyPrinter(name, prefix, is_template, printer))

    def __call__(self, val):

        # Get the type name.
        lookup_tag = gdb.types.get_basic_type(val.type).tag
        if not lookup_tag:
            lookup_tag = val.type.name
        if not lookup_tag:
            return None

        index = find_match_brackets(lookup_tag)

        # Ignore subtypes of classes
        # We do not want HashTable<T>::iterator as an example, just HashTable<T>
        if index == -1 or index + 1 == len(lookup_tag):
            for printer in self.subprinters:
                if printer.enabled and (
                   (printer.is_template and lookup_tag.find(printer.prefix) == 0) or
                   (not printer.is_template and lookup_tag == printer.prefix)):
                    return printer.printer(val)

        return None


def build_pretty_printer():
    pp = MongoPrettyPrinterCollection()
    pp.add('BSONObj', 'mongo::BSONObj', False, BSONObjPrinter)
    pp.add('Decorable', 'mongo::Decorable', True, DecorablePrinter)
    pp.add('Status', 'mongo::Status', False, StatusPrinter)
    pp.add('StatusWith', 'mongo::StatusWith', True, StatusWithPrinter)
    pp.add('StringData', 'mongo::StringData', False, StringDataPrinter)
    pp.add('UnorderedFastKeyTable', 'mongo::UnorderedFastKeyTable', True, UnorderedFastKeyTablePrinter)
    return pp

###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(
    gdb.current_objfile(),
    build_pretty_printer(),
    True)

print("MongoDB GDB pretty-printers loaded")
