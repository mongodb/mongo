
import hacks_ubuntu

def findHacks( un ):
    if un[3].find( "Ubuntu" ) >= 0:
        return hacks_ubuntu
    return None
