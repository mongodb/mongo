#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

class BaseDataSet(object):
    """
    BaseDataSet is an abstract base class for other *DataSet classes.
    An object of this type should not be created directly.  These classes
    represent test data sets that can be used to populate tables and
    to check the contents of existing tables.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        self.testcase = testcase
        self.uri = uri
        self.rows = rows
        self.key_format = kwargs.get('key_format', 'S')
        self.value_format = kwargs.get('value_format', 'S')
        self.config = kwargs.get('config', '')
        self.projection = kwargs.get('projection', '')

    def create(self):
        self.testcase.session.create(self.uri, 'key_format=' + self.key_format
                                     + ',value_format=' + self.value_format
                                     + ',' + self.config)

    def store_one_cursor(self, c, i):
        c[self.key(i)] = self.value(i)

    def store_range_cursor(self, c, key, count):
        for i in range(key, key + count):
            self.store_one(c, i)

    def store_range(self, key, count):
        c = self.testcase.session.open_cursor(self.uri, None)
        for i in range(key, key + count):
            self.store_one_cursor(c, i)
        c.close()

    def fill(self):
        self.store_range(1, self.rows)

    def postfill_create(self):
        pass

    @classmethod
    def is_lsm(cls):
        return False

    def populate(self, create=True):
        self.testcase.pr('populate: ' + self.uri + ' with '
                         + str(self.rows) + ' rows')
        if create:
            self.create()
        self.fill()
        if create:
            self.postfill_create()

    # Create a key for a Simple or Complex data set.
    @staticmethod
    def key_by_format(i, key_format):
        if key_format == 'i' or key_format == 'r':
            return i
        elif key_format == 'u':
            return bytes(('%015d' % i).encode())
        elif key_format == 'S':
            return str('%015d' % i)
        else:
            raise AssertionError(
                'key: object has unexpected format: ' + key_format)

    # Deduce the source integer for a key in a Simple or Complex data set.
    @staticmethod
    def reverse_key_by_format(key, key_format):
        return int(key)

    # Create a value for a Simple data set.
    @staticmethod
    def value_by_format(i, value_format):
        if value_format == 'i' or value_format == 'r':
            return i
        elif value_format == 'u':
            return bytes((str(i) + ': abcdefghijklmnopqrstuvwxyz').encode())
        elif value_format == 'S':
            return str(i) + ': abcdefghijklmnopqrstuvwxyz'
        elif value_format == '8t':
            value = (
                0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xaa, 0xab,
                0xac, 0xad, 0xae, 0xaf, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
                0xb7, 0xb8, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf)
            return value[i % len(value)]
        elif value_format == '6t':
            value = (
                0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x2b,
                0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                0x37, 0x38, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f)
            return value[i % len(value)]
        else:
            raise AssertionError(
                'value: object has unexpected format: ' + value_format)

    # Create a key for this data set.  Simple and Complex data sets have
    # the same key space.
    def key(self, i):
        return BaseDataSet.key_by_format(i, self.key_format)

    def check(self):
        self.testcase.pr('check: ' + self.uri)
        cursor = self.testcase.session.open_cursor(
            self.uri + self.projection, None, None)
        self.check_cursor(cursor)
        cursor.close()

class SimpleDataSet(BaseDataSet):
    """
    SimpleDataSet creates a table with a single key and value that is
    populated with predefined data, up to the requested number of rows.
    key_format and value_format may be set in the constructor to
    override the simple string defaults.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        super(SimpleDataSet, self).__init__(testcase, uri, rows, **kwargs)

    # A value suitable for checking the value returned by a cursor.
    def comparable_value(self, i):
        return BaseDataSet.value_by_format(i, self.value_format)

    # A value suitable for assigning to a cursor.
    def value(self, i):
        return BaseDataSet.value_by_format(i, self.value_format)

    def check_cursor(self, cursor):
        i = 0
        for key, val in cursor:
            i += 1
            self.testcase.assertEqual(key, self.key(i))
            if cursor.value_format == '8t' and val == 0:    # deleted
                continue
            self.testcase.assertEqual(val, self.value(i))
        self.testcase.assertEqual(i, self.rows)

class SimpleLSMDataSet(SimpleDataSet):
    """
    SimpleLSMDataSet is identical to SimpleDataSet, but using LSM files
    via the type=lsm configuration.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        kwargs['config'] = kwargs.get('config', '') + ',type=lsm'
        super(SimpleLSMDataSet, self).__init__(
            testcase, uri, rows, **kwargs)

    @classmethod
    def is_lsm(cls):
        return True

class SimpleIndexDataSet(SimpleDataSet):
    """
    SimpleIndexDataSet is identical to SimpleDataSet, adding one index
    that maps the value to the key.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        self.indexname = 'index:' + uri.split(":")[1] + ':index1'
        self.origconfig = kwargs.get('config', '')
        kwargs['config'] = self.origconfig + ',columns=(key0,value0)'
        super(SimpleIndexDataSet, self).__init__(
            testcase, uri, rows, **kwargs)

    def create(self):
        super(SimpleIndexDataSet, self).create()
        self.testcase.session.create(self.indexname, 'columns=(value0,key0),' +
            self.origconfig)

    def check(self):
        BaseDataSet.check(self)

        # Check values in the index.
        idxcursor = self.testcase.session.open_cursor(self.indexname)
        for i in range(1, self.rows + 1):
            k = self.key(i)
            v = self.value(i)
            ik = (v, k)  # The index key is columns=(v,k).
            self.testcase.assertEqual(v, idxcursor[ik])
        idxcursor.close()

class SimpleIndexLSMDataSet(SimpleIndexDataSet):
    """
    SimpleIndexLSMDataSet is identical to SimpleIndexDataSet, but
    using LSM files.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        kwargs['config'] = kwargs.get('config', '') + ',type=lsm'
        super(SimpleIndexLSMDataSet, self).__init__(
            testcase, uri, rows, **kwargs)

    @classmethod
    def is_lsm(cls):
        return True

class ComplexDataSet(BaseDataSet):
    """
    ComplexDataSet populates a table with a mixed set of indices
    and column groups.  Some indices are created before the
    table is populated, some after.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        self.indexlist = [
            ['indx1', 'column2'],
            ['indx2', 'column3'],
            ['indx3', 'column4'],
            ['indx4', 'column2,column4'],
            ['indx5', 'column3,column5'],
            ['indx6', 'column3,column5,column4']]
        self.cglist = [
            ['cgroup1', 'column2'],
            ['cgroup2', 'column3'],
            ['cgroup3', 'column4'],
            ['cgroup4', 'column2,column3'],
            ['cgroup5', 'column3,column4'],
            ['cgroup6', 'column2,column4,column5']]
        self.cgconfig = kwargs.pop('cgconfig', '')
        config = kwargs.get('config', '')
        config += ',columns=(record,column2,column3,column4,column5),' + \
                  'colgroups=(cgroup1,cgroup2,cgroup3,cgroup4,cgroup5,cgroup6)'
        kwargs['config'] = config
        kwargs['value_format'] = 'SiSS'
        super(ComplexDataSet, self).__init__(testcase, uri, rows, **kwargs)

    def create(self):
        config = 'key_format=' + self.key_format + \
                 ',value_format=' + self.value_format + ',' + self.config
        session = self.testcase.session
        ##self.testcase.tty('URI=' + self.uri + 'CONFIG=' + config)
        session.create(self.uri, config)
        tablepart = self.uri.split(":")[1] + ':'
        for cg in self.cglist:
            session.create('colgroup:' + tablepart + cg[0],
                           ',columns=(' + cg[1] + '),' + self.cgconfig)
        for index in self.indexlist[0:4]:
            session.create('index:' + tablepart + index[0],
                           ',columns=(' + index[1] + '),' + self.config)

    def postfill_create(self):
        # add some indices after filling the table
        tablepart = self.uri.split(":")[1] + ':'
        session = self.testcase.session
        for index in self.indexlist[4:]:
            session.create('index:' + tablepart + index[0],
                           ',columns=(' + index[1] + ')')

    def colgroup_count(self):
        return len(self.cglist)

    def colgroup_name(self, i):
        return 'colgroup:' + self.uri.split(":")[1] + ':' + self.cglist[i][0]

    def index_count(self):
        return len(self.indexlist)

    def index_name(self, i):
        return 'index:' + self.uri.split(":")[1] + ':' + self.indexlist[i][0]

    # A value suitable for checking the value returned by a cursor, as
    # cursor.get_value() returns a list.
    def comparable_value(self, i):
        # Most of these columns are the keys for indices.  To make sure our indices
        # are ordered in a different order than the main btree, we'll reverse some
        # decimal strings to produce the column values.
        reversed = str(i)[::-1]
        reversed18 = str(i*18)[::-1]
        reversed23 = str(i*23)[::-1]
        return [reversed + ': abcdefghijklmnopqrstuvwxyz'[0:i%26],    # column2
                int(reversed),                                        # column3
                reversed23 + ': abcdefghijklmnopqrstuvwxyz'[0:i%23],  # column4
                reversed18 + ': abcdefghijklmnopqrstuvwxyz'[0:i%18]]  # column5

    # A value suitable for assigning to a cursor, as cursor.set_value() expects
    # a tuple when it is used with a single argument and the value is composite.
    def value(self, i):
        return tuple(self.comparable_value(i))

    def check_cursor(self, cursor):
        i = 0
        for key, s1, i2, s3, s4 in cursor:
            i += 1
            self.testcase.assertEqual(key, self.key(i))
            v = self.value(i)
            self.testcase.assertEqual(s1, v[0])
            self.testcase.assertEqual(i2, v[1])
            self.testcase.assertEqual(s3, v[2])
            self.testcase.assertEqual(s4, v[3])
        self.testcase.assertEqual(i, self.rows)

class ComplexLSMDataSet(ComplexDataSet):
    """
    ComplexLSMDataSet is identical to ComplexDataSet, but using LSM files.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        kwargs['cgconfig'] = kwargs.get('cgconfig', '') + ',type=lsm'
        super(ComplexLSMDataSet, self).__init__(
            testcase, uri, rows, **kwargs)

    @classmethod
    def is_lsm(cls):
        return True

class ProjectionDataSet(SimpleDataSet):
    """
    ProjectionDataSet creates a table with predefined data identical to
    SimpleDataSet (single key and value), but when checking it, uses
    a cursor with a projection.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        kwargs['config'] = kwargs.get('config', '') + ',columns=(k,v0)'
        kwargs['projection'] = '(v0,v0,v0)'
        super(ProjectionDataSet, self).__init__(testcase, uri, rows, **kwargs)

    # A value suitable for checking the value returned by a cursor.
    def comparable_value(self, i):
        v0 = self.value(i)
        return [v0, v0, v0]

    def check_cursor(self, cursor):
        i = 0
        for key, got0, got1, got2 in cursor:
            i += 1
            self.testcase.assertEqual(key, self.key(i))
            if cursor.value_format == '8t' and got0 == 0:    # deleted
                continue
            self.testcase.assertEqual([got0, got1, got2],
                self.comparable_value(i))
        self.testcase.assertEqual(i, self.rows)

class ProjectionIndexDataSet(BaseDataSet):
    """
    ProjectionIndexDataSet creates a table with three values and
    an index.  Checks are made against a projection of the main table
    and a projection of the index.
    """
    def __init__(self, testcase, uri, rows, **kwargs):
        self.origconfig = kwargs.get('config', '')
        self.indexname = 'index:' + uri.split(":")[1] +  ':index0'
        kwargs['config'] = self.origconfig + ',columns=(k,v0,v1,v2)'
        kwargs['value_format'] = kwargs.get('value_format', 'SiS')
        kwargs['projection'] = '(v1,v2,v0)'
        super(ProjectionIndexDataSet, self).__init__(
            testcase, uri, rows, **kwargs)

    def value(self, i):
        return ('v0:' + str(i), i*i, 'v2:' + str(i))

    # Suitable for checking the value returned by a cursor using a projection.
    def comparable_value(self, i):
        return [i*i, 'v2:' + str(i), 'v0:' + str(i)]

    def create(self):
        super(ProjectionIndexDataSet, self).create()
        self.testcase.session.create(self.indexname, 'columns=(v2,v1),' +
            self.origconfig)

    def check_cursor(self, cursor):
        i = 0
        for key, got0, got1, got2 in cursor:
            i += 1
            self.testcase.assertEqual(key, self.key(i))
            if cursor.value_format == '8t' and got0 == 0:    # deleted
                continue
            self.testcase.assertEqual([got0, got1, got2],
                self.comparable_value(i))
        self.testcase.assertEqual(i, self.rows)

    def check_index_cursor(self, cursor):
        for i in range(1, self.rows + 1):
            k = self.key(i)
            v = self.value(i)
            ik = (v[2], v[1])  # The index key is (v2,v2)
            expect = [v[1],k,v[2],v[0]]
            self.testcase.assertEqual(expect, cursor[ik])

    def check(self):
        BaseDataSet.check(self)

        # Check values in the index.
        idxcursor = self.testcase.session.open_cursor(
            self.indexname + '(v1,k,v2,v0)')
        self.check_index_cursor(idxcursor)
        idxcursor.close()

    def index_count(self):
        return 1

    def index_name(self, i):
        return self.indexname

# A data set based on ComplexDataSet that allows large values (depending on a multiplier),
# the ability to update keys with different values, and track the expected value for each key.
class TrackedComplexDataSet(ComplexDataSet):
    alphabet = 'abcdefghijklmnopqrstuvwxyz'

    def __init__(self, testcase, uri, multiplier, **kwargs):
        super(TrackedComplexDataSet, self).__init__(testcase, uri, 0, **kwargs)
        self.multiplier = multiplier
        self.track_values = dict()
        self.refstr = ': ' + self.alphabet * multiplier

    def store_count(self, i):
        try:
            return self.track_values[i]
        except:
            return 0

    # override
    def store_one_cursor(self, c, i):
        self.track_values[i] = self.store_count(i) + 1
        c[self.key(i)] = self.value(i)

    # Redefine the value stored to get bigger depending on the multiplier,
    # and to mix up the value depending on how many times it has been updated.
    #
    # If multiplier is 0, use the basic value used by ComplexDataSet.
    # In this case, since it doesn't rely on the number of stores, updates
    # of the same key will store the same value each time.
    def comparable_value(self, i):
        if self.multiplier == 0:
            return ComplexDataSet.comparable_value(self, i)
        nstores = self.store_count(i)
        m = self.multiplier
        bigi = i * m + nstores
        return [str(i) + self.refstr[0 : bigi % (26*m)],
                i,
                str(i) + self.refstr[0 : bigi % (23*m)],
                str(i) + self.refstr[0 : bigi % (18*m)]]

    def check_cursor(self, cursor):
        expect = dict(self.track_values)
        for key, s1, i2, s3, s4 in cursor:
            i = BaseDataSet.reverse_key_by_format(key, self.key_format)
            v = self.value(i)
            #self.testcase.tty('KEY: {} -> {}'.format(key, i))
            #self.testcase.tty('GOT: {},{},{},{}'.format(s1, i2, s3, s4))
            #self.testcase.tty('EXPECT: {}'.format(v))
            self.testcase.assertEqual(s1, v[0])
            self.testcase.assertEqual(i2, v[1])
            self.testcase.assertEqual(s3, v[2])
            self.testcase.assertEqual(s4, v[3])
            self.testcase.assertTrue(i in expect)
            del expect[i]
        self.testcase.assertEqual(len(expect), 0)

# A data set based on SimpleDataSet that allows large values (depending on a multiplier),
# the ability to update keys with different values, and track the expected value for each key.
class TrackedSimpleDataSet(SimpleDataSet):
    alphabet = 'abcdefghijklmnopqrstuvwxyz'

    def __init__(self, testcase, uri, multiplier, **kwargs):
        super(TrackedSimpleDataSet, self).__init__(testcase, uri, 0, **kwargs)
        self.multiplier = multiplier
        self.track_values = dict()
        self.refstr = ': ' + self.alphabet * multiplier

    def store_count(self, i):
        try:
            return self.track_values[i]
        except:
            return 0

    # override
    def store_one_cursor(self, c, i):
        self.track_values[i] = self.store_count(i) + 1
        c[self.key(i)] = self.value(i)

    # Redefine the value stored to get bigger depending on the multiplier,
    # and to mix up the value depending on how many times it has been updated.
    #
    # If multiplier is 0, use the basic value used by SimpleDataSet.
    # In this case, since it doesn't rely on the number of stores, updates
    # of the same key will store the same value each time.
    def comparable_value(self, i):
        if self.multiplier == 0:
            return SimpleDataSet.comparable_value(self, i)
        nstores = self.store_count(i)
        m = self.multiplier
        bigi = i * m + nstores
        return str(i) + self.refstr[0 : bigi % (26*m)]

    def value(self, i):
        return self.comparable_value(i)

    def check_cursor(self, cursor):
        expect = dict(self.track_values)
        for key, s in cursor:
            i = BaseDataSet.reverse_key_by_format(key, self.key_format)
            v = self.value(i)
            #self.testcase.tty('KEY: {} -> {}'.format(key, i))
            #self.testcase.tty('GOT: {}'.format(s))
            #self.testcase.tty('EXPECT: {}'.format(v))
            self.testcase.assertEqual(s, v)
            del expect[i]
        self.testcase.assertEqual(len(expect), 0)

# create a key based on a cursor as a shortcut to creating a SimpleDataSet
def simple_key(cursor, i):
    return BaseDataSet.key_by_format(i, cursor.key_format)

# create a value based on a cursor as a shortcut to creating a SimpleDataSet
def simple_value(cursor, i):
    return BaseDataSet.value_by_format(i, cursor.value_format)

# create a key based on a cursor as a shortcut to creating a ComplexDataSet
def complex_key(cursor, i):
    return BaseDataSet.key_by_format(i, cursor.key_format)
