import concurrent.futures

import opentelemetry.context


class OtelThreadPoolExecutor(concurrent.futures.ThreadPoolExecutor):
    def submit(self, fn, *args, **kwargs):
        context = opentelemetry.context.get_current()
        if context:

            def attach_context(*args, **kwargs):
                opentelemetry.context.attach(context)
                return fn(*args, **kwargs)

            return super().submit(attach_context, *args, **kwargs)
        else:
            return super().submit(fn, *args, **kwargs)
