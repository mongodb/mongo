#ifndef RUNTIME_MONITOR_H
#define RUNTIME_MONITOR_H

#include "thread_manager.h"

namespace test_harness {
class runtime_monitor {
    public:
    runtime_monitor()
    {
        thread_context *tc = new thread_context(thread_operation::MONITOR);
        _thread_manager.add_thread(tc, &monitor);
    }

    ~runtime_monitor()
    {
        _thread_manager.finish();
        /* Destructor of thread manager will be called automatically here. */
    }

    private:
    static void
    monitor(thread_context &context)
    {
        while (context.is_running()) {
            /* Junk operation to demonstrate thread_contexts. */
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    thread_manager _thread_manager;
};
} // namespace test_harness

#endif
