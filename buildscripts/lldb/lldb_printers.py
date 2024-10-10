"""LLDB Pretty-printers for MongoDB.

To import script in lldb, run:

   command script import buildscripts/lldb/lldb_printers.py

This file must maintain Python 2 and 3 compatibility until Apple
upgrades to Python 3 and updates their LLDB to use it.
"""

from __future__ import print_function

import datetime
import struct
import sys
import uuid

import lldb

try:
    import bson
    from bson import Decimal128, json_util
    from bson.codec_options import DEFAULT_CODEC_OPTIONS
except ImportError:
    print("Warning: Could not load bson library for Python {}.".format(sys.version))
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None


def __lldb_init_module(debugger, *_args):
    """Register pretty printers."""
    debugger.HandleCommand(
        "type summary add -s 'A${*var.__ptr_.__value_}' -x '^std::__1::unique_ptr<.+>$'"
    )

    debugger.HandleCommand("type summary add -s '${var._value}' -x '^mongo::AtomicWord<.+>$'")
    debugger.HandleCommand("type summary add -s '${var._M_base._M_i}' 'std::atomic<bool>'")
    debugger.HandleCommand("type summary add -s '${var._M_i}' -x '^std::atomic<.+>$'")

    debugger.HandleCommand("type summary add mongo::BSONObj -F lldb_printers.BSONObjPrinter")
    debugger.HandleCommand(
        "type summary add mongo::BSONElement -F lldb_printers.BSONElementPrinter"
    )

    debugger.HandleCommand("type summary add mongo::Status -F lldb_printers.StatusPrinter")
    debugger.HandleCommand(
        "type summary add -x '^mongo::StatusWith<.+>$' -F lldb_printers.StatusWithPrinter"
    )

    debugger.HandleCommand("type summary add mongo::StringData -F lldb_printers.StringDataPrinter")
    debugger.HandleCommand("type summary add mongo::NamespaceString --summary-string '${var._ns}'")

    debugger.HandleCommand("type summary add mongo::UUID -F lldb_printers.UUIDPrinter")
    debugger.HandleCommand("type summary add mongo::Decimal128 -F lldb_printers.Decimal128Printer")
    debugger.HandleCommand("type summary add mongo::Date_t -F lldb_printers.Date_tPrinter")

    debugger.HandleCommand(
        "type summary add --summary-string '${var.m_pathname}' 'boost::filesystem::path'"
    )

    debugger.HandleCommand(
        "type synthetic add -x '^boost::optional<.+>$' --python-class lldb_printers.OptionalPrinter"
    )
    debugger.HandleCommand(
        "type synthetic add -x '^std::unique_ptr<.+>$' --python-class lldb_printers.UniquePtrPrinter"
    )

    debugger.HandleCommand(
        "type summary add -x '^boost::optional<.+>$' -F lldb_printers.OptionalSummaryPrinter"
    )
    debugger.HandleCommand(
        "type summary add mongo::ConstDataRange -F lldb_printers.ConstDataRangePrinter"
    )

    debugger.HandleCommand(
        "type synthetic add  -x '^mongo::stdx::unordered_set<.+>$' --python-class lldb_printers.AbslHashSetPrinter"
    )
    debugger.HandleCommand(
        "type synthetic add -x '^mongo::stdx::unordered_map<.+>$' --python-class lldb_printers.AbslHashSetPrinter"
    )


#############################
# Pretty Printer Defintions #
#############################


def StatusPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Pretty-Prints MongoDB Status objects."""
    err = valobj.GetChildMemberWithName("_error")
    px = err.GetChildMemberWithName("px")
    if px.GetValueAsUnsigned() == 0:
        return "Status::OK()"
    code = px.GetChildMemberWithName("code").GetValue()
    reason = px.GetChildMemberWithName("reason").GetSummary()
    return "Status({}, {})".format(code, reason)


def StatusWithPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Extend the StatusPrinter to print the value of With for a StatusWith."""
    status = valobj.GetChildMemberWithName("_status")
    code = (
        status.GetChildMemberWithName("_error")
        .GetChildMemberWithName("px")
        .GetChildMemberWithName("code")
        .GetValueAsUnsigned()
    )
    if code == 0:
        return "StatusWith(OK, {})".format(valobj.GetChildMemberWithName("_t").children[0])
    rep = StatusPrinter(status)
    return rep.replace("Status", "StatusWith", 1)


def StringDataPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Print StringData value."""
    ptr = valobj.GetChildMemberWithName("_data").GetValueAsUnsigned()
    if ptr == 0:
        return "nullptr"

    size1 = valobj.GetChildMemberWithName("_size").GetValueAsUnsigned(0)
    return '"{}"'.format(valobj.GetProcess().ReadMemory(ptr, size1, lldb.SBError()).decode("utf-8"))


def read_memory_as_hex(process, address, size):
    err = lldb.SBError()
    raw_memory = process.ReadMemory(address, size, err)

    # Format bytes as a space delimited list of hex bytes
    ba = bytearray(raw_memory)
    return "0x" + ba.hex(" ", 1)


def ConstDataRangePrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Pretty-Prints MongoDB Status objects."""
    begin_value = valobj.GetChildMemberWithName("_begin")
    begin = begin_value.GetValueAsUnsigned()
    end = valobj.GetChildMemberWithName("_end").GetValueAsUnsigned()

    if begin == 0:
        return "nullptr"

    size = end - begin
    value = None
    max_hex = min(50, size)

    if size:
        value = read_memory_as_hex(valobj.GetProcess(), begin, max_hex)

    if max_hex == 50:
        value += "..."

    return "size=%d,v=%s" % (size, value)


def BSONObjPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Print a BSONObj in a JSON format."""
    ptr = valobj.GetChildMemberWithName("_objdata").GetValueAsUnsigned()

    bson_size = valobj.GetProcess().ReadMemory(ptr, 4, lldb.SBError())
    if bson_size is None:
        return None

    size = struct.unpack("<I", bson_size)[0]
    if size < 5 or size > 17 * 1024 * 1024:
        return None
    mem = bytes(memoryview(valobj.GetProcess().ReadMemory(ptr, size, lldb.SBError())))
    if not bson.is_valid(mem):
        return None

    buf_str = bson.decode(mem)
    obj = json_util.dumps(buf_str, indent=4)
    # If the object is huge then just dump it as one line
    if obj.count("\n") > 1000:
        return json_util.dumps(buf_str)
    # Otherwise try to be nice and pretty print the JSON
    return obj


def BSONElementPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Print a BSONElement in a JSON format."""
    ptr = valobj.GetChildMemberWithName("data").GetValueAsUnsigned()
    size = valobj.GetChildMemberWithName("totalSize").GetValueAsUnsigned()

    if size <= 1:
        return "<empty>"

    mem = bytes(memoryview(valobj.GetProcess().ReadMemory(ptr, size, lldb.SBError())))

    # Call an internal bson method to directly convert an BSON element to a string
    el_tuple = bson._element_to_dict(mem, memoryview(mem), 0, len(mem), DEFAULT_CODEC_OPTIONS)  # pylint: disable=protected-access

    return '"%s": %s' % (el_tuple[0], el_tuple[1])


def Date_tPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Print a Date_t in a string format."""
    millis = valobj.GetChildMemberWithName("millis").GetValueAsUnsigned()

    if millis == 9223372036854775807:
        return "Date_t::max()"

    if millis == 0:
        return "Date_t::min()"

    dt = datetime.datetime.utcfromtimestamp(millis)

    return dt.isoformat()


def UUIDPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Print the UUID's hex string value."""
    char_array = valobj.GetChildMemberWithName("_uuid").GetChildAtIndex(0)
    raw_bytes = [x.GetValueAsUnsigned() for x in char_array]
    uuid_hex_bytes = [hex(b)[2:].zfill(2) for b in raw_bytes]
    return str(uuid.UUID("".join(uuid_hex_bytes)))


def Decimal128Printer(valobj, *_args):  # pylint: disable=invalid-name
    """Print the Decimal128's string value."""
    value = valobj.GetChildMemberWithName("_value")
    low64 = value.GetChildMemberWithName("low64").GetValueAsUnsigned()
    high64 = value.GetChildMemberWithName("high64").GetValueAsUnsigned()
    return Decimal128((high64, low64))


class UniquePtrPrinter:
    """Pretty printer for std::unique_ptr."""

    def __init__(self, valobj, *_args):
        """Store valobj and retrieve object held at the unique_ptr."""
        self.valobj = valobj
        self.update()

    def num_children(self):
        """Match LLDB's expected API."""
        return 1

    def get_child_index(self, name):
        """Match LLDB's expected API."""
        if name == "ptr":
            return 0
        else:
            return None

    def get_child_at_index(self, index):
        """Match LLDB's expected API.

        Always prints object pointed at by the ptr.
        """
        if index == 0:
            return (
                self.valobj.GetChildMemberWithName("__ptr_")
                .GetChildMemberWithName("__value_")
                .Dereference()
            )
        else:
            return None

    def has_children(self):
        """Match LLDB's expected API."""
        return True

    def update(self):
        """Match LLDB's expected API."""
        pass


class OptionalPrinter:
    """Pretty printer for boost::optional."""

    def __init__(self, valobj, *_args):
        """Store the valobj and get the value of the optional."""
        self.valobj = valobj
        self.update()

    def num_children(self):
        """Match LLDB's expected API."""
        return 1

    def get_child_index(self, name):
        """Match LLDB's expected API."""
        if name == "value":
            return 0
        else:
            return None

    def get_child_at_index(self, index):
        """Match LLDB's expected API."""
        if index == 0:
            return self.value
        else:
            return None

    def has_children(self):
        """Match LLDB's expected API."""
        return True

    def update(self):
        """Check if the optional has changed."""
        self.is_init = self.valobj.GetChildMemberWithName("m_initialized").GetValueAsUnsigned() != 0
        self.value = None
        if self.is_init:
            temp_type = self.valobj.GetType().GetTemplateArgumentType(0)
            storage = self.valobj.GetChildMemberWithName("m_storage")
            self.value = storage.Cast(temp_type)


# In LLDB, some values have either values, summary or description
# Value - integer types
# Summary - string or custom printers
def optional_sb_value_to_string(sb_value):
    if sb_value is None:
        return "boost::none"

    if sb_value.value is not None:
        return "{}".format(sb_value.value)
    if sb_value.summary is not None:
        return sb_value.summary

    # __str__ is SBValue::GetDescription() and returns "No value" in case of Python "None"
    desc = sb_value.__str__()
    if desc == "No value":
        return "boost::none"

    return desc


def OptionalSummaryPrinter(valobj, *_args):  # pylint: disable=invalid-name
    """Pretty-Prints boost::optional objects."""
    # This is displayed in vscode variables windows
    # The input is from OptionalPrinter
    if valobj is not None:
        return optional_sb_value_to_string(valobj.children[0])
    else:
        return "boost::none"


class AbslHashSetPrinter:
    """Pretty printer for absl::container_internal::raw_hash_set."""

    def __init__(self, valobj, *_args):
        """Store the valobj and get the value of the hash_set."""
        self.valobj = valobj
        self.capacity = 0
        self.data_size = 0
        self.data_type = None

    def num_children(self):
        """Match LLDB's expected API."""
        return self.valobj.GetChildMemberWithName("size_").GetValueAsUnsigned()

    def get_child_index(self, _name):
        """Match LLDB's expected API."""
        return None

    def get_child_at_index(self, index):
        """Match LLDB's expected API."""
        pos = 0
        count = 0
        ctrl = self.valobj.GetChildMemberWithName("ctrl_")
        if index == 0 and ctrl.GetChildAtIndex(pos, False, True).GetValueAsSigned() > 0:
            pos = 1
        else:
            while count <= index and pos <= self.capacity:
                slot = ctrl.GetChildAtIndex(pos, False, True).GetValueAsSigned()
                if slot >= 0:
                    count += 1

                pos += 1

        value = self.valobj.GetChildMemberWithName("slots_").CreateChildAtOffset(
            "%d" % (index), (pos - 1) * self.data_size, self.data_type
        )
        return value.Dereference()

    def has_children(self):
        """Match LLDB's expected API."""
        return True

    def update(self):
        self.capacity = self.valobj.GetChildMemberWithName("capacity_").GetValueAsUnsigned()

        self.data_type = self.valobj.GetChildMemberWithName("slots_").GetType()

        self.data_size = self.data_type.GetByteSize()

        try:
            self.data_type = resolve_type_to_base(
                self.valobj.GetChildMemberWithName("slots_").GetType()
            ).GetPointerType()
        except:  # pylint: disable=bare-except
            print("Exception: " + str(sys.exc_info()))


class AbslHashMapPrinter:
    """Pretty printer for absl::container_internal::raw_hash_map."""

    def __init__(self, valobj, _internal_dict):
        """Store the valobj and get the value of the hash_map."""
        self.valobj = valobj
        self.capacity = 0
        self.data_size = 0
        self.data_type = None

    def num_children(self):
        """Match LLDB's expected API."""
        return self.valobj.GetChildMemberWithName("size_").GetValueAsUnsigned()

    def get_child_index(self, _name):
        """Match LLDB's expected API."""
        return None

    def get_child_at_index(self, index):
        """Match LLDB's expected API."""
        pos = 0
        count = 0
        ctrl = self.valobj.GetChildMemberWithName("ctrl_")
        if index == 0 and ctrl.GetChildAtIndex(pos, False, True).GetValueAsSigned() > 0:
            pos = 1
        else:
            while count <= index and pos <= self.capacity:
                slot = ctrl.GetChildAtIndex(pos, False, True).GetValueAsSigned()
                if slot >= 0:
                    count += 1

                pos += 1

        return (
            self.valobj.GetChildMemberWithName("slots_")
            .GetChildAtIndex(pos - 1, False, True)
            .Dereference()
        )

    def has_children(self):
        """Match LLDB's expected API."""
        return True

    def update(self):
        self.capacity = self.valobj.GetChildMemberWithName("capacity_").GetValueAsUnsigned()
        self.data_type = self.valobj.GetChildMemberWithName("slots_").GetType()
        self.data_size = self.data_type.GetByteSize()


#####################################################################################
# LLDB Debugging utility functions
#
def print_type_base(data_type):
    print("type: %s " % (data_type))
    print("basic_type: %s " % (data_type.GetBasicType()))
    # print("canonical: %s " % (data_type.GetCanonicalType()))
    print("dereference: %s " % (data_type.GetDereferencedType()))
    print("display type: %s " % (data_type.GetDisplayTypeName()))
    print("IsTypeDef: %s " % (data_type.IsTypedefType()))
    print("IsPointer: %s " % (data_type.IsPointerType()))
    print("IsReference: %s " % (data_type.IsReferenceType()))
    print("IsTypeComplete: %s " % (data_type.IsTypeComplete()))
    print("IsAnonymousType: %s " % (data_type.IsAnonymousType()))
    print("IsArrayType: %s " % (data_type.IsArrayType()))
    print("IsAggregateType: %s " % (data_type.IsArrayType()))
    print("IsFunctionType: %s " % (data_type.IsFunctionType()))
    print("IsPolymorphicClass: %s " % (data_type.IsPolymorphicClass()))
    print("GetNumberOfFields: %s " % (data_type.GetNumberOfFields()))
    print("GetNumberOfMemberFunctions: %s " % (data_type.GetNumberOfMemberFunctions()))
    print("GetNumberOfTemplateArguments: %s " % (data_type.GetNumberOfTemplateArguments()))
    print("GetNumberOfVirtualBaseClasses: %s " % (data_type.GetNumberOfVirtualBaseClasses()))
    print("GetNumberOfDirectBaseClasses: %s " % (data_type.GetNumberOfDirectBaseClasses()))


def print_type(data_type):
    if isinstance(data_type, lldb.SBTypeMember):
        print("TypeMember: " + str(data_type))
        print_type(data_type.GetType())
        return

    print_type_base(data_type)
    if data_type.IsPointerType():
        print("---")
        print_type(data_type.GetPointeeType())


def walk_type_to_base(data_type):
    print("walk_type: %s " % (data_type))
    if data_type.IsPointerType():
        print("===P")
        walk_type_to_base(data_type.GetPointeeType())
    elif data_type.IsTypedefType():
        print("===D")
        walk_type_to_base(data_type.GetTypedefedType())
        print_type_base(data_type.GetTypedefedType())
    elif data_type.IsReferenceType():
        print("===R")
        walk_type_to_base(data_type.GetReferenceType())


def resolve_type_to_base(data_type):
    if isinstance(data_type, lldb.SBTypeMember):
        return resolve_type_to_base(data_type.GetType())

    if data_type.IsPointerType():
        return resolve_type_to_base(data_type.GetPointeeType())
    elif data_type.IsTypedefType():
        return resolve_type_to_base(data_type.GetTypedefedType())
    elif data_type.IsReferenceType():
        return resolve_type_to_base(data_type.GetReferenceType())

    return data_type


def dump_value(value):
    print("====================================")
    print("value.addr: %s" % (value.addr))
    print("value.address_of: %s" % (value.address_of))
    print("value.changed: %s" % (value.changed))
    print("value.child: %s" % (value.child))
    print("value.children: %s" % (value.children))
    print("value.data: %s" % (value.data))
    print("value.deref: %s" % (value.deref))
    print("value.description: %s" % (value.description))
    print("value.dynamic: %s" % (value.dynamic))
    print("value.error: %s" % (value.error))
    print("value.format: %s" % (value.format))
    print("value.frame: %s" % (value.frame))
    print("value.is_in_scope: %s" % (value.is_in_scope))
    print("value.load_addr: %s" % (value.load_addr))
    print("value.location: %s" % (value.location))
    print("value.name: %s" % (value.name))
    print("value.num_children: %s" % (value.num_children))
    print("value.path: %s" % (value.path))
    print("value.process: %s" % (value.process))
    print("value.signed: %s" % (value.signed))
    print("value.size: %s" % (value.size))
    print("value.summary: %s" % (value.summary))
    print("value.target: %s" % (value.target))
    print("value.thread: %s" % (value.thread))
    print("value.type: %s" % (value.type))
    print("value.unsigned: %s" % (value.unsigned))
    print("value.value: %s" % (value.value))
    print("value.value_type: %s" % (value.value_type))
    print("----------------------------------------")
