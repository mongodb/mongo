#!/usr/bin/env python
#
# Copyright (c) 2008-2013 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# WiredTiger Python RPC server for testing and tutorials.

import sys

from wiredtiger.service import WiredTiger
from wiredtiger.service.ttypes import *

from wiredtiger.impl import *

from thrift.transport import TSocket
from thrift.transport import TTransport
from thrift.protocol import TBinaryProtocol
from thrift.server import TServer

class WiredTigerHandler:

handler = WiredTigerHandler()
processor = WiredTiger.Processor(handler)
transport = TSocket.TServerSocket(9090)
tfactory = TTransport.TBufferedTransportFactory()
pfactory = TBinaryProtocol.TBinaryProtocolFactory()

server = TServer.TSimpleServer(processor, transport, tfactory, pfactory)

# We could do one of these for a multithreaded server
#server = TServer.TThreadedServer(processor, transport, tfactory, pfactory)
#server = TServer.TThreadPoolServer(processor, transport, tfactory, pfactory)

print 'Starting the server...'
server.serve()
print 'done.'
