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

    try:
        (failures,tests) = doctest.testfile(filename,verbose=verbose)
        if failures > 0:
            print doctest.DIVIDER
    except:
        # e.g., Python 2.2
        tester = doctest.Tester(globs={},verbose=verbose)
        f = open(filename)
        (failures,tests) = tester.runstring(f.read(),filename)
        f.close()
        if failures > 0:
            print "*"*65

    testword = "test"
    if tests != 1: testword = "tests"
    failword = "failure"
    if failures != 1: failword = "failures"
    print "File %s: %d %s, %d %s"%(filename,tests,testword,failures,failword)

if __name__ == "__main__":
    main()
