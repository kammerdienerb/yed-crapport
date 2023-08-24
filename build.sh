#!/usr/bin/env bash

gcc -o crapport.so crapport.c $(yed --print-cflags --print-ldflags) -g -O0
# gcc -o crapport.so crapport.c $(yed --print-cflags --print-ldflags) -g -O3
