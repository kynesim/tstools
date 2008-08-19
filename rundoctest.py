#! /usr/bin/env python
"""Run the doctest on a text file

    Usage: doctext.py  [file]

[file] defaults to ``test.txt``
"""

import sys
import doctest

def main():
    args = sys.argv[1:]
    filename = None
    verbose = False

    for word in args:
        if word in ("-v", "-verbose"):
            verbose = True
        elif word in ("-h", "-help", "/?", "/help", "--help"):
            print __doc__
            return
        else:
            if filename:
                print "Filename '%s' already specified"%filename
                return
            else:
                filename = word

    if not filename:
        filename = "test.txt"

    (failures,tests) = doctest.testfile(filename,verbose=verbose)

    testword = "test"
    if tests != 1: testword = "tests"
    failword = "failure"
    if failures != 1: failword = "failures"
    print
    print "File %s: %d %s, %d %s"%(filename,tests,testword,failures,failword)

if __name__ == "__main__":
    main()
