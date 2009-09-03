
import hacks_ubuntu
import os;

def findHacks( un ):
    if os.path.exists( "/etc/debian_version" ) or un[3].find( "Ubuntu" ) >= 0:
        return hacks_ubuntu
    return None
