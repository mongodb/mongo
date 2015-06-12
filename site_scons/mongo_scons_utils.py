import md5

def default_variant_dir_generator(target, source, env, for_signature):

    if env.GetOption('cache') != None:
        return 'cached'

    # If an option should affect the variant directory, name it here.
    variant_options = [
        'opt',
        'dbg',
    ]

    # Hash the named options and their values, and take the first 8 characters of the hash as
    # the variant name
    hasher = md5.md5()
    for option in variant_options:
        hasher.update(option)
        hasher.update(str(env.GetOption(option)))
    variant_dir = hasher.hexdigest()[0:8]

    # If our option hash yields a well known hash, replace it with its name.
    known_variant_hashes = {
        '343e6678' : 'debug',
        '85fcf9b0' : 'opt',
        '981ce870' : 'debug',
        '9fface73' : 'optdebug',
        'c52b1cc3' : 'opt',
    }

    return known_variant_hashes.get(variant_dir, variant_dir)


def os_specific_variant_dir_generator(target, source, env, for_signature):
    return '-'.join([
        env['TARGET_OS'],
        default_variant_dir_generator(target, source, env, for_signature)])
