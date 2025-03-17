#!/usr/bin/env bash

: ${CC=gcc}

CFLAGS="$CFLAGS -fPIC -Ilib/lite-xl/resources/include"
LDFLAGS=""

[[ "$@" == "clean" ]] && rm -f plugins/*/*.so plugins/*/*.dll && exit 0

[[ $OSTYPE != 'msys'* && $CC != *'mingw'* ]] && LDFLAGS="$LDFLAGS -lutil"

for d in plugins/*; do
  $CC $CFLAGS d/*.c $@ -shared -o d/native.so
done
