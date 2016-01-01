#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wiredtiger, wttest
from wtscenario import check_scenarios

# test_base05.py
#    Cursor operations
class test_base05(wttest.WiredTigerTestCase):
    """
    Test that various types of content can be stored
    Test the 'english' huffman encoding with English and non-English strings.
    """

    table_name1 = 'test_base05a'
    table_name2 = 'test_base05b'
    nentries = 1000
    scenarios = check_scenarios([
        ('no_huffman', dict(extraconfig='')),
        ('huffman_key', dict(extraconfig='huffman_key="english"')),
        ('huffman_val', dict(extraconfig='huffman_value="english"')),
        ('huffman_keyval', dict(extraconfig='huffman_key="english",huffman_value="english"'))
        ])

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return self.extraconfig

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    # Moby Dick by Herman Melville, Chapter 1 (excerpt)
    english_strings = [
        'Call me Ishmael.',
        'Some years ago-never mind how long precisely-having little or no money in my purse, and nothing particular to interest me on shore, I thought I would sail about a little and see the watery part of the world.',
        'It is a way I have of driving off the spleen and regulating the circulation.',
        'Whenever I find myself growing grim about the mouth; whenever it is a damp, drizzly November in my soul; whenever I find myself involuntarily pausing before coffin warehouses, and bringing up the rear of every funeral I meet; and especially whenever my hypos get such an upper hand of me, that it requires a strong moral principle to prevent me from deliberately stepping into the street, and methodically knocking people\'s hats off-then, I account it high time to get to sea as soon as I can.',
        'This is my substitute for pistol and ball.',
        'With a philosophical flourish Cato throws himself upon his sword; I quietly take to the ship.',
        'There is nothing surprising in this.',
        'If they but knew it, almost all men in their degree, some time or other, cherish very nearly the same feelings towards the ocean with me.',
        'There now is your insular city of the Manhattoes, belted round by wharves as Indian isles by coral reefs-commerce surrounds it with her surf.',
        'Right and left, the streets take you waterward.',
        'Its extreme downtown is the battery, where that noble mole is washed by waves, and cooled by breezes, which a few hours previous were out of sight of land.',
        'Look at the crowds of water-gazers there.',
        'Circumambulate the city of a dreamy Sabbath afternoon.',
        'Go from Corlears Hook to Coenties Slip, and from thence, by Whitehall, northward.',
        'What do you see?-Posted like silent sentinels all around the town, stand thousands upon thousands of mortal men fixed in ocean reveries.',
        'Some leaning against the spiles; some seated upon the pier-heads; some looking over the bulwarks glasses! of ships from China; some high aloft in the rigging, as if striving to get a still better seaward peep.',
        'But these are all landsmen; of week days pent up in lath and plaster- tied to counters, nailed to benches, clinched to desks.',
        'How then is this?  Are the green fields gone?  What do they here?  But look! here come more crowds, pacing straight for the water, and seemingly bound for a dive.',
        'Strange!  Nothing will content them but the extremest limit of the land; loitering under the shady lee of yonder warehouses will not suffice.',
        'No.',
        'They must get just as nigh the water as they possibly can without falling in.',
        'And there they stand-miles of them-leagues.',
        'Inlanders all, they come from lanes and alleys, streets and avenues,- north, east, south, and west.',
        'Yet here they all unite.',
        'Tell me, does the magnetic virtue of the needles of the compasses of all those ships attract them thither?  Once more.',
        'Say you are in the country; in some high land of lakes.',
        'Take almost any path you please, and ten to one it carries you down in a dale, and leaves you there by a pool in the stream.',
        'There is magic in it.',
        'Let the most absent-minded of men be plunged in his deepest reveries-stand that man on his legs, set his feet a-going, and he will infallibly lead you to water, if water there be in all that region.',
        'Should you ever be athirst in the great American desert, try this experiment, if your caravan happen to be supplied with a metaphysical professor.',
        'Yes, as every one knows, meditation and water are wedded for ever.',
        'But here is an artist.',
        'He desires to paint you the dreamiest, shadiest, quietest, most enchanting bit of romantic landscape in all the valley of the Saco.',
        'What is the chief element he employs?  There stand his trees, each with a hollow trunk, as if a hermit and a crucifix were within; and here sleeps his meadow, and there sleep his cattle; and up from yonder cottage goes a sleepy smoke.',
        'Deep into distant woodlands winds a mazy way, reaching to overlapping spurs of mountains bathed in their hill-side blue.',
        'But though the picture lies thus tranced, and though this pine-tree shakes down its sighs like leaves upon this shepherd\'s head, yet all were vain, unless the shepherd\'s eye were fixed upon the magic stream before him.',
        'Go visit the Prairies in June, when for scores on scores of miles you wade knee-deep among Tiger-lilies-what is the one charm wanting?- Water - there is not a drop of water there!  Were Niagara but a cataract of sand, would you travel your thousand miles to see it?  Why did the poor poet of Tennessee, upon suddenly receiving two handfuls of silver, deliberate whether to buy him a coat, which he sadly needed, or invest his money in a pedestrian trip to Rockaway Beach?  Why is almost every robust healthy boy with a robust healthy soul in him, at some time or other crazy to go to sea?  Why upon your first voyage as a passenger, did you yourself feel such a mystical vibration, when first told that you and your ship were now out of sight of land?  Why did the old Persians hold the sea holy?  Why did the Greeks give it a separate deity, and own brother of Jove?  Surely all this is not without meaning.',
        'And still deeper the meaning of that story of Narcissus, who because he could not grasp the tormenting, mild image he saw in the fountain, plunged into it and was drowned.',
        'But that same image, we ourselves see in all rivers and oceans.',
        'It is the image of the ungraspable phantom of life; and this is the key to it all.',
        'Now, when I say that I am in the habit of going to sea whenever I begin to grow hazy about the eyes, and begin to be over conscious of my lungs, I do not mean to have it inferred that I ever go to sea as a passenger.',
        'For to go as a passenger you must needs have a purse, and a purse is but a rag unless you have something in it.',
        'Besides, passengers get sea-sick- grow quarrelsome-don\'t sleep of nights-do not enjoy themselves much, as a general thing;-no, I never go as a passenger; nor, though I am something of a salt, do I ever go to sea as a Commodore, or a Captain, or a Cook.',
        'I abandon the glory and distinction of such offices to those who like them.',
        'For my part, I abominate all honorable respectable toils, trials, and tribulations of every kind whatsoever.',
        'It is quite as much as I can do to take care of myself, without taking care of ships, barques, brigs, schooners, and what not.',
        'And as for going as cook,-though I confess there is considerable glory in that, a cook being a sort of officer on ship-board-yet, somehow, I never fancied broiling fowls;-though once broiled, judiciously buttered, and judgmatically salted and peppered, there is no one who will speak more respectfully, not to say reverentially, of a broiled fowl than I will.',
        'It is out of the idolatrous dotings of the old Egyptians upon broiled ibis and roasted river horse, that you see the mummies of those creatures in their huge bakehouses the pyramids.',
        'No, when I go to sea, I go as a simple sailor, right before the mast, plumb down into the fore-castle, aloft there to the royal mast-head.',
        'True, they rather order me about some, and make me jump from spar to spar, like a grasshopper in a May meadow.',
        'And at first, this sort of thing is unpleasant enough.',
        'It touches one\'s sense of honor, particularly if you come of an old established family in the land, the Van Rensselaers, or Randolphs, or Hardicanutes.',
        'And more than all, if just previous to putting your hand into the tar-pot, you have been lording it as a country schoolmaster, making the tallest boys stand in awe of you.',
        'The transition is a keen one, I assure you, from a schoolmaster to a sailor, and requires a strong decoction of Seneca and the Stoics to enable you to grin and bear it.',
        'But even this wears off in time.'
        ]

    # 'Hello' in several languages that use the non-latin part of unicode
    non_english_strings = [
        # This notation creates 'string' objects that have embedded unicode.
        '\u20320\u22909',
        '\u1571\u1604\u1587\u1617\u1604\u1575\u1605\u32\u1593\u1604\u1610\u1603\u1605',
        '\u1513\u1500\u1493\u1501',
        '\u20170\u26085\u12399',
        '\u50504\u45397\u54616\u49464\u50836',
        '\u1047\u1076\u1088\u1072\u1074\u1089\u1090\u1074\u1091\u1081\u1090\u1077',
        "\u4306\u4304\u4315\u4304\u4320\u4335\u4317\u4305\u4304",
        u'Hello',  # This notation creates a 'unicode' type object.
        ]

    def mixed_string(self, n):
        """
        Build a string composed of some wide ranging number of substrings,
        chosen mostly from the english_strings list with occasional
        entries from non_english_strings list.
        The returned value should be a somewhat random looking
        mix, but must be repeatable for any given N.
        """
        nstrings = 2 << (n % 10)
        result = ''
        for i in range(nstrings):
            if (n + i) % 20 == 0:
                reflist = self.non_english_strings
            else:
                reflist = self.english_strings
            choice = (n + i) % len(reflist)
            result += reflist[choice]
        return result + ':' + str(n)

    def test_table_ss(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S,' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1)
        numbers = {}
        for i in range(0, self.nentries):
            numbers[i] = i
            key = self.mixed_string(i)
            value = self.mixed_string(i+1)
            cursor[key] = value

        # quick spot check to make sure searches work
        for divisor in [3, 5, 7]:
            i = self.nentries / divisor
            key = self.mixed_string(i)
            value = self.mixed_string(i+1)
            cursor.set_key(key)
            self.assertEqual(0, cursor.search())
            self.assertEqual(key, cursor.get_key())
            self.assertEqual(value, cursor.get_value())

        total = 0
        cursor.reset()
        for key, value in cursor:
            colonpos = key.rfind(':')
            i = int(key[(colonpos+1):])
            del numbers[i]
            self.assertEqual(key, self.mixed_string(i))
            self.assertEqual(value, self.mixed_string(i+1))
            total += 1

        self.assertEqual(total, self.nentries)
        self.assertEqual(0, len(numbers))
        cursor.close()

    def do_test_table_base(self, convert):
        """
        Base functionality that uses regular strings with
        non-ASCII (UTF) chars and optionally converts them to
        Unicode (considered a type separate from string in Python).
        """
        create_args = 'key_format=S,value_format=S,' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1)
        strlist = self.non_english_strings
        for i in range(0, len(strlist)):
            if convert:
                key = val = unicode(strlist[i])
            else:
                key = val = strlist[i]
            cursor[key] = val

        for i in range(0, len(strlist)):
            if convert:
                key = val = unicode(strlist[i])
            else:
                key = val = strlist[i]
            cursor.set_key(key)
            self.assertEqual(0, cursor.search())
            self.assertEqual(key, cursor.get_key())
            self.assertEqual(val, cursor.get_value())

    def test_table_string(self):
        """
        Create entries using regular strings that may have non-ASCII (UTF)
        chars, and read back in a cursor: key=string, value=string
        """
        self.do_test_table_base(False)

    def test_table_unicode(self):
        """
        Create entries using unicode strings,
        and read back in a cursor: key=string, value=string
        """
        self.do_test_table_base(True)

if __name__ == '__main__':
    wttest.run()
