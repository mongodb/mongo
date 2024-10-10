import re

import SCons

task_limiter_patterns = {}


def setup_task_limiter(
    env, name, concurrency_ratio=0.75, builders=None, source_file_regex=".*", target_file_regex=".*"
):
    global task_limiter_patterns

    task_limiter_patterns[name] = {}
    task_limiter_patterns[name]["source"] = re.compile(source_file_regex)
    task_limiter_patterns[name]["target"] = re.compile(target_file_regex)

    # We need to convert the ratio value into a int that corrlates to a specific
    # number of concurrent jobs allowed
    concurrency_ratio = float(concurrency_ratio)
    if concurrency_ratio <= 0.0:
        env.FatalError(
            f"The concurrency ratio for {name} must be a positive, got {max_concurrency}"
        )

    if concurrency_ratio > 1.0:
        concurrency_ratio = 1.0

    max_concurrency = env.GetOption("num_jobs") * concurrency_ratio
    max_concurrency = round(max_concurrency)
    if max_concurrency < 1.0:
        max_concurrency = 1.0

    max_concurrency = int(max_concurrency)

    # A bound map of stream (as in stream of work) name to side-effect
    # file. Since SCons will not allow tasks with a shared side-effect
    # to execute concurrently, this gives us a way to limit link jobs
    # independently of overall SCons concurrency.
    concurrent_stream_map = dict()

    def task_limiter_emitter(target, source, env):
        global task_limiter_patterns
        nonlocal name

        matched = False
        for s_file in source:
            if re.search(task_limiter_patterns[name]["source"], s_file.path):
                matched = True
                break

        if not matched:
            for t_file in target:
                if re.search(task_limiter_patterns[name]["target"], t_file.path):
                    matched = True
                    break
        if matched:
            se_name = f"#{name}-stream{hash(str(target[0])) % max_concurrency}"
            se_node = concurrent_stream_map.get(se_name, None)
            if not se_node:
                se_node = env.Entry(se_name)
                # This may not be necessary, but why chance it
                env.NoCache(se_node)
                concurrent_stream_map[se_name] = se_node
            env.SideEffect(se_node, target)

        return (target, source)

    if isinstance(builders, dict):
        for target_builder, suffixes in builders.items():
            builder = env["BUILDERS"][target_builder]
            emitterdict = builder.builder.emitter
            for suffix in emitterdict.keys():
                if suffix not in suffixes:
                    continue
                base = emitterdict[suffix]
                emitterdict[suffix] = SCons.Builder.ListEmitter(
                    [
                        base,
                        task_limiter_emitter,
                    ]
                )
    else:
        for target_builder in builders:
            builder = env["BUILDERS"][target_builder]
            base_emitter = builder.emitter
            new_emitter = SCons.Builder.ListEmitter([base_emitter, task_limiter_emitter])
            builder.emitter = new_emitter

    return max_concurrency


def exists(env):
    return True


def generate(env):
    env.AddMethod(setup_task_limiter, "SetupTaskLimiter")
