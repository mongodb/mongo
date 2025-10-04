# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

"""All the matchers.

Matchers, a way to express complex assertions outside the testcase.

Inspired by 'hamcrest'.

Matcher provides the abstract API that all matchers need to implement.

Bundled matchers are listed in __all__: a list can be obtained by running
$ python -c 'import testtools.matchers; print testtools.matchers.__all__'
"""

__all__ = [
    'AfterPreprocessing',
    'AllMatch',
    'Always',
    'Annotate',
    'AnyMatch',
    'Contains',
    'ContainsAll',
    'ContainedByDict',
    'ContainsDict',
    'DirContains',
    'DirExists',
    'DocTestMatches',
    'EndsWith',
    'Equals',
    'FileContains',
    'FileExists',
    'GreaterThan',
    'HasLength',
    'HasPermissions',
    'Is',
    'IsDeprecated',
    'IsInstance',
    'KeysEqual',
    'LessThan',
    'MatchesAll',
    'MatchesAny',
    'MatchesDict',
    'MatchesException',
    'MatchesListwise',
    'MatchesPredicate',
    'MatchesPredicateWithParams',
    'MatchesRegex',
    'MatchesSetwise',
    'MatchesStructure',
    'Never',
    'NotEquals',
    'Not',
    'PathExists',
    'Raises',
    'raises',
    'SameMembers',
    'SamePath',
    'StartsWith',
    'TarballContains',
    'Warnings',
    'WarningMessage'
    ]

from ._basic import (
    Contains,
    EndsWith,
    Equals,
    GreaterThan,
    HasLength,
    Is,
    IsInstance,
    LessThan,
    MatchesRegex,
    NotEquals,
    SameMembers,
    StartsWith,
    )
from ._const import (
    Always,
    Never,
    )
from ._datastructures import (
    ContainsAll,
    MatchesListwise,
    MatchesSetwise,
    MatchesStructure,
    )
from ._dict import (
    ContainedByDict,
    ContainsDict,
    KeysEqual,
    MatchesDict,
    )
from ._doctest import (
    DocTestMatches,
    )
from ._exception import (
    MatchesException,
    Raises,
    raises,
    )
from ._filesystem import (
    DirContains,
    DirExists,
    FileContains,
    FileExists,
    HasPermissions,
    PathExists,
    SamePath,
    TarballContains,
    )
from ._higherorder import (
    AfterPreprocessing,
    AllMatch,
    Annotate,
    AnyMatch,
    MatchesAll,
    MatchesAny,
    MatchesPredicate,
    MatchesPredicateWithParams,
    Not,
    )
from ._warnings import (
    IsDeprecated,
    WarningMessage,
    Warnings,
    )

# XXX: These are not explicitly included in __all__.  It's unclear how much of
# the public interface they really are.
from ._impl import (  # noqa: F401
    Matcher,
    Mismatch,
    MismatchError,
    )
