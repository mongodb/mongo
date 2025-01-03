#pragma once

#include "mongo/db/modules/monograph/tx_service/include/circular_queue.h"
#include <memory>
#include <stack>
#include <vector>

namespace mongo {
template <typename T>
void deinit(T* ptr) {}

template <typename T>
class ObjectPool {
public:
    ObjectPool() = default;
    ~ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;

    class Deleter {
    public:
        void operator()(T* ptr) {
            deinit(ptr);
            _localPool.Enqueue(std::unique_ptr<T>(ptr));
        }
    };

    /*
      Implicitly convert type for classes which need a polymorphism Deleter.
      For example, RecoveryUnit and MonographRecoveryUnit
    */
    template <typename Base>
    static void PolyDeleter(Base* ptr) {
        T* dptr = static_cast<T*>(ptr);
        deinit(dptr);
        _localPool.Enqueue(std::unique_ptr<T>(dptr));
    }

    template <typename... Args>
    static std::unique_ptr<T, Deleter> newObject(Args&&... args) {
        T* ptr{nullptr};

        if (_localPool.Size() == 0) {
            ptr = new T(std::forward<Args>(args)...);
        } else {
            ptr = _localPool.Peek().release();
            _localPool.Dequeue();
            ptr->reset(std::forward<Args>(args)...);
        }
        return std::unique_ptr<T, Deleter>(ptr);
    }

    template <typename Base, typename... Args>
    static std::unique_ptr<Base, void (*)(Base*)> newObject(Args&&... args) {
        T* ptr{nullptr};

        if (_localPool.Size() == 0) {
            ptr = new T(std::forward<Args>(args)...);
        } else {
            ptr = _localPool.Peek().release();
            _localPool.Dequeue();
            ptr->reset(std::forward<Args>(args)...);
        }
        return std::unique_ptr<Base, void (*)(Base*)>(ptr, &PolyDeleter<Base>);
    }

    template <typename... Args>
    static std::shared_ptr<T> newObjectSharedPointer(Args&&... args) {
        T* ptr{nullptr};

        if (_localPool.Size() == 0) {
            ptr = new T(std::forward<Args>(args)...);
        } else {
            ptr = _localPool.Peek().release();
            _localPool.Dequeue();
            ptr->reset(std::forward<Args>(args)...);
        }
        return std::shared_ptr<T>(ptr, Deleter());
    }

    /*
      Some Mongo classes have owned custom deleter.
      We could modify their custom to reuse Object
      instead of returning std::unique_ptr<T, Deleter> when creation.
      For example, PlanExecutor
    */
    template <typename... Args>
    static T* newObjectRawPointer(Args&&... args) {
        T* ptr{nullptr};

        if (_localPool.Size() == 0) {
            ptr = new T(std::forward<Args>(args)...);
        } else {
            ptr = _localPool.Peek().release();
            _localPool.Dequeue();
            ptr->reset(std::forward<Args>(args)...);
        }
        return ptr;
    }

    /*
      Only class that allocate call newObjectRawPointer
      need call this function to recycle object manually.
    */
    static void recycleObject(T* ptr) {
        deinit(ptr);
        _localPool.Enqueue(std::unique_ptr<T>(ptr));
    }

private:
    static constexpr size_t kDefaultCapacity{32};
    static thread_local CircularQueue<std::unique_ptr<T>> _localPool;
};

template <typename T>
thread_local CircularQueue<std::unique_ptr<T>> ObjectPool<T>::_localPool = {};

}  // namespace mongo


// namespace mongo {
// template <typename T>
// class ObjectPool {
// public:
//     ObjectPool() = default;
//     ~ObjectPool() = default;
//     ObjectPool(const ObjectPool&) = delete;
//     ObjectPool(ObjectPool&&) = delete;

//     class Deleter {
//     public:
//         void operator()(T* ptr) {
//             _localPool.push(std::unique_ptr<T>(ptr));
//         }
//     };

//     /*
//       Implicitly convert type for classes which need a polymorphism Deleter.
//       For example, RecoveryUnit and MonographRecoveryUnit
//     */
//     template <typename Base>
//     static void PolyDeleter(Base* ptr) {
//         _localPool.push(std::unique_ptr<T>(static_cast<T*>(ptr)));
//     }

//     template <typename... Args>
//     static std::unique_ptr<T, Deleter> newObject(Args&&... args) {
//         T* ptr{nullptr};

//         if (_localPool.empty()) {
//             ptr = new T(std::forward<Args>(args)...);
//         } else {
//             ptr = _localPool.top().release();
//             _localPool.pop();
//             ptr->reset(std::forward<Args>(args)...);
//         }
//         return std::unique_ptr<T, Deleter>(ptr);
//     }

//     template <typename Base, typename... Args>
//     static std::unique_ptr<Base, void (*)(Base*)> newObject(Args&&... args) {
//         T* ptr{nullptr};

//         if (_localPool.empty()) {
//             ptr = new T(std::forward<Args>(args)...);
//         } else {
//             ptr = _localPool.top().release();
//             _localPool.pop();
//             ptr->reset(std::forward<Args>(args)...);
//         }
//         return std::unique_ptr<Base, void (*)(Base*)>(ptr, &PolyDeleter<Base>);
//     }

//     template <typename... Args>
//     static std::shared_ptr<T> newObjectSharedPointer(Args&&... args) {
//         T* ptr{nullptr};

//         if (_localPool.empty()) {
//             ptr = new T(std::forward<Args>(args)...);
//         } else {
//             ptr = _localPool.top().release();
//             _localPool.pop();
//             ptr->reset(std::forward<Args>(args)...);
//         }
//         return std::shared_ptr<T>(ptr, Deleter());
//     }

//     /*
//       Some Mongo classes have owned custom deleter.
//       We could modify their custom to reuse Object
//       instead of returning std::unique_ptr<T, Deleter> when creation.
//       For example, PlanExecutor
//     */
//     template <typename... Args>
//     static T* newObjectRawPointer(Args&&... args) {
//         T* ptr{nullptr};

//         if (_localPool.empty()) {
//             ptr = new T(std::forward<Args>(args)...);
//         } else {
//             ptr = _localPool.top().release();
//             _localPool.pop();
//             ptr->reset(std::forward<Args>(args)...);
//         }
//         return ptr;
//     }

//     /*
//       Only class that allocate call newObjectRawPointer
//       need call this function to recycle object manually.
//     */
//     static void recycleObject(T* ptr) {
//         _localPool.push(std::unique_ptr<T>(ptr));
//     }

// private:
//     static constexpr size_t kDefaultCapacity{32};
//     static thread_local std::stack<std::unique_ptr<T>, std::vector<std::unique_ptr<T>>>
//     _localPool;
// };

// template <typename T>
// thread_local std::stack<std::unique_ptr<T>, std::vector<std::unique_ptr<T>>>
// ObjectPool<T>::_localPool = {};


// }  // namespace mongo

