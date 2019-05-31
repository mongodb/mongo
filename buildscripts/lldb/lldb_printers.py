"""LLDB Pretty-printers for MongoDB.

To import script in lldb, run:

   command script import buildscripts/lldb/lldb_printers.py

This file must maintain Python 2 and 3 compatibility until Apple
upgrades to Python 3 and updates their LLDB to use it.
"""
from __future__ import print_function

import string
import struct
import sys
import uuid

import lldb

try:
    import bson
    import collections
    from bson import json_util
    from bson.codec_options import CodecOptions
except ImportError as err:
    print("Warning: Could not load bson library for Python {}.".format(sys.version))
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None


def __lldb_init_module(debugger, dict):
    ############################
    # register pretty printers #
    ############################
    debugger.HandleCommand("type summary add -s 'A${*var.__ptr_.__value_}' -x '^std::__1::unique_ptr<.+>$'")
    debugger.HandleCommand("type summary add mongo::BSONObj -F lldb_printers.BSONObjPrinter")
    debugger.HandleCommand("type summary add mongo::Status -F lldb_printers.StatusPrinter")
    debugger.HandleCommand("type summary add mongo::StatusWith -F lldb_printers.StatusWithPrinter")
    debugger.HandleCommand("type summary add mongo::StringData -F lldb_printers.StringDataPrinter")
    debugger.HandleCommand("type summary add mongo::UUID -F lldb_printers.UUIDPrinter")
    debugger.HandleCommand("type summary add --summary-string '${var.m_pathname}' 'boost::filesystem::path'")
    debugger.HandleCommand("type synthetic add -x '^boost::optional<.+>$' --python-class lldb_printers.OptionalPrinter")
    debugger.HandleCommand("type synthetic add -x '^std::unique_ptr<.+>$' --python-class lldb_printers.UniquePtrPrinter")


#############################
# Pretty Printer Defintions #
#############################

def StatusPrinter(valobj, *args):
    err = valobj.GetChildMemberWithName("_error")
    code = err.\
        GetChildMemberWithName("code").\
        GetValue()
    if code is None:
        return "Status::OK()"
    reason = err.\
        GetChildMemberWithName("reason").\
        GetSummary()
    return "Status({}, {})".format(code, reason)


def StatusWithPrinter(valobj, *args):
    status = valobj.GetChildMemberWithName("_status")
    code = status.GetChildMemberWithName("_error").\
        GetChildMemberWithName("code").\
        GetValue()
    if code is None:
        return "StatusWith(OK, {})".format(valobj.GetChildMemberWithName("_t").GetValue())
    rep = StatusPrinter(status)
    return rep.replace("Status", "StatusWith", 1)


def StringDataPrinter(valobj, *args):
    ptr = valobj.GetChildMemberWithName("_data").GetValueAsUnsigned()
    size1 = valobj.GetChildMemberWithName("_size").GetValueAsUnsigned(0)
    return '"{}"'.format(valobj.GetProcess().ReadMemory(ptr, size1, lldb.SBError()).encode("utf-8"))


def BSONObjPrinter(valobj, *args):
    ptr = valobj.GetChildMemberWithName("_objdata").GetValueAsUnsigned()
    size = struct.unpack("<I", valobj.GetProcess().ReadMemory(ptr, 4, lldb.SBError()))[0]
    if size < 5 or size > 17 * 1024 * 1024:
        return
    buf = bson.BSON(bytes(valobj.GetProcess().ReadMemory(ptr, size, lldb.SBError())))
    buf_str = buf.decode()
    obj = json_util.dumps(buf_str, indent=4)
    # If the object is huge then just dump it as one line
    if obj.count("\n") > 1000:
        return json_util.dumps(buf_str)
    # Otherwise try to be nice and pretty print the JSON
    return obj


def UUIDPrinter(valobj, *args):
    char_array = valobj.GetChildMemberWithName("_uuid").GetChildAtIndex(0)
    raw_bytes = [x.GetValueAsUnsigned() for x in char_array]
    uuid_hex_bytes = [hex(b)[2:].zfill(2) for b in raw_bytes]
    return str(uuid.UUID("".join(uuid_hex_bytes)))


class UniquePtrPrinter:
    def __init__(self, valobj, dict):
        self.valobj = valobj
        self.update()

    def num_children(self):
        return 1

    def get_child_index(self, name):
        if name == "ptr":
            return 0
        else:
            return None

    def get_child_at_index(self, index):
        if index == 0:
            return self.valobj.GetChildMemberWithName("__ptr_").GetChildMemberWithName("__value_").Dereference()
        else:
            return None

    def has_children():
        return True

    def update(self):
        pass


class OptionalPrinter:
    def __init__(self, valobj, dict):
        self.valobj = valobj
        self.update()

    def num_children(self):
        return 1

    def get_child_index(self, name):
        if name == "value":
            return 0
        else:
            return None

    def get_child_at_index(self, index):
        if index == 0:
            return self.value
        else:
            return None

    def has_children():
        return True

    def update(self):
        self.is_init = self.valobj.GetChildMemberWithName("m_initialized").GetValueAsUnsigned() != 0
        self.value = None
        if self.is_init:
            temp_type = self.valobj.GetType().GetTemplateArgumentType(0)
            storage = self.valobj.GetChildMemberWithName("m_storage")
            self.value = storage.Cast(temp_type)

