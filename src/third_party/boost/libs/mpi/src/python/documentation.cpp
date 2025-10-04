// (C) Copyright 2005 The Trustees of Indiana University.
// (C) Copyright 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor

/** @file documentation.cpp
 *
 *  This file contains all of the documentation strings for the
 *  Boost.MPI Python bindings.
 */
namespace boost { namespace mpi { namespace python {

const char* module_docstring = 
  "The boost.mpi module contains Python wrappers for Boost.MPI.\n"
  "Boost.MPI is a C++ interface to the Message Passing Interface 1.1,\n"
  "a high-performance message passing library for parallel programming.\n"
  "\n"
  "This module supports the most commonly used subset of MPI 1.1. All\n"
  "communication operations can transmit any Python object that can be\n"
  "pickled and unpickled, along with C++-serialized data types and\n"
  "separation of the structure of a data type from its content.\n"
  "Collectives that have a user-supplied functions,\n"
  "such as reduce() or scan(), accept arbitrary Python functions, and\n"
  "all collectives can operate on any serializable or picklable data type.\n"
  "\n"
  "IMPORTANT MODULE DATA\n"
  "  any_source       This constant may be used for the source parameter of\n"
  "                   receive and probe operations to indicate that a\n"
  "                   message may be received from any source.\n"
  "\n"
  "  any_tag          This constant may be used for the tag parameter of\n"
  "                   receive or probe operations to indicate that a send\n"
  "                   with any tag will be matched.\n"
  "\n"
  "  collectives_tag  Returns the reserved tag value used by the Boost.MPI\n"
  "                   implementation for collective operations. Although\n"
  "                   users are not permitted to use this tag to send or\n"
  "                   receive messages with this tag, it may be useful when\n"
  "                   monitoring communication patterns.\n"
  "\n"
  "  host_rank        If there is a host process, this is the rank of that\n"
  "                   that process. Otherwise, this value will be None. MPI\n"
  "                   does not define the meaning of a \"host\" process: \n"
  "                   consult the documentation for your MPI implementation.\n"
  "\n"
  "  io_rank          The rank of a process that can perform input/output\n"
  "                   via the standard facilities. If every process can\n"
  "                   perform I/O using the standard facilities, this value\n"
  "                   will be the same as any_source. If no process can\n"
  "                   perform I/O, this value will be None.\n"
  "\n"
  "  max_tag          The maximum value that may be used for the tag\n"
  "                   parameter of send/receive operations. This value will\n"
  "                   be somewhat smaller than the value of MPI_TAG_UB,\n"
  "                   because the Boost.MPI implementation reserves some\n"
  "                   tags for collective operations.\n"
  "\n"
  "  processor_name   The name of this processor. The actual form of the\n"
  "                   of the name is unspecified, but may be documented by\n"
  "                   the underlying MPI implementation.\n"
  "\n"
  "  rank             The rank of this process in the \"world\" communicator.\n"
  "\n"
  "  size             The number of processes in the \"world\" communicator.\n"
  "                   that process. Otherwise, this value will be None. MPI\n"
  "                   does not define the meaning of a \"host\" process: \n"
  "\n"
  "  world            The \"world\" communicator from which all other\n"
  "                   communicators will be derived. This is the equivalent\n"
  "                   of MPI_COMM_WORLD.\n"
  "\n"
  "TRANSMITTING USER-DEFINED DATA\n"
  "  Boost.MPI can transmit user-defined data in several different ways.\n"
  "  Most importantly, it can transmit arbitrary Python objects by pickling\n"
  "  them at the sender and unpickling them at the receiver, allowing\n"
  "  arbitrarily complex Python data structures to interoperate with MPI.\n"
  "\n"
  "  Boost.MPI also supports efficient serialization and transmission of\n"
  "  C++ objects (that have been exposed to Python) through its C++\n"
  "  interface. Any C++ type that provides (de-)serialization routines that\n"
  "  meet the requirements of the Boost.Serialization library is eligible\n"
  "  for this optimization, but the type must be registered in advance. To\n"
  "  register a C++ type, invoke the C++ function:\n"
  "    boost::mpi::python::register_serialized\n"
  "\n"
  "  Finally, Boost.MPI supports separation of the structure of an object\n"
  "  from the data it stores, allowing the two pieces to be transmitted\n"
  "  separately. This \"skeleton/content\" mechanism, described in more\n"
  "  detail in a later section, is a communication optimization suitable\n"
  "  for problems with fixed data structures whose internal data changes\n"
  "  frequently.\n"
  "\n"
  "COLLECTIVES\n"
  "  Boost.MPI supports all of the MPI collectives (scatter, reduce, scan,\n"
  "  broadcast, etc.) for any type of data that can be transmitted with the\n"
  "  point-to-point communication operations. For the MPI collectives that\n"
  "  require a user-specified operation (e.g., reduce and scan), the\n"
  "  operation can be an arbitrary Python function. For instance, one could\n"
  "  concatenate strings with all_reduce:\n\n"
  "    mpi.all_reduce(my_string, lambda x,y: x + y)\n\n"
  "  The following module-level functions implement MPI collectives:\n"
  "    all_gather    Gather the values from all processes.\n"
  "    all_reduce    Combine the results from all processes.\n"
  "    all_to_all    Every process sends data to every other process.\n"
  "    broadcast     Broadcast data from one process to all other processes.\n"
  "    gather        Gather the values from all processes to the root.\n"
  "    reduce        Combine the results from all processes to the root.\n"
  "    scan          Prefix reduction of the values from all processes.\n"
  "    scatter       Scatter the values stored at the root to all processes.\n"
  "\n"
  "SKELETON/CONTENT MECHANISM\n"
  "  Boost.MPI provides a skeleton/content mechanism that allows the\n"
  "  transfer of large data structures to be split into two separate stages,\n"
  "  with the `skeleton' (or, `shape') of the data structure sent first and\n"
  "  the content (or, `data') of the data structure sent later, potentially\n"
  "  several times, so long as the structure has not changed since the\n"
  "  skeleton was transferred. The skeleton/content mechanism can improve\n"
  "  performance when the data structure is large and its shape is fixed,\n"
  "  because while the skeleton requires serialization (it has an unknown\n"
  "  size), the content transfer is fixed-size and can be done without\n"
  "  extra copies.\n"
  "\n"
  "  To use the skeleton/content mechanism from Python, you must first\n"
  "  register the type of your data structure with the skeleton/content\n"
  "  mechanism *from C++*. The registration function is\n"
  "    boost::mpi::python::register_skeleton_and_content\n"
  "  and resides in the <boost/mpi/python.hpp> header.\n"
  "\n"
  "  Once you have registered your C++ data structures, you can extract\n"
  "  the skeleton for an instance of that data structure with skeleton().\n"
  "  The resulting SkeletonProxy can be transmitted via the normal send\n"
  "  routine, e.g.,\n\n"
  "    mpi.world.send(1, 0, skeleton(my_data_structure))\n\n"
  "  SkeletonProxy objects can be received on the other end via recv(),\n"
  "  which stores a newly-created instance of your data structure with the\n"
  "  same `shape' as the sender in its `object' attribute:\n\n"
  "    shape = mpi.world.recv(0, 0)\n"
  "    my_data_structure = shape.object\n\n"
  "  Once the skeleton has been transmitted, the content (accessed via \n"
  "  get_content) can be transmitted in much the same way. Note, however,\n"
  "  that the receiver also specifies get_content(my_data_structure) in its\n"
  "  call to receive:\n\n"
  "    if mpi.rank == 0:\n"
  "      mpi.world.send(1, 0, get_content(my_data_structure))\n"
  "    else:\n"
  "      mpi.world.recv(0, 0, get_content(my_data_structure))\n\n"
  "  Of course, this transmission of content can occur repeatedly, if the\n"
  "  values in the data structure--but not its shape--changes.\n"
  "\n"
  "  The skeleton/content mechanism is a structured way to exploit the\n"
  "  interaction between custom-built MPI datatypes and MPI_BOTTOM, to\n"
  "  eliminate extra buffer copies.\n"
  "\n"
  "C++/PYTHON MPI COMPATIBILITY\n"
  "  Boost.MPI is a C++ library whose facilities have been exposed to Python\n"
  "  via the Boost.Python library. Since the Boost.MPI Python bindings are\n"
  "  build directly on top of the C++ library, and nearly every feature of\n"
  "  C++ library is available in Python, hybrid C++/Python programs using\n"
  "  Boost.MPI can interact, e.g., sending a value from Python but receiving\n"
  "  that value in C++ (or vice versa). However, doing so requires some\n"
  "  care. Because Python objects are dynamically typed, Boost.MPI transfers\n"
  "  type information along with the serialized form of the object, so that\n"
  "  the object can be received even when its type is not known. This\n"
  "  mechanism differs from its C++ counterpart, where the static types of\n"
  "  transmitted values are always known.\n"
  "\n"
  "  The only way to communicate between the C++ and Python views on \n"
  "  Boost.MPI is to traffic entirely in Python objects. For Python, this is\n"
  "  the normal state of affairs, so nothing will change. For C++, this\n"
  "  means sending and receiving values of type boost::python::object, from\n"
  "  the Boost.Python library. For instance, say we want to transmit an\n"
  "  integer value from Python:\n\n"
  "    comm.send(1, 0, 17)\n\n"
  "  In C++, we would receive that value into a Python object and then\n"
  "  `extract' an integer value:\n\n"
  "    boost::python::object value;\n"
  "    comm.recv(0, 0, value);\n"
  "    int int_value = boost::python::extract<int>(value);\n\n"
  "  In the future, Boost.MPI will be extended to allow improved\n"
  "  interoperability with the C++ Boost.MPI and the C MPI bindings.\n"
  ;

/***********************************************************
 * environment documentation                               *
 ***********************************************************/
const char* environment_init_docstring = 
  "Initialize the MPI environment. Users should not need to call\n"
  "this function directly, because the MPI environment will be\n"
  "automatically initialized when the Boost.MPI module is loaded.\n";

const char* environment_finalize_docstring = 
  "Finalize (shut down) the MPI environment. Users only need to\n"
  "invoke this function if MPI should be shut down before program\n"
  "termination. Boost.MPI will automatically finalize the MPI\n"
  "environment when the program exits.\n";
 
const char* environment_abort_docstring = 
  "Aborts all MPI processes and returns to the environment. The\n"
  "precise behavior will be defined by the underlying MPI\n"
  "implementation. This is equivalent to a call to MPI_Abort with\n"
  "MPI_COMM_WORLD.\n"
  "errcode is the error code to return from aborted processes.\n";

const char* environment_initialized_docstring = 
 "Determine if the MPI environment has already been initialized.\n";

const char* environment_finalized_docstring = 
  "Determine if the MPI environment has already been finalized.\n";

/***********************************************************
 * nonblocking documentation                               *
 ***********************************************************/
const char* request_list_init_docstring=
  "Without arguments, constructs an empty RequestList.\n"
  "With one argument `iterable', copies request objects from this\n"
  "iterable to the new RequestList.\n";

const char* nonblocking_wait_any_docstring =
  "Waits until any of the given requests has been completed. It provides\n"
  "functionality equivalent to MPI_Waitany.\n"
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "Returns a triple (value, status, index) consisting of received value\n"
  "(or None), the Status object for the completed request, and its index\n"
  "in the RequestList.\n";

const char* nonblocking_test_any_docstring =
  "Tests if any of the given requests have been completed, but does not wait\n"
  "for completion. It provides functionality equivalent to MPI_Testany.\n"
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "Returns a triple (value, status, index) like wait_any or None if no request\n"
  "is complete.\n";

const char* nonblocking_wait_all_docstring =
  "Waits until all of the given requests have been completed. It provides\n"
  "functionality equivalent to MPI_Waitall.\n"
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "If the second parameter `callable' is provided, it is called with each\n"
  "completed request's received value (or None) and it s Status object as\n"
  "its arguments. The calls occur in the order given by the `requests' list.\n";

const char* nonblocking_test_all_docstring =
  "Tests if all of the given requests have been completed. It provides\n"
  "functionality equivalent to MPI_Testall.\n"
  "\n"
  "Returns True if all requests have been completed.\n"
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "If the second parameter `callable' is provided, it is called with each\n"
  "completed request's received value (or None) and it s Status object as\n"
  "its arguments. The calls occur in the order given by the `requests' list.\n";

const char* nonblocking_wait_some_docstring =
  "Waits until at least one of the given requests has completed. It\n"
  "then completes all of the requests it can, partitioning the input\n"
  "sequence into pending requests followed by completed requests.\n"
  "\n"
  "This routine provides functionality equivalent to MPI_Waitsome.\n"
  "\n"
  "Returns the index of the first completed request."
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "If the second parameter `callable' is provided, it is called with each\n"
  "completed request's received value (or None) and it s Status object as\n"
  "its arguments. The calls occur in the order given by the `requests' list.\n";

const char* nonblocking_test_some_docstring =
  "Tests to see if any of the given requests has completed. It completes\n"
  "all of the requests it can, partitioning the input sequence into pending\n"
  "requests followed by completed requests. This routine is similar to\n"
  "wait_some, but does not wait until any requests have completed.\n"
  "\n"
  "This routine provides functionality equivalent to MPI_Testsome.\n"
  "\n"
  "Returns the index of the first completed request."
  "\n"
  "requests must be a RequestList instance.\n"
  "\n"
  "If the second parameter `callable' is provided, it is called with each\n"
  "completed request's received value (or None) and it s Status object as\n"
  "its arguments. The calls occur in the order given by the `requests' list.\n";

/***********************************************************
 * exception documentation                                 *
 ***********************************************************/
const char* exception_docstring = 
  "Instances of this class will be thrown when an MPI error\n"
  "occurs. MPI failures that trigger these exceptions may or may not\n"
  "be recoverable, depending on the underlying MPI implementation.\n"
  "Consult the documentation for your MPI implementation to determine\n"
  "the effect of MPI errors.\n";

const char* exception_what_docstring = 
  "A description of the error that occured. At present, this refers\n"
  "only to the name of the MPI routine that failed.\n";

const char* exception_routine_docstring = 
  "The name of the MPI routine that reported the error.\n";

const char* exception_result_code_docstring = 
  "The result code returned from the MPI routine that reported the\n"
  "error.\n";

/***********************************************************
 * collectives documentation                               *
 ***********************************************************/
const char* all_gather_docstring =
  "all_gather is a collective algorithm that collects the values\n"
  "stored at each process into a tuple of values indexed by the\n"
  "process number they came from. all_gather is (semantically) a\n" 
  "gather followed by a broadcast. The same tuple of values is\n"
  "returned to all processes.\n";

const char* all_reduce_docstring =
  "all_reduce is a collective algorithm that combines the values\n"
  "stored by each process into a single value. The values can be\n"
  "combined arbitrarily, specified via any function. The values\n"
  "a1, a2, .., ap provided by p processors will be combined by the\n"
  "binary function op into the result\n"
  "         op(a1, op(a2, ... op(ap-1,ap)))\n"
  "that will be returned to all processes. This function is the\n"
  "equivalent of calling all_gather() and then applying the built-in\n"
  "reduce() function to the returned sequence. op is assumed to be\n"
  "associative.\n";

const char* all_to_all_docstring =
  "all_to_all is a collective algorithm that transmits values from\n"
  "every process to every other process. On process i, the jth value\n"
  "of the values sequence is sent to process j and placed in the ith\n"
  "position of the tuple that will be returned from all_to_all.\n";

const char* broadcast_docstring =
  "broadcast is a collective algorithm that transfers a value from an\n"
  "arbitrary root process to every other process that is part of the\n"
  "given communicator (comm). The root parameter must be the same for\n"
  "every process. The value parameter need only be specified at the root\n"
  "root. broadcast() returns the same broadcasted value to every process.\n";

const char* gather_docstring =
  "gather is a collective algorithm that collects the values\n"
  "stored at each process into a tuple of values at the root\n"
  "process. This tuple is indexed by the process number that the\n"
  "value came from, and will be returned only by the root process.\n"
  "All other processes return None.\n";

const char* reduce_docstring =
  "reduce is a collective algorithm that combines the values\n"
  "stored by each process into a single value at the root. The\n"
  "values can be combined arbitrarily, specified via any function.\n"
  "The values a1, a2, .., ap provided by p processors will be\n"
  "combined by the binary function op into the result\n"
  "     op(a1, op(a2, ... op(ap-1,ap)))\n"
  "that will be returned on the root process. This function is the\n"
  "equivalent of calling gather() to the root and then applying the\n"
  "built-in reduce() function to the returned sequence. All non-root\n"
  "processes return None. op is assumed to be associative.\n";

const char* scan_docstring =
  "@c scan computes a prefix reduction of values from all processes.\n"
  "It is a collective algorithm that combines the values stored by\n"
  "each process with the values of all processes with a smaller rank.\n"
  "The values can be arbitrarily combined, specified via a binary\n"
  "function op. If each process i provides the value ai, then scan\n"
  "returns op(a1, op(a2, ... op(ai-1, ai))) to the ith process. op is\n"
  "assumed to be associative. This routine is the equivalent of an\n"
  "all_gather(), followed by a built-in reduce() on the first i+1\n"
  "values in the resulting sequence on processor i. op is assumed\n"
  "to be associative.\n";

const char* scatter_docstring =
  "scatter is a collective algorithm that scatters the values stored\n"
  "in the root process (as a container with comm.size elements) to\n"
  "all of the processes in the communicator. The values parameter \n"
  "(only significant at the root) is indexed by the process number to\n"
  "which the corresponding value will be sent. The value received by \n"
  "each process is returned from scatter.\n";

/***********************************************************
 * communicator documentation                              *
 ***********************************************************/
const char* communicator_docstring =
 "The Communicator class abstracts a set of communicating\n"
 "processes in MPI. All of the processes that belong to a certain\n"
 "communicator can determine the size of the communicator, their rank\n"
 "within the communicator, and communicate with any other processes\n"
 "in the communicator.\n";

const char* communicator_default_constructor_docstring =
  "Build a new Boost.MPI Communicator instance for MPI_COMM_WORLD.\n";

const char* communicator_rank_docstring =
  "Returns the rank of the process in the communicator, which will be a\n"
  "value in [0, size).\n";

const char* communicator_size_docstring =
  "Returns the number of processes in the communicator.\n";

const char* communicator_send_docstring =
  "This routine executes a potentially blocking send with the given\n"
  "tag to the process with rank dest. It can be received by the\n"
  "destination process with a matching recv call. The value will be\n"
  "transmitted in one of several ways:\n"
  "\n"
  "  - For C++ objects registered via register_serialized(), the value\n"
  "    will be serialized and transmitted.\n"
  "\n"
  "  - For SkeletonProxy objects, the skeleton of the object will be\n"
  "    serialized and transmitted.\n"
  "\n"
  "  - For Content objects, the content will be transmitted directly.\n"
  "    This content can be received by a matching recv/irecv call that\n"
  "    provides a suitable `buffer' argument.\n"
  "\n"
  "  - For all other Python objects, the value will be pickled and\n"
  "    transmitted.\n";

const char* communicator_recv_docstring =
  "This routine blocks until it receives a message from the process\n"
  "source with the given tag. If the source parameter is not specified,\n"
  "the message can be received from any process. Likewise, if the tag\n"
  "parameter is not specified, a message with any tag can be received.\n"
  "If return_status is True, returns a tuple containing the received\n"
  "object followed by a Status object describing the communication.\n"
  "Otherwise, recv() returns just the received object.\n"
  "\n"
  "When receiving the content of a data type that has been sent separately\n"
  "from its skeleton, user code must provide a value for the `buffer'\n"
  "argument. This value should be the Content object returned from\n"
  "get_content().\n";

const char* communicator_isend_docstring =
  "This routine executes a nonblocking send with the given\n"
  "tag to the process with rank dest. It can be received by the\n"
  "destination process with a matching recv call. The value will be\n"
  "transmitted in the same way as with send().\n"
  "This routine returns a Request object, which can be used to query\n"
  "when the transmission has completed, wait for its completion, or\n"
  "cancel the transmission.\n";

const char* communicator_irecv_docstring =
  "This routine initiates a non-blocking receive from the process\n"
  "source with the given tag. If the source parameter is not specified,\n"
  "the message can be received from any process. Likewise, if the tag\n"
  "parameter is not specified, a message with any tag can be received.\n"
  "This routine returns a Request object, which can be used to query\n"
  "when the transmission has completed, wait for its completion, or\n"
  "cancel the transmission. The received value be accessible\n"
  "through the `value' attribute of the Request object once transmission\n"
  "has completed.\n"
  "\n"
  "As with the recv() routine, when receiving the content of a data type\n"
  "that has been sent separately from its skeleton, user code must provide\n"
  "a value for the `buffer' argument. This value should be the Content\n"
  "object returned from get_content().\n"; 

 const char* communicator_probe_docstring =
  "This operation waits until a message matching (source, tag)\n"
  "is available to be received. It then returns information about\n"
  "that message. If source is omitted, a message from any process\n"
  "will match. If tag is omitted, a message with any tag will match.\n"
  "The actual source and tag can be retrieved from the returned Status\n"
  "object. To check if a message is available without blocking, use\n"
  "iprobe.\n";

const char* communicator_iprobe_docstring =
  "This operation determines if a message matching (source, tag) is\n"
  "available to be received. If so, it returns information about that\n"
  "message; otherwise, it returns None. If source is omitted, a message\n"
  "from any process will match. If tag is omitted, a message with any\n"
  "tag will match. The actual source and tag can be retrieved from the\n"
  "returned Status object. To wait for a message to become available, use\n"
  "probe.\n";

const char* communicator_barrier_docstring = 
  "Wait for all processes within a communicator to reach the\n"
  "barrier.\n";

const char* communicator_split_docstring = 
  "Split the communicator into multiple, disjoint communicators\n"
  "each of which is based on a particular color. This is a\n"
  "collective operation that returns a new communicator that is a\n"
  "subgroup of this. This routine is functionally equivalent to\n"
  "MPI_Comm_split.\n\n"
  "color is the color of this process. All processes with the\n"
  "same color value will be placed into the same group.\n\n"
  "If provided, key is a key value that will be used to determine\n"
  "the ordering of processes with the same color in the resulting\n"
  "communicator. If omitted, the key will default to the rank of\n"
  "the process in the current communicator.\n\n"
  "Returns a new Communicator instance containing all of the \n"
  "processes in this communicator that have the same color.\n";

const char* communicator_abort_docstring = 
  "Makes a \"best attempt\" to abort all of the tasks in the group of\n"
  "this communicator. Depending on the underlying MPI\n"
  "implementation, this may either abort the entire program (and\n"
  "possibly return errcode to the environment) or only abort\n"
  "some processes, allowing the others to continue. Consult the\n"
  "documentation for your MPI implementation. This is equivalent to\n"
  "a call to MPI_Abort\n\n"
  "errcode is the error code to return from aborted processes.\n";

/***********************************************************
 * request documentation                                   *
 ***********************************************************/
const char* request_docstring = 
  "The Request class contains information about a non-blocking send\n"
  "or receive and will be returned from isend or irecv, respectively.\n"
  "When a Request object represents a completed irecv, the `value' \n"
  "attribute will contain the received value.\n";

const char* request_with_value_docstring = 
  "This class is an implementation detail. Any call that accepts a\n"
  "Request also accepts a RequestWithValue, and vice versa.\n";

const char* request_wait_docstring =
  "Wait until the communication associated with this request has\n"
  "completed. For a request that is associated with an isend(), returns\n"
  "a Status object describing the communication. For an irecv()\n"
  "operation, returns the received value by default. However, when\n"
  "return_status=True, a (value, status) pair is returned by a\n"
  "completed irecv request.\n";

const char* request_test_docstring =
  "Determine whether the communication associated with this request\n"
  "has completed successfully. If so, returns the Status object\n"
  "describing the communication (for an isend request) or a tuple\n"
  "containing the received value and a Status object (for an irecv\n"
  "request). Note that once test() returns a Status object, the\n"
  "request has completed and wait() should not be called.\n";

const char* request_cancel_docstring =
  "Cancel a pending communication, assuming it has not already been\n"
  "completed.\n";

const char* request_value_docstring =
  "If this request originated in an irecv(), this property makes the"
  "sent value accessible once the request completes.\n"
  "\n"
  "If no value is available, ValueError is raised.\n";

/***********************************************************
 * skeleton/content documentation                          *
 ***********************************************************/
const char* object_without_skeleton_docstring = 
  "The ObjectWithoutSkeleton class is an exception class used only\n"
  "when the skeleton() or get_content() function is called with an\n"
  "object that is not supported by the skeleton/content mechanism.\n"
  "All C++ types for which skeletons and content can be transmitted\n"
  "must be registered with the C++ routine:\n"
  "  boost::mpi::python::register_skeleton_and_content\n";

const char* object_without_skeleton_object_docstring = 
  "The object on which skeleton() or get_content() was invoked.\n";

const char* skeleton_proxy_docstring = 
  "The SkeletonProxy class is used to represent the skeleton of an\n"
  "object. The SkeletonProxy can be used as the value parameter of\n"
  "send() or isend() operations, but instead of transmitting the\n"
  "entire object, only its skeleton (\"shape\") will be sent, without\n"
  "the actual data. Its content can then be transmitted, separately.\n"
  "\n"
  "User code cannot generate SkeletonProxy instances directly. To\n"
  "refer to the skeleton of an object, use skeleton(object). Skeletons\n"
  "can also be received with the recv() and irecv() methods.\n"
  "\n"
  "Note that the skeleton/content mechanism can only be used with C++\n"
  "types that have been explicitly registered.\n";

const char* skeleton_proxy_object_docstring = 
  "The actual object whose skeleton is represented by this proxy object.\n";

const char* content_docstring = 
  "The content is a proxy class that represents the content of an object,\n"
  "which can be separately sent or received from its skeleton.\n"
  "\n"
  "User code cannot generate content instances directly. Call the\n"
  "get_content() routine to retrieve the content proxy for a particular\n"
  "object. The content instance can be used with any of the send() or\n"
  "recv() variants. Note that get_content() can only be used with C++\n"
  "data types that have been explicitly registered with the Python\n"
  "skeleton/content mechanism.\n";

const char* skeleton_docstring = 
  "The skeleton function retrieves the SkeletonProxy for its object\n"
  "parameter, allowing the transmission of the skeleton (or \"shape\")\n"
  "of the object separately from its data. The skeleton/content mechanism\n"
  "is useful when a large data structure remains structurally the same\n"
  "throughout a computation, but its content (i.e., the values in the\n"
  "structure) changes several times. Tranmission of the content part does\n"
  "not require any serialization or unnecessary buffer copies, so it is\n"
  "very efficient for large data structures.\n"
  "\n"
  "Only C++ types that have been explicitly registered with the Boost.MPI\n"
  "Python library can be used with the skeleton/content mechanism. Use:\b"
  "  boost::mpi::python::register_skeleton_and_content\n";

const char* get_content_docstring = 
  "The get_content function retrieves the content for its object parameter,\n"
  "allowing the transmission of the data in a data structure separately\n"
  "from its skeleton (or \"shape\"). The skeleton/content mechanism\n"
  "is useful when a large data structure remains structurally the same\n"
  "throughout a computation, but its content (i.e., the values in the\n"
  "structure) changes several times. Tranmission of the content part does\n"
  "not require any serialization or unnecessary buffer copies, so it is\n"
  "very efficient for large data structures.\n"
  "\n"
  "Only C++ types that have been explicitly registered with the Boost.MPI\n"
  "Python library can be used with the skeleton/content mechanism. Use:\b"
  "  boost::mpi::python::register_skeleton_and_content\n";

/***********************************************************
 * status documentation                                    *
 ***********************************************************/
const char* status_docstring = 
  "The Status class stores information about a given message, including\n"
  "its source, tag, and whether the message transmission was cancelled\n"
  "or resulted in an error.\n";

const char* status_source_docstring =
  "The source of the incoming message.\n";

const char* status_tag_docstring =
  "The tag of the incoming message.\n";

const char* status_error_docstring =
  "The error code associated with this transmission.\n";

const char* status_cancelled_docstring =
  "Whether this transmission was cancelled.\n";

/***********************************************************
 * timer documentation                                     *
 ***********************************************************/
const char* timer_docstring =
  "The Timer class is a simple wrapper around the MPI timing facilities.\n";

const char* timer_default_constructor_docstring =
  "Initializes the timer. After this call, elapsed == 0.\n";

const char* timer_restart_docstring =
  "Restart the timer, after which elapsed == 0.\n";

const char* timer_elapsed_docstring =
  "The time elapsed since initialization or the last restart(),\n"
  "whichever is more recent.\n";

const char* timer_elapsed_min_docstring =
  "Returns the minimum non-zero value that elapsed may return\n"
  "This is the resolution of the timer.\n";

const char* timer_elapsed_max_docstring =
  "Return an estimate of the maximum possible value of elapsed. Note\n"
  "that this routine may return too high a value on some systems.\n";

const char* timer_time_is_global_docstring = 
  "Determines whether the elapsed time values are global times or\n"
  "local processor times.\n";

} } } // end namespace boost::mpi::python
