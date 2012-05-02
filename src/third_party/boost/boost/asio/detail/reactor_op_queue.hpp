//
// detail/reactor_op_queue.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_REACTOR_OP_QUEUE_HPP
#define BOOST_ASIO_DETAIL_REACTOR_OP_QUEUE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/hash_map.hpp>
#include <boost/asio/detail/noncopyable.hpp>
#include <boost/asio/detail/op_queue.hpp>
#include <boost/asio/detail/reactor_op.hpp>
#include <boost/asio/error.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

template <typename Descriptor>
class reactor_op_queue
  : private noncopyable
{
public:
  // Constructor.
  reactor_op_queue()
    : operations_()
  {
  }

  // Add a new operation to the queue. Returns true if this is the only
  // operation for the given descriptor, in which case the reactor's event
  // demultiplexing function call may need to be interrupted and restarted.
  bool enqueue_operation(Descriptor descriptor, reactor_op* op)
  {
    typedef typename operations_map::iterator iterator;
    typedef typename operations_map::value_type value_type;
    std::pair<iterator, bool> entry =
      operations_.insert(value_type(descriptor, operations()));
    entry.first->second.op_queue_.push(op);
    return entry.second;
  }

  // Cancel all operations associated with the descriptor. Any operations
  // pending for the descriptor will be notified that they have been cancelled
  // next time perform_cancellations is called. Returns true if any operations
  // were cancelled, in which case the reactor's event demultiplexing function
  // may need to be interrupted and restarted.
  bool cancel_operations(Descriptor descriptor, op_queue<operation>& ops,
      const boost::system::error_code& ec =
        boost::asio::error::operation_aborted)
  {
    typename operations_map::iterator i = operations_.find(descriptor);
    if (i != operations_.end())
    {
      while (reactor_op* op = i->second.op_queue_.front())
      {
        op->ec_ = ec;
        i->second.op_queue_.pop();
        ops.push(op);
      }
      operations_.erase(i);
      return true;
    }

    return false;
  }

  // Whether there are no operations in the queue.
  bool empty() const
  {
    return operations_.empty();
  }

  // Determine whether there are any operations associated with the descriptor.
  bool has_operation(Descriptor descriptor) const
  {
    return operations_.find(descriptor) != operations_.end();
  }

  // Perform the operations corresponding to the descriptor. Returns true if
  // there are still unfinished operations queued for the descriptor.
  bool perform_operations(Descriptor descriptor, op_queue<operation>& ops)
  {
    typename operations_map::iterator i = operations_.find(descriptor);
    if (i != operations_.end())
    {
      while (reactor_op* op = i->second.op_queue_.front())
      {
        if (op->perform())
        {
          i->second.op_queue_.pop();
          ops.push(op);
        }
        else
        {
          return true;
        }
      }
      operations_.erase(i);
    }
    return false;
  }

  // Fill a descriptor set with the descriptors corresponding to each active
  // operation. The op_queue is used only when descriptors fail to be added to
  // the descriptor set.
  template <typename Descriptor_Set>
  void get_descriptors(Descriptor_Set& descriptors, op_queue<operation>& ops)
  {
    typename operations_map::iterator i = operations_.begin();
    while (i != operations_.end())
    {
      Descriptor descriptor = i->first;
      ++i;
      if (!descriptors.set(descriptor))
      {
        boost::system::error_code ec(error::fd_set_failure);
        cancel_operations(descriptor, ops, ec);
      }
    }
  }

  // Perform the operations corresponding to the ready file descriptors
  // contained in the given descriptor set.
  template <typename Descriptor_Set>
  void perform_operations_for_descriptors(
      const Descriptor_Set& descriptors, op_queue<operation>& ops)
  {
    typename operations_map::iterator i = operations_.begin();
    while (i != operations_.end())
    {
      typename operations_map::iterator op_iter = i++;
      if (descriptors.is_set(op_iter->first))
      {
        while (reactor_op* op = op_iter->second.op_queue_.front())
        {
          if (op->perform())
          {
            op_iter->second.op_queue_.pop();
            ops.push(op);
          }
          else
          {
            break;
          }
        }

        if (op_iter->second.op_queue_.empty())
          operations_.erase(op_iter);
      }
    }
  }

  // Get all operations owned by the queue.
  void get_all_operations(op_queue<operation>& ops)
  {
    typename operations_map::iterator i = operations_.begin();
    while (i != operations_.end())
    {
      typename operations_map::iterator op_iter = i++;
      ops.push(op_iter->second.op_queue_);
      operations_.erase(op_iter);
    }
  }

private:
  struct operations
  {
    operations() {}
    operations(const operations&) {}
    void operator=(const operations&) {}

    // The operations waiting on the desccriptor.
    op_queue<reactor_op> op_queue_;
  };

  // The type for a map of operations.
  typedef hash_map<Descriptor, operations> operations_map;

  // The operations that are currently executing asynchronously.
  operations_map operations_;
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_REACTOR_OP_QUEUE_HPP
