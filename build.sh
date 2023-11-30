#!/usr/bin/env bash

# gcc -o crapport.so crapport.c $(yed --print-cflags --print-ldflags) -g -O0 -Wall -Wextra -Wno-null-pointer-subtraction -Wno-gnu-null-pointer-arithmetic
gcc -o crapport.so crapport.c $(yed --print-cflags --print-ldflags) -g -O3 -Wno-null-pointer-subtraction -Wno-gnu-null-pointer-arithmetic
