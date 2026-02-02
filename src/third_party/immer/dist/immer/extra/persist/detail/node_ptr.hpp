#pragma once

#include <functional>
#include <immer/vector.hpp>

namespace immer::persist::detail {

template <typename Node>
struct ptr_with_deleter
{
    Node* ptr;
    std::function<void(Node* ptr)> deleter;

    ptr_with_deleter()
        : ptr{nullptr}
        , deleter{}
    {
    }

    ptr_with_deleter(Node* ptr_, std::function<void(Node* ptr)> deleter_)
        : ptr{ptr_}
        , deleter{std::move(deleter_)}
    {
    }

    void dec() const
    {
        if (ptr && ptr->dec()) {
            deleter(ptr);
        }
    }

    friend void swap(ptr_with_deleter& x, ptr_with_deleter& y)
    {
        using std::swap;
        swap(x.ptr, y.ptr);
        swap(x.deleter, y.deleter);
    }
};

template <typename Node>
class node_ptr
{
public:
    node_ptr() = default;

    node_ptr(Node* ptr_, std::function<void(Node* ptr)> deleter_)
        : ptr{ptr_, std::move(deleter_)}
    {
        // Assuming the node has just been created and not calling inc() on
        // it.
    }

    node_ptr(const node_ptr& other)
        : ptr{other.ptr}
    {
        if (ptr.ptr) {
            ptr.ptr->inc();
        }
    }

    node_ptr(node_ptr&& other)
        : node_ptr{}
    {
        swap(*this, other);
    }

    node_ptr& operator=(const node_ptr& other)
    {
        auto temp = other;
        swap(*this, temp);
        return *this;
    }

    node_ptr& operator=(node_ptr&& other)
    {
        auto temp = node_ptr{std::move(other)};
        swap(*this, temp);
        return *this;
    }

    ~node_ptr() { ptr.dec(); }

    explicit operator bool() const { return ptr.ptr; }

    Node* release() &&
    {
        auto result = ptr.ptr;
        ptr.ptr     = nullptr;
        return result;
    }

    auto release_full() &&
    {
        auto result = ptr;
        ptr         = {};
        return result;
    }

    Node* get() { return ptr.ptr; }

    friend void swap(node_ptr& x, node_ptr& y)
    {
        using std::swap;
        swap(x.ptr, y.ptr);
    }

private:
    ptr_with_deleter<Node> ptr;
};

} // namespace immer::persist::detail
