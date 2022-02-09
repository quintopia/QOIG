# QOIG
Fast streaming PNG&lt;->QOI converter with some compression-improving extensions. Can achieve 1%-10% better compression than QOI without sacrificing performance.

## DEPENDS ON
libspng <https://github.com/randy408/libspng/> (tested on version 0.7.1) using miniz (https://github.com/richgel999/miniz)

## COMPILES LIKE
I use `gcc -O3 qoigconv.c -o qoigconv spng.o miniz.o -lm` where spng was compiled with the miniz compiler option, modified to let them live in the same source folder rather than installing miniz as a library. If you have miniz installed as library, this would look more like `gcc -O3 qoigconv.c -o qoigconv spng.o -lminiz -lm` (but don't quote me on the latter). I'm not providing a makefile because it's beyond the scope of this project to make it easy to compile with your preferred settings.

## GOALS
- Fast streaming converter supporting large file sizes. (I don't know how large this can do, but it should theoretically be able to handle images many gigabytes in size.)
- Adjustable parameters allowing you to choose your space/time tradeoff
- Full compatibility with QOI

## DETAILS
See qoig.h

## FULL RATIONALE
Coming later

## FUTURE STUFF?
- pre-populate near-match cache with all web-safe colors (or at least as many as do not collide)
- fastmod calculation for hash functions?
