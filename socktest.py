#! /usr/bin/env python
"""socktest.py -- a simple client to talk to tsserve

Command line - optionally:

    -host   <host>        defaults to "localhost"
    -port   <port>        defaults to 8889
    -output <filename>    defaults to None
    -file   <filename>    the same
    -nonblock             the socket should operate in non-blocking mode

followed by zero or more commands, specified as:

    <letter> <count>      for n, F, f, r or R
or  <letter>              for any of the above, and also q, p, > and <, or 0 .. 9

If a <count> is given, it is the number of data packets to try to read before
issuing the next command.

The commands "n", "f", "F", r" and "R", and the "select channel and play" commands
"0" .. "9" may be given a count, in which case the command is given and then that
many data packets are read before the next command is given. If no count is given,
then data packets are read "forever".

The commands "p", ">" and "<" do not take a count, and do not read any data
packets.

The command "q" does not take a count, but reads the rest of the data packets.

If no commands are given, the default is "n" with no count.
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

global total_packets

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

def read_next_packet(sock,file=None):
    """Read the next packet from the socket, checking and counting it.
    """
    data = get_packet(sock)
    if ord(data[0]) == 0x47 and len(data) == 188:
        sys.stdout.write(".")
    else:
        sys.stdout.write("[%x]/%d"%(ord(data[0]),len(data)))
    sys.stdout.flush()
    global total_packets
    total_packets += 1
    if file:
        file.write(data)

def give_command(sock,command="n",file=None,howmany=None):
    """Give the command specified, and then read data packets.

    If `howmany` is specified, try to read that many packets (and return
    thereafter), otherwise, just keep trying to read.

    Raises DoneException if there is no more data to read.
    """
    if howmany is None:
        print "Sending command '%s' and listening"%command
    else:
        print "Sending command '%s' and listening for %d packets"%(command,
                                                                   howmany)
    sock.send(command)
    if howmany is None:
        while 1:
            read_next_packet(sock,file)
    else:
        try:
            for count in range(howmany):
                read_next_packet(sock,file)
        except DoneException:
            sys.stdout.write("\n")
            sys.stdout.write("Finished listening after %d packets"%count)
            raise DoneException
        except socket.error, val:
            print "socket.error:",val
            raise DoneException
    print

def main():
    global total_packets
    total_packets = 0
    host = "localhost"
    port = 8889
    stream = filename = None
    nonblock = 0

    argv = sys.argv[1:]
    if len(argv) == 0:
        print __doc__
        return
    while len(argv) > 0 and argv[0].startswith("-"):
        if argv[0] in ("-h", "-help", "--help"):
            print __doc__
            return
        elif argv[0] == "-host":
            host = argv[1]
            argv = argv[2:]
        elif argv[0] == "-port":
            port = int(argv[1])
            argv = argv[2:]
        elif argv[0] in ("-file", "-output"):
            filename = argv[1]
            argv = argv[2:]
        elif argv[0] in ("-nonblock"):
            nonblock = 1
            argv = argv[1:]
        else:
            print "Unexpected switch",argv[0]
            return

    commands = []
    if len(argv) == 0:
        print "No commands specified - assuming 'n'ormal play"
        commands = [("n",None)]

    command = None
    count   = 0
    for word in argv:
        if command:  # we have a command waiting for a count
            try:
                count = int(word)
            except:
                print "'%s' does not work as a count for command '%s'"%(word,command)
                return
            commands.append((command,count))
            command = None
        elif word in ("p", ">", "<"):  # commands that don't take a count
            commands.append((word,0))
            command = None
        elif word in ("q"):  # commands that read the rest of input
            commands.append((word,None))
            command = None
        elif word in ("n","F","f","r","R",
                      "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"):  # commands that do take a count
            command = word
        else:
            print "Unrecognised command '%s'"%word
    if command:
        commands.append((command,None))

    print "Commands:", commands

    sock = socket.socket(socket.AF_INET,socket.SOCK_STREAM)
    print "Connecting to %s on port %d"%(host,port)
    sock.connect((host,port))
    if filename:
        print "Writing output to file %s"%filename
        stream = file(filename,"wb")

    if nonblock:
        sock.setblocking(0)

    try:
        for command,count in commands:
            give_command(sock,command=command,file=stream,howmany=count)
    except (KeyboardInterrupt, DoneException):
        if stream:
            stream.close()
    sys.stdout.write("\n")
    sys.stdout.write("Total packets: %d\n"%total_packets)
    sock.close()



if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print
