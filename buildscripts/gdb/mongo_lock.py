from __future__ import print_function

import gdb
import gdb.printing
import re
import sys

if sys.version_info[0] >= 3:
    # GDB only permits converting a gdb.Value instance to its numerical address when using the
    # long() constructor in Python 2 and not when using the int() constructor. We define the
    # 'long' class as an alias for the 'int' class in Python 3 for compatibility.
    long = int


class Thread(object):
    def __init__(self, thread_id, lwpid):
        self.thread_id = thread_id
        self.lwpid = lwpid

    def __eq__(self, other):
        if isinstance(other, Thread):
            return self.thread_id == other.thread_id
        return NotImplemented

    def __ne__(self, other):
        return not self == other

    def __str__(self):
        return "Thread 0x{:012x} (LWP {})".format(self.thread_id, self.lwpid)

    def key(self):
        return "Thread 0x{:012x}".format(self.thread_id)


class Lock(object):
    def __init__(self, addr, resource):
        self.addr = addr
        self.resource = resource

    def __eq__(self, other):
        if isinstance(other, Lock):
            return self.addr == other.addr
        return NotImplemented

    def __ne__(self, other):
        return not self == other

    def __str__(self):
        return "Lock 0x{:012x} ({})".format(self.addr, self.resource)

    def key(self):
        return "Lock 0x{:012x}".format(self.addr)


class Graph(object):
    # The Graph is a dict with the following structure:
    #   {'node_key': {'node': {id: val}, 'next_nodes': [node_key_1, ...]}}
    # Example graph:
    #   {
    #    'Lock 1': {'node': {1: 'MongoDB lock'}, 'next_nodes': ['Thread 1']},
    #    'Lock 2': {'node': {1: 'MongoDB lock'}, 'next_nodes': ['Thread 2']},
    #    'Thread 1': {'node': {1: 123}, 'next_nodes': ['Lock 2']},
    #    'Thread 2': {'node': {2: 456}, 'next_nodes': ['Lock 1']}
    #   }
    def __init__(self):
        self.nodes = {}

    def is_empty(self):
        return not bool(self.nodes)

    def add_node(self, node):
        if not self.find_node(node):
            self.nodes[node.key()] = {'node': node, 'next_nodes': []}

    def find_node(self, node):
        if node.key() in self.nodes:
            return self.nodes[node.key()]
        return None

    def find_from_node(self, from_node):
        for node_key in self.nodes:
            node = self.nodes[node_key]
            for next_node in node['next_nodes']:
                if next_node == from_node['node'].key():
                    return node
        return None

    def remove_nodes_without_edge(self):
        # Rebuild graph by removing any nodes which do not have any incoming or outgoing any edges.
        temp_nodes = {}
        for node_key in self.nodes:
            node = self.nodes[node_key]
            if node['next_nodes'] or self.find_from_node(node) is not None:
                temp_nodes[node_key] = self.nodes[node_key]
        self.nodes = temp_nodes

    def add_edge(self, from_node, to_node):
        f = self.find_node(from_node)
        if f is None:
            self.add_node(from_node)
            f = self.nodes[from_node.key()]

        t = self.find_node(to_node)
        if t is None:
            self.add_node(to_node)
            t = self.nodes[to_node.key()]

        for n in f['next_nodes']:
            if n == to_node.key():
                return
        self.nodes[from_node.key()]['next_nodes'].append(to_node.key())

    def print(self):
        for node_key in self.nodes:
            print("Node", self.nodes[node_key]['node'])
            for to in self.nodes[node_key]['next_nodes']:
                print(" ->", to)

    def to_graph(self):
        sb = []
        sb.append('# Legend:')
        sb.append('#    Thread 1 -> Lock 1 indicates Thread 1 is waiting on Lock 1')
        sb.append('#    Lock 2 -> Thread 2 indicates Lock 2 is held by Thread 2')
        sb.append('digraph "mongod+lock-status" {')
        for node_key in self.nodes:
            for next_node_key in self.nodes[node_key]['next_nodes']:
                sb.append('    "%s" -> "%s";' % (node_key, next_node_key))
        for node_key in self.nodes:
            sb.append('    "%s" [label="%s"]' % (node_key, self.nodes[node_key]['node']))
        sb.append("}")
        return "\n".join(sb)


def find_lwpid(thread_dict, search_thread_id):
    for (lwpid, thread_id) in thread_dict.items():
        if thread_id == search_thread_id:
            return lwpid
    return None


def find_func_block(block):
    while block:
        if block.function:
            return block
        block = block.superblock
    return None


def find_frame(function_name_pattern):
    frame = gdb.newest_frame()
    while frame:
        block = None
        try:
            block = frame.block()
        except RuntimeError as err:
            if err.args[0] != "Cannot locate block for frame.":
                raise

        block = find_func_block(block)
        if block and re.match(function_name_pattern, block.function.name):
            return frame
        try:
            frame = frame.older()
        except gdb.error as err:
            print("Ignoring GDB error '%s' in find_frame" % str(err))
            break
    return None


def find_mutex_holder(graph, thread_dict, show):
    frame = find_frame(r'std::mutex::lock\(\)')
    if frame is None:
        return

    frame.select()

    # Waiting for mutex locking!
    mutex_this, _ = gdb.lookup_symbol("this", frame.block())
    mutex_value = mutex_this.value(frame)
    # The mutex holder is a LWPID
    mutex_holder = int(mutex_value["_M_mutex"]["__data"]["__owner"])
    mutex_holder_id = thread_dict[mutex_holder]

    (_, mutex_waiter_lwpid, _) = gdb.selected_thread().ptid
    mutex_waiter_id = thread_dict[mutex_waiter_lwpid]
    if show:
        print("Mutex at {} held by thread 0x{:x} (LWP {}) "
              " waited on by thread 0x{:x} (LWP {})".format(mutex_value,
                                                            mutex_holder_id,
                                                            mutex_holder,
                                                            mutex_waiter_id,
                                                            mutex_waiter_lwpid))
    if graph:
        graph.add_edge(Thread(mutex_waiter_id, mutex_waiter_lwpid),
                       Lock(long(mutex_value), "Mutex"))
        graph.add_edge(Lock(long(mutex_value), "Mutex"), Thread(mutex_holder_id, mutex_holder))


def find_lock_manager_holders(graph, thread_dict, show):
    frame = find_frame(r'mongo::LockerImpl\<.*\>::')
    if not frame:
        return

    frame.select()

    (_, lwpid, _) = gdb.selected_thread().ptid

    locker_ptr_type = gdb.lookup_type("mongo::LockerImpl<false>").pointer()
    lock_head = gdb.parse_and_eval(
        "mongo::getGlobalLockManager()->_getBucket(resId)->findOrInsert(resId)")

    grantedList = lock_head.dereference()["grantedList"]
    lock_request_ptr = grantedList["_front"]
    while lock_request_ptr:
        lock_request = lock_request_ptr.dereference()
        locker_ptr = lock_request["locker"]
        locker_ptr = locker_ptr.cast(locker_ptr_type)
        locker = locker_ptr.dereference()
        lock_thread_id = int(locker["_threadId"]["_M_thread"])
        lock_thread_lwpid = find_lwpid(thread_dict, lock_thread_id)
        if show:
            print("MongoDB Lock at {} ({}) held by thread id 0x{:x} (LWP {})".format(
                lock_head, lock_request["mode"], lock_thread_id, lock_thread_lwpid) +
                " waited on by thread 0x{:x} (LWP {})".format(thread_dict[lwpid], lwpid))
        if graph:
            graph.add_edge(Thread(thread_dict[lwpid], lwpid), Lock(long(lock_head), "MongoDB lock"))
            graph.add_edge(Lock(long(lock_head), "MongoDB lock"),
                           Thread(lock_thread_id, lock_thread_lwpid))
        lock_request_ptr = lock_request["next"]


def get_locks(graph, thread_dict, show=False):
    for thread in gdb.selected_inferior().threads():
        try:
            if not thread.is_valid():
                continue
            thread.switch()
            find_mutex_holder(graph, thread_dict, show)
            find_lock_manager_holders(graph, thread_dict, show)
        except gdb.error as err:
            print("Ignoring GDB error '%s' in get_locks" % str(err))


def get_threads_info(graph=None):
    thread_dict = {}
    for thread in gdb.selected_inferior().threads():
        try:
            if not thread.is_valid():
                continue
            thread.switch()
            # PTID is a tuple: Process ID (PID), Lightweight Process ID (LWPID), Thread ID (TID)
            (_, lwpid, _) = thread.ptid
            thread_num = thread.num
            thread_id = get_thread_id()
            if not thread_id:
                print("Unable to retrieve thread_info for thread %d" % thread_num)
                continue
            thread_dict[lwpid] = thread_id
        except gdb.error as err:
            print("Ignoring GDB error '%s' in get_threads_info" % str(err))

    return thread_dict


class MongoDBShowLocks(gdb.Command):
    """Show MongoDB locks & pthread mutexes"""
    def __init__(self):
        register_mongo_command(self, "mongodb-show-locks", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        self.mongodb_show_locks()

    def mongodb_show_locks(self):
        """GDB in-process python supplement"""
        try:
            thread_dict = get_threads_info()
            get_locks(graph=None, thread_dict=thread_dict, show=True)
        except gdb.error as err:
            print("Ignoring GDB error '%s' in mongodb_show_locks" % str(err))

MongoDBShowLocks()


class MongoDBWaitsForGraph(gdb.Command):
    """Create MongoDB WaitsFor lock graph [graph_file]"""
    def __init__(self):
        register_mongo_command(self, "mongodb-waitsfor-graph", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        self.mongodb_waitsfor_graph(arg)

    def mongodb_waitsfor_graph(self, file=None):
        """GDB in-process python supplement"""

        graph = Graph()
        try:
            thread_dict = get_threads_info(graph=graph)
            get_locks(graph=graph, thread_dict=thread_dict, show=False)
            graph.remove_nodes_without_edge()
            if graph.is_empty():
                print("Not generating the digraph, since the lock graph is empty")
                return
            if file:
                print("Saving digraph to %s" % file)
                with open(file, 'w') as f:
                    f.write(graph.to_graph())
            else:
                print(graph.to_graph())

        except gdb.error as err:
            print("Ignoring GDB error '%s' in mongod_deadlock_graph" % str(err))


MongoDBWaitsForGraph()

print("MongoDB Lock analysis commands loaded")
