# General starlark utility functions

def retry_download_and_extract(ctx, tries, **kwargs):
    sleep_time = 1
    for attempt in range(tries):
        is_retriable = attempt + 1 < tries
        result = ctx.download_and_extract(allow_fail = is_retriable, **kwargs)
        if result.success:
            return result
        else:
            print("Download failed (Attempt #%s), sleeping for %s seconds then retrying..." % (attempt + 1, sleep_time))
            ctx.execute(["sleep", str(sleep_time)])
            sleep_time *= 2
