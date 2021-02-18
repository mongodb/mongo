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

key_config=[
    Config('key_size', 0, r'''
        The size of the keys to be created''', min=0, max=10000),
]

value_config = [
    Config('value_size', 0, r'''
        The size of the values to be created''', min=0, max=10000),
]

scale_config = [
    Config('collection_count', 1, r'''
        The number of colections the workload generator operates over''', min=0, max=200000),
    Config('key_count', 0, r'''
        The number of keys to be operated on per colection''', min=0, max=1000000),
]

load_config = key_config + value_config + scale_config

workload_config = [
    Config('read_threads', 0, r'''
        The number of threads performing read operations''', min=0, max=100),
    Config('insert_threads', 0, r'''
        The number of threads performing insert operations''',min=0, max=20),
    Config('insert_config',0, r'''
        The definition of the record being inserted''',
        subconfig=load_config),
    Config('update_threads', 0, r'''
        The number of threads performing update operations''',min=0, max=20),
    Config('update_config',0,r''',
        The definition of the record being updated''', subconfig=load_config)
]

methods = {
'poc_test' : Method(load_config + workload_config),
}
