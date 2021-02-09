# This file is a python script that describes the cpp test framework test configuration options.

class Method:
    def __init__(self, config):
        # Deal with duplicates: with complex configurations (like
        # WT_SESSION::create), it's simpler to deal with duplicates once than
        # manually as configurations are defined
        self.config = []
        lastname = None
        for c in sorted(config):
            if '.' in c.name:
                raise "Bad config key '%s'" % c.name
            if c.name == lastname:
                continue
            lastname = c.name
            self.config.append(c)

class Config:
    def __init__(self, name, default, desc, subconfig=None, **flags):
        self.name = name
        self.default = default
        self.desc = desc
        self.subconfig = subconfig
        self.flags = flags

    # Comparators for sorting.
    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __lt__(self, other):
        return self.name < other.name

    def __le__(self, other):
        return self.name <= other.name

    def __gt__(self, other):
        return self.name > other.name

    def __ge__(self, other):
        return self.name >= other.name
methods = {
'poc_test' : Method([
    Config('collection_count', '1', r'''
        the number of collections to create for testing''',
        min='1', max='10'),
    Config('key_size', '10', r'''
        the size of the keys to be created in bytes''',
        min='1', max='10000'),
    Config('values', 'first', r'''
        The value that each key will be populated with, used an example string configuration''',
        choices=['first', 'second', 'third'])
]),
}
