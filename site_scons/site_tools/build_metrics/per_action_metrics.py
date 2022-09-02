import functools
import time
import psutil
import platform

import memory_profiler
import SCons
import sys

from .util import fullname
from .protocol import BuildMetricsCollector


class ProfiledFunction:
    """
    A class which mimics a FunctionAction function, behaving exactly the same
    as the original FunctionAction function, except for gather perf metrics
    during the __call__ of the function.
    """

    def __init__(self, per_action_instance, original_func) -> None:
        self.original_func = original_func
        self.per_action_instance = per_action_instance

        if hasattr(original_func, 'strfunction'):
            self.strfunction = original_func.strfunction

        if isinstance(self.original_func, SCons.Action.ActionCaller):
            self.original_func = original_func.__call__

        self.__name__ = "profiled_function"

    def __call__(self, target, source, env):
        return self.function_action_execute(target, source, env)

    def __str__(self) -> str:
        return str(self.original_func)

    def function_action_execute(self, target, source, env):

        task_metrics = {
            'outputs': [str(t) for t in target],
            'inputs': [str(s) for s in source],
            'action': fullname(self.original_func),
            'builder': target[0].get_builder().get_name(target[0].get_env()),
        }
        profile = memory_profiler.LineProfiler(include_children=False)

        task_metrics['start_time'] = time.time_ns()
        thread_start_time = time.thread_time_ns()
        return_value = profile(self.original_func)(target=target, source=source, env=env)
        task_metrics['cpu_time'] = time.thread_time_ns() - thread_start_time
        task_metrics['end_time'] = time.time_ns()

        memory_increases_per_line = []
        for (file_where_code_is, lines_of_code) in profile.code_map.items():

            # skip the first item in the list because this is just the initial
            # memory state, and we are interested just in the increases
            for (line_number, memory_usage) in list(lines_of_code)[1:]:
                if memory_usage:
                    memory_increase = memory_usage[0]
                    memory_increases_per_line.append(memory_increase)

        task_metrics['mem_usage'] = int(sum(memory_increases_per_line) * 1024 * 1024)

        self.per_action_instance.build_tasks_metrics.append(task_metrics)
        task_metrics['array_index'] = self.per_action_instance.build_tasks_metrics.index(
            task_metrics)

        return return_value


class PerActionMetrics(BuildMetricsCollector):
    """
    Creates hooks the CommandAction and FunctionAction execute calls in SCons to track
    CPU, memory and duration of execution of said action types.
    """

    def __init__(self) -> None:
        self.build_tasks_metrics = []

        # place hooks into scons internals to give us a chance to
        # adjust things to take measurements
        original_command_execute = SCons.Action.CommandAction.execute

        def build_metrics_CommandAction_execute(command_action_instance, target, source, env,
                                                executor=None):
            if 'conftest' not in str(target[0]):

                # We use the SPAWN var to control the SCons proper execute to call our spawn.
                # We set the spawn back after the proper execute is done
                original_spawn = env['SPAWN']
                env['SPAWN'] = functools.partial(self.command_spawn_func, target=target,
                                                 source=source)
                result = original_command_execute(command_action_instance, target, source, env,
                                                  executor)
                env['SPAWN'] = original_spawn
            else:
                result = original_command_execute(command_action_instance, target, source, env,
                                                  executor)
            return result

        SCons.Action.CommandAction.execute = build_metrics_CommandAction_execute

        original_function_action_execute = SCons.Action.FunctionAction.execute

        def build_metrics_FunctionAction_execute(function_action_instance, target, source, env,
                                                 executor=None):
            if target and 'conftest' not in str(target[0]) and not isinstance(
                    function_action_instance.execfunction, ProfiledFunction):

                # set our profiled function class as the function action call. Profiled function
                # should look and behave exactly as the original function, besides the __call__
                # behaving differently. We set back the original function for posterity just in case
                original_func = function_action_instance.execfunction
                function_action_instance.execfunction = ProfiledFunction(
                    self, function_action_instance.execfunction)
                original_function_action_execute(function_action_instance, target, source, env,
                                                 executor)
                function_action_instance.execfunction = original_func
            else:
                return original_function_action_execute(function_action_instance, target, source,
                                                        env, executor)

        SCons.Action.FunctionAction.execute = build_metrics_FunctionAction_execute

    def get_name(self):
        return "Per-Action Metrics"

    def get_mem_cpu(self, proc):
        with proc.oneshot():
            cpu = (proc.cpu_times().system + proc.cpu_times().user)
            mem = proc.memory_info().vms
        for p in proc.children(recursive=True):
            with p.oneshot():
                cpu += (p.cpu_times().system + p.cpu_times().user)
                mem += p.memory_info().vms
        return cpu, mem

    def track_process(self, proc, target):
        """ Poll virtual memory of a process and children. """
        try:
            peak_cpu, peak_mem = self.get_mem_cpu(proc)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            return 0, 0

        while proc.poll() is None:
            try:
                cpu, mem = self.get_mem_cpu(proc)
                if peak_cpu < cpu:
                    peak_cpu = cpu
                if peak_mem < mem:
                    peak_mem = mem
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
            else:
                time.sleep(0.01)

        return peak_cpu, peak_mem

    def command_spawn_func(self, sh, escape, cmd, args, env, target, source):

        task_metrics = {
            'outputs': [str(t) for t in target],
            'inputs': [str(s) for s in source],
            'action': ' '.join(args),
            'start_time': time.time_ns(),
            'builder': target[0].get_builder().get_name(target[0].get_env()),
        }

        if sys.platform[:3] == 'win':
            # have to use shell=True for windows because of https://github.com/python/cpython/issues/53908
            proc = psutil.Popen(' '.join(args), env=env, close_fds=True, shell=True)
        else:
            proc = psutil.Popen([sh, '-c', ' '.join(args)], env=env, close_fds=True)

        cpu_usage, mem_usage = self.track_process(proc, target[0])
        return_code = proc.wait()

        task_metrics['end_time'] = time.time_ns()
        task_metrics['cpu_time'] = int(cpu_usage * (10.0**9.0))
        # apparently macos big sur (11) changed some of the api for getting memory,
        # so the memory comes up a bit larger than expected:
        # https://github.com/giampaolo/psutil/issues/1908
        if sys.platform == "darwin" and platform.mac_ver()[0] and int(
                platform.mac_ver()[0].split('.')[0]) > 10:
            task_metrics['mem_usage'] = int(mem_usage / 1024.0)
        else:
            task_metrics['mem_usage'] = int(mem_usage)
        self.build_tasks_metrics.append(task_metrics)
        task_metrics['array_index'] = self.build_tasks_metrics.index(task_metrics)

        return return_code

    def finalize(self):
        return 'build_tasks', self.build_tasks_metrics
