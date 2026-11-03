from sys import argv
from os.path import isfile

if __name__ == '__main__':
    if len(argv) != 2:
        exit('expected 1 argument')

    db_script_filename = argv[1]
    if not isfile(db_script_filename):
        exit('expected an existing filename as argument')

    content = open(db_script_filename, 'r').read()
    generate(parse(tokenize(content)))