# Fill out block comments to the full line length (currently 100).
#
# We're defining a "block comment" to be a multiline comment where each line
# begins with an alphabetic character.
#
# We also have some special logic to handle function description comments even
# though those don't conform to our definition of a "block comment".
import re, sys

# List of words in the current block comment.
words = []

# Whether we're inside a potential block comment.
multiline = False

# The maximum allowed line length.
line_length = 100

# How far to indent the current block comment.
indentation = 0

# Whether we're inside a function description comment. This is not a block
# comment by our definition but we want to fill these too.
function_desc = False

# Whether we've seen a line in the multiline comment to indicate that it is NOT
# a block comment. In that case don't use the refilling logic and just print the
# contents verbatim.
block = False

# The literal contents of the current block comment. If we realise halfway
# through the comment that it's not a block comment then we'll just print this
# out and pretend none of this ever happened.
comment = str()

for line in sys.stdin:
    sline = line.strip()
    # Beginning of a block comment.
    if sline == '/*':
        comment = line
        assert not multiline
        multiline = True
        block = True
        # Figure out how far we need to indent.
        indentation = 0
        for c in line:
            if c == ' ':
                indentation += 1
            elif c == '\t':
                indentation += 8
            else:
                break
    # End of a block comment.
    elif sline.endswith('*/'):
        comment += line
        # Don't mess with generated comments.
        # Scripts in dist rely on them to figure out where to generate code.
        if 'DO NOT EDIT' in comment:
            block = False
        if multiline and not block:
            sys.stdout.write(comment)
        elif multiline:
            indent_ws = ' ' * indentation
            sys.stdout.write('{}/*\n'.format(indent_ws))
            current_line = indent_ws + ' *'
            for i in range(len(words)):
                word = words[i]
                if word == '--' and function_desc:
                    sys.stdout.write(current_line + ' ' + word + '\n')
                    current_line = indent_ws + ' *' + ' ' * 4
                    continue
                if word == '\n':
                    # If we already have partially built a line, write it out.
                    if current_line != indent_ws + ' *':
                        sys.stdout.write(current_line + '\n')
                    # If there are more words in this comment after this
                    # newline, add another line break.
                    if i < (len(words) - 1):
                        sys.stdout.write(indent_ws + ' *' + '\n')
                    current_line = indent_ws + ' *'
                    continue
                if len(current_line) + len(word) >= line_length:
                    sys.stdout.write(current_line + '\n')
                    current_line = indent_ws + ' *'
                    if function_desc:
                        current_line += ' ' * 4
                current_line += ' ' + word
            sys.stdout.write(current_line + '\n')
            sys.stdout.write('{} */\n'.format(indent_ws))
        else:
            sys.stdout.write(line)
        block = False
        words = []
        multiline = False
        function_desc = False
    elif multiline:
        comment += line
        # We want to preserve newlines for block comments that have multiple paragraphs.
        if sline == '*':
            words.append('\n')
            continue
        # Function names begin with either a lowercase char or an underscore.
        if (len(sline) >= 3 and sline.startswith('*') and sline[1] == ' ' and
            (sline[2].islower() or sline[2] == '_') and sline.endswith('--')):
            function_desc = True
        # We're only reformatting block comments where each line begins with a space and an
        # normal comment character after the asterisk, or a parenthetical. The only exceptions
        # are function descriptions.
        block = block and \
            len(sline) >= 3 and sline.startswith('*') and sline[1] == ' ' and \
            (sline[2].isalpha() or sline[2] == '"' or sline[2] == "'" or (len(sline) >= 5 and \
            (sline[2] == '(' and sline[3].isalpha() and sline[4] != ')'))) or function_desc
        # Trim asterisks at the beginning of each line in a multiline comment.
        if sline.startswith('*'):
            sline = sline[1:]
        # Might be trailing whitespace after the asterisk. Leading strip again.
        sline = sline.lstrip()
        words.extend(sline.split())
    else:
        sys.stdout.write(line)
