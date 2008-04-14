#! /usr/bin/env python
"""sockread.py -- a simple client to read from a socket
"""

# ***** BEGIN LICENSE BLOCK *****
# Version: MPL 1.1
# 
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
# 
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
# 
# The Original Code is the MPEG TS, PS and ES tools.
# 
# The Initial Developer of the Original Code is Amino Communications Ltd.
# Portions created by the Initial Developer are Copyright (C) 2008
# the Initial Developer. All Rights Reserved.
# 
# Contributor(s):
#   Amino Communications Ltd, Swavesey, Cambridge UK
# 
# ***** END LICENSE BLOCK *****

import sys
import socket

class DoneException(Exception):
    pass

def get_packet(sock,packet_size=188):
    """Read a packet from the socket, coping with partial reads.
    """
    data = ""
    total = 0
    while total < packet_size:
        data += sock.recv(packet_size - total)
        if len(data) == 0:
            raise DoneException
        total += len(data)
    return data

def read_next_packet(sock,f=None):
    """Read the next packet from the socket, checking and counting it.
    """
    data = get_packet(sock)
    if ord(data[0]) == 0x47 and len(data) == 188:
        sys.stdout.write(".")
    else:
        sys.stdout.write("[%x]/%d"%(ord(data[0]),len(data)))
    sys.stdout.flush()
    if f:
        f.write(data)

def main():
    total_packets = 0
    sock = socket.socket(socket.AF_INET,socket.SOCK_STREAM)
    print "Waiting on port 8889"
    sock.bind(("localhost",8889))
    sock.listen(1)
    conn, addr = sock.accept()
    print 'Connected by', addr
    #print "Writing to file temp.ts"
    #stream = file("temp.ts","wb")
    stream = None
    try:
        while 1:
            read_next_packet(conn,stream)
            total_packets += 1
    except DoneException:
        #stream.close()
        pass
    sys.stdout.write("\n")
    sys.stdout.write("Total packets: %d\n"%total_packets)
    sock.close()



if __name__ == "__main__":
#    try:
        main()
#    except KeyboardInterrupt:
#        print
