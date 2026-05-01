#!/bin/bash
clang++-19 -c jit-arm-64.cpp -o jit.cpo -O2 -fomit-frame-pointer -Wall -std=c++17 -L../asmjit/build -lasmjit-l1vm -Wl,-z,relro,-z,now
