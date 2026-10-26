#!/usr/bin/env sh
set -eu

x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -municode -mwindows -s -o udpping.exe client/udpping-gui.c -lws2_32
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -s -o udpping-cli.exe client/udpping-cli.c -lws2_32
