#!/bin/sh
#

gcc -L/usr/local/lib/ -lpiface-1.0 -lsqlite3 -o nistkasten nistkasten.cpp
