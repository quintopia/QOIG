#include <stdlib.h>
//Fast lossless image compression and decompression based on QOI
//https://qoiformat.org/qoi-specification.pdf
/*While this can be use as fast streaming PNG<->QOI converter, that 
  is not the primary purpose of this bit of software. It's a proof
  of concept of some ideas to improve QOI's compression without
  sacrificing performance.
  
  The format here is slightly different, achieving slightly better
  compression across the board while keeping decoding as fast
  and making encoding tunable, allowing small amounts of extra
  time to be spent to get better compression.
  
  The extra compression is achieved by filling the huge gap between
  QOI's short byte codes (1 and 2 bytes) and its longest ones (4
  and 5 bytes). There are several new features to introduce new 2
  and 3 byte codes. The new features are:
  
  1. split cache with near-match section
  2. long runs
  3. secondary cache 
  4. raw blocks (rgb runs)

  1. SPLIT CACHE
  The first difference is that the cache can be split into two parts.
  The first part is like QOI's cache: it always contains the last
  n colors that have unique hashes under QOI's hash function.
  However, n can be any value in a subset of 0-64 now--any such number
  not divisible by 3, 5, or 7 to keep the hashing uniform.

  The remainder of the cache is mapped by a different function that
  tries to map similar colors to the same slot. When no exact match
  can be found in the first part of the cache, the encoder attempts
  to find an approximate match here. It will even, if allowed, try 
  to search this whole section of the cache for a similar color. If
  a color near enough to pattern a OP_DIFF or OP_LUMA off of is 
  found, an OP_INDEX is issued for that index, followed immediately
  by the OP_DIFF or OP_LUMA. Thus, sometimes an expensive 4-byte 
  OP_RGB or 5-byte OP_RGBA can be saved, replaced by an indexed 
  OP_DIFF (2 bytes) or indexed OP_LUMA (3 bytes). The decoder knows
  to expect the following OP to be applied to the color before 
  writing if the OP_INDEX points to this part of the cache.

  The optimal place to split the cache between these two parts depends
  on the content of the image being compressed, so the lower 5 bits of
  the fourth byte of the file are now used to encode this place (xored
  with 24). Setting the cache length parameter to 30 reproduces the 
  caching behavior of QOI.
  
  2. LONG RUNS
  Another optional compression technique is long runs, though it will
  almost never hurt to turn on--the best reason not to use it is to 
  maintain backwards compatibility with QOI. It will almost always 
  result in files no larger than would result without it and when it 
  saves, it can save a lot without affecting compression or decompress-
  -ion speed at all. Here's the spec:
  
  Any time a run of length 62 is emitted, the next byte will encode a
  long run. If the value of this byte is less than 128, it encodes a 
  run of that length. Otherwise, it is concatenated with the next byte
  after that to make a 16-bit (value) encoding a run of length
  (value-2^15+128). Thus, runs of length up to 2^15+127+62 can be
  encoded using only 3 bytes. This especially helps with transparent
  backgrounds. The only time this scheme can compress to more bytes than
  QOI is when the run length is exactly 62.
  
  ┌─ OP_LONG_RUN ──────────┬────────────────────────┬────────────────────────┐
  │         Byte[0]        │         Byte[1]        │  Byte[2] (iff e == 1)  │
  │ 7  6  5  4  3  2  1  0 │ 7  6  5  4  3  2  1  0 │ 7  6  5  4  3  2  1  0 │
  │────────────────────────┼───┼────────────────────┼────────────────────────│
  │ 1  1  1  1  1  1  0  1 │ e │          run       │   run (lower 8 bits)   │
  └────────────────────────┴───┴────────────────────┴────────────────────────┘
  
  The most significant bit of the fourth byte of the file is set to enable this
  feature.
  
  3. BACKUP CACHES
  The main 64 color cache is rather small and colors that could be useful later
  are frequently ejected. So, an optional (and usually helpful) compression 
  feature in QOIG is a pair of secondary 256 color caches, and a modification of 
  OP_INDEX to address them. The first is for exact matches and the second for near
  matches usable by indexed DIFFs and indexed LUMAs. When long indexing is enabled,
  the longest exact match cache length (30) is unavailable.
  
  ┌─ OP_LONG_INDEX ────────┬────────────────────────┐
  │         Byte[0]        │         Byte[1]        │
  │ 7  6  5  4  3  2  1  0 │ 7  6  5  4  3  2  1  0 │
  │────────────────────┼───┼────────────────────────│
  │ 0  0  1  1  1  1  1│ c │   index into cache c   │
  └────────────────────┴───┴────────────────────────┘
  
  The second most significant bit of the fourth byte of the file is set to DISable
  this feature.
  
  NB: When cache length parameter is set to 30 and longruns and longindex are both
  disabled, the fourth byte of the file will be "f", exactly as in plain QOI, thus
  ensuring all QOI files are valid QOIG files.
  
  4. RAW BLOCKS (RGB/RGBA RUNS)
  This extension replaces runs of consecutive OP_RGB or OP_RGBA with a new opcode,
  OP_RGBRUN, which marks off an entire block of the encoding as raw RGB or RGBA color
  data. The byte following this new opcode indicates whether the alpha data is included
  and how many colors the block contains, up to 129 (encoded with a bias of 2). 
  Immediately following these two bytes is a run of data so described containing only 
  pixel colors 3 or 4 bytes at a time with no intervening byte codes. The two byte 
  marker at the beginning of this block looks like this:
  
  ┌─ OP_RGBRUN ────────────┬────────────────────────┐
  │         Byte[0]        │         Byte[1]        │
  │ 7  6  5  4  3  2  1  0 │ 7  6  5  4  3  2  1  0 │
  │────────────────────────┼───┼────────────────────│
  │ 0  1  1  0  1  0  1  0 │ t │  length of run - 2 │
  └────────────────────────┴───┴────────────────────┘
  
  The third most significant bit of the fourth byte of the file is set to DISable 
  this feature.
  */
#include <string.h>
#include <arpa/inet.h>

//Uses libspng with miniz
#define SPNG_STATIC
#define SPNG_USE_MINIZ
#include "spng.h"

#define QOIG_SRBG 0
#define QOIG_CACHES {0,1,2,4,8,\
                     11,13,16,17,19,\
                     22,23,26,29,31,\
                     32,34,37,38,41,\
                     43,44,46,47,52,\
                     53,58,59,61,62,\
                     64}
#define IS_BIG_ENDIAN ((color){ .rgba = 1 }.alpha)
#define OP_RGB (uint8_t)0xFE
#define OP_RGBA (uint8_t)0xFF
#define OP_RGBRUN (uint8_t)0x6A
#define OP_INDEX (uint8_t)0
#define OP_DIFF (uint8_t)0x40
#define OP_LUMA (uint8_t)0x80
#define OP_RUN (uint8_t)0xC0
#define OP_CODE (uint8_t)0xC0
#define OP_ARGS (uint8_t)0x3F
#define OP_LUMA_ARG (uint8_t)0x3F
#define OP_INDEX_ARG (uint8_t)0x3F
#define HASH(C,H) ((C.red*3+C.green*5+C.blue*7+C.alpha*11)%H)
#define LHASH(C) ((23*C.red+29*C.green+59*C.blue+197*C.alpha)&0xFF)
#define LRS(a,b) ((unsigned)(a)>>b)
#define LOCALHASH(C,H,L) (H+(LRS(C.red+8,3)*37+LRS(C.green+8,3)*59+\
                     LRS(C.blue+8,3)*67)%(L-H))
#define TUBITRANGE(a,b) ((char)(a-b)>-3 && (char)(a-b)<2)
#define COLORRANGES(a,b) TUBITRANGE(a.red,b.red) && \
                         TUBITRANGE(a.green,b.green) && \
                         TUBITRANGE(a.blue,b.blue)
#define EQCOLOR(a,b) (a.red == b.red && a.green == b.green && \
                       a.blue == b.blue && a.alpha == b.alpha)
#define QOIG_PRINT(b) if (bufferedrgb && !rgbrun) {\
                          if (!cfg.simulate) {\
                              fprintf(outfile,"%c",bufferedrgb);\
                              fwrite(&last,1,3+(bufferedrgb&1),outfile);\
                          }\
                          ct+=4+(bufferedrgb&1);\
                          bufferedrgb = 0;\
                      } else if (rgbrun) {\
                          if (!cfg.simulate) {\
                              fprintf(outfile,"%c%c",OP_RGBRUN,rgbrun-2|(bufferedrgb&1)<<7);\
                              fwrite(rgbbuffer,1,rgbrun*(bufferedrgb-0xFB),outfile);\
                          }\
                          ct+=2+rgbrun*(bufferedrgb-0xFB);\
                          rgbrun=0;\
                          bufferedrgb=0;\
                      }\
                      if (!cfg.simulate) fprintf(outfile,"%c",b);\
                      ct++
#define QOIG_READ(a,b,c,d) if (fread(a,b,c,d)!=c) return -1

typedef union {
    uint32_t rgba;
    struct {
        unsigned char red;
        unsigned char green;
        unsigned char blue;
        unsigned char alpha;
    };
} color;

typedef struct {
	uint32_t width;
	uint32_t height;
	uint8_t channels;
	uint8_t colorspace;
} qoig_desc;

typedef struct {
    unsigned int bytecap;
    unsigned char longruns;
    unsigned char searchcache;
    unsigned char clen;
    unsigned char simulate;
    unsigned char channels;
    unsigned char longindex;
    unsigned char rawblocks;
} qoig_cfg;
static color default_colors_be[256] = {
0x0000ffff,0xffcc33ff,0x003300ff,0x66cc66ff,0x993399ff,0xffccffff,0x0033ccff,0xffff00ff,
0x838383ff,0x66ff33ff,0x996666ff,0xffffccff,0x006699ff,0x66ffffff,0xddddddff,0x6c6c6cff,
0x999933ff,0xcc0066ff,0x009966ff,0x330099ff,0x9999ffff,0xc6c6c6ff,0x99cc00ff,0xcc3333ff,
0x00cc33ff,0x333366ff,0x99ccccff,0xcc33ffff,0x00ccffff,0xcc6600ff,0x00ff00ff,0x336633ff,
0x99ff99ff,0xcc66ccff,0x00ffccff,0x3366ffff,0xff0000ff,0x339900ff,0x660033ff,0xcc9999ff,
0xff00ccff,0x3399ccff,0x6600ffff,0x101010ff,0x663300ff,0xcccc66ff,0xff3399ff,0x33cc99ff,
0x6633ccff,0x6a6a6aff,0xf9f9f9ff,0xccff33ff,0xff6666ff,0x33ff66ff,0x666699ff,0xccffffff,
0x535353ff,0xe2e2e2ff,0xff9933ff,0x000000ff,0x669966ff,0x990099ff,0xff99ffff,0x0000ccff,
0xffcc00ff,0x5a5a5aff,0x66cc33ff,0x993366ff,0xffccccff,0x003399ff,0x66ccffff,0xb4b4b4ff,
0x66ff00ff,0x996633ff,0xffff99ff,0x006666ff,0x66ffccff,0x9966ffff,0x9d9d9dff,0x999900ff,
0xcc0033ff,0x009933ff,0x330066ff,0x9999ccff,0xcc00ffff,0x0099ffff,0xcc3300ff,0x00cc00ff,
0x333333ff,0x99cc99ff,0xcc33ccff,0x00ccccff,0x3333ffff,0xfefefeff,0x336600ff,0x99ff66ff,
0xcc6699ff,0x00ff99ff,0x3366ccff,0x585858ff,0xe7e7e7ff,0x660000ff,0xcc9966ff,0xff0099ff,
0x339999ff,0x6600ccff,0x414141ff,0xd0d0d0ff,0xcccc33ff,0xff3366ff,0x33cc66ff,0x663399ff,
0xccccffff,0x2a2a2aff,0xccff00ff,0xff6633ff,0x33ff33ff,0x666666ff,0xccffccff,0xff66ffff,
0x33ffffff,0xff9900ff,0x313131ff,0x669933ff,0x990066ff,0xff99ccff,0x000099ff,0x6699ffff,
0x8b8b8bff,0x66cc00ff,0x993333ff,0xffcc99ff,0x003366ff,0x66ccccff,0x9933ffff,0x747474ff,
0x996600ff,0xffff66ff,0x006633ff,0x66ff99ff,0x9966ccff,0xcececeff,0x0066ffff,0xcc0000ff,
0x009900ff,0x330033ff,0x999999ff,0xcc00ccff,0x0099ccff,0x3300ffff,0xd5d5d5ff,0x333300ff,
0x99cc66ff,0xcc3399ff,0x00cc99ff,0x3333ccff,0x2f2f2fff,0xbebebeff,0x99ff33ff,0xcc6666ff,
0x00ff66ff,0x336699ff,0x99ffffff,0x181818ff,0xa7a7a7ff,0xcc9933ff,0xff0066ff,0x339966ff,
0x660099ff,0xcc99ffff,0x010101ff,0xcccc00ff,0xff3333ff,0x33cc33ff,0x663366ff,0xccccccff,
0xff33ffff,0x33ccffff,0xff6600ff,0x33ff00ff,0x666633ff,0xccff99ff,0xff66ccff,0x33ffccff,
0x6666ffff,0x626262ff,0x669900ff,0x990033ff,0xff9999ff,0x000066ff,0x6699ccff,0x9900ffff,
0x4b4b4bff,0x993300ff,0xffcc66ff,0x003333ff,0x66cc99ff,0x9933ccff,0xa5a5a5ff,0x0033ffff,
0xffff33ff,0x006600ff,0x66ff66ff,0x996699ff,0xffffffff,0x0066ccff,0x1d1d1dff,0xacacacff,
0x330000ff,0x999966ff,0xcc0099ff,0x009999ff,0x3300ccff,0x060606ff,0x959595ff,0x99cc33ff,
0xcc3366ff,0x00cc66ff,0x333399ff,0x99ccffff,0xefefefff,0x99ff00ff,0xcc6633ff,0x00ff33ff,
0x336666ff,0x99ffccff,0xcc66ffff,0x00ffffff,0xcc9900ff,0xff0033ff,0x339933ff,0x660066ff,
0xcc99ccff,0xff00ffff,0x3399ffff,0xff3300ff,0x33cc00ff,0x663333ff,0xcccc99ff,0xff33ccff,
0x33ccccff,0x6633ffff,0x393939ff,0x666600ff,0xccff66ff,0xff6699ff,0x33ff99ff,0x6666ccff,
0x939393ff,0x222222ff,0x990000ff,0xff9966ff,0x000033ff,0x669999ff,0x9900ccff,0x7c7c7cff};
static color default_colors_le[256] = {
0xffff0000,0xff33ccff,0xff003300,0xff66cc66,0xff993399,0xffffccff,0xffcc3300,0xff00ffff,
0xff838383,0xff33ff66,0xff666699,0xffccffff,0xff996600,0xffffff66,0xffdddddd,0xff6c6c6c,
0xff339999,0xff6600cc,0xff669900,0xff990033,0xffff9999,0xffc6c6c6,0xff00cc99,0xff3333cc,
0xff33cc00,0xff663333,0xffcccc99,0xffff33cc,0xffffcc00,0xff0066cc,0xff00ff00,0xff336633,
0xff99ff99,0xffcc66cc,0xffccff00,0xffff6633,0xff0000ff,0xff009933,0xff330066,0xff9999cc,
0xffcc00ff,0xffcc9933,0xffff0066,0xff101010,0xff003366,0xff66cccc,0xff9933ff,0xff99cc33,
0xffcc3366,0xff6a6a6a,0xfff9f9f9,0xff33ffcc,0xff6666ff,0xff66ff33,0xff996666,0xffffffcc,
0xff535353,0xffe2e2e2,0xff3399ff,0xff000000,0xff669966,0xff990099,0xffff99ff,0xffcc0000,
0xff00ccff,0xff5a5a5a,0xff33cc66,0xff663399,0xffccccff,0xff993300,0xffffcc66,0xffb4b4b4,
0xff00ff66,0xff336699,0xff99ffff,0xff666600,0xffccff66,0xffff6699,0xff9d9d9d,0xff009999,
0xff3300cc,0xff339900,0xff660033,0xffcc9999,0xffff00cc,0xffff9900,0xff0033cc,0xff00cc00,
0xff333333,0xff99cc99,0xffcc33cc,0xffcccc00,0xffff3333,0xfffefefe,0xff006633,0xff66ff99,
0xff9966cc,0xff99ff00,0xffcc6633,0xff585858,0xffe7e7e7,0xff000066,0xff6699cc,0xff9900ff,
0xff999933,0xffcc0066,0xff414141,0xffd0d0d0,0xff33cccc,0xff6633ff,0xff66cc33,0xff993366,
0xffffcccc,0xff2a2a2a,0xff00ffcc,0xff3366ff,0xff33ff33,0xff666666,0xffccffcc,0xffff66ff,
0xffffff33,0xff0099ff,0xff313131,0xff339966,0xff660099,0xffcc99ff,0xff990000,0xffff9966,
0xff8b8b8b,0xff00cc66,0xff333399,0xff99ccff,0xff663300,0xffcccc66,0xffff3399,0xff747474,
0xff006699,0xff66ffff,0xff336600,0xff99ff66,0xffcc6699,0xffcecece,0xffff6600,0xff0000cc,
0xff009900,0xff330033,0xff999999,0xffcc00cc,0xffcc9900,0xffff0033,0xffd5d5d5,0xff003333,
0xff66cc99,0xff9933cc,0xff99cc00,0xffcc3333,0xff2f2f2f,0xffbebebe,0xff33ff99,0xff6666cc,
0xff66ff00,0xff996633,0xffffff99,0xff181818,0xffa7a7a7,0xff3399cc,0xff6600ff,0xff669933,
0xff990066,0xffff99cc,0xff010101,0xff00cccc,0xff3333ff,0xff33cc33,0xff663366,0xffcccccc,
0xffff33ff,0xffffcc33,0xff0066ff,0xff00ff33,0xff336666,0xff99ffcc,0xffcc66ff,0xffccff33,
0xffff6666,0xff626262,0xff009966,0xff330099,0xff9999ff,0xff660000,0xffcc9966,0xffff0099,
0xff4b4b4b,0xff003399,0xff66ccff,0xff333300,0xff99cc66,0xffcc3399,0xffa5a5a5,0xffff3300,
0xff33ffff,0xff006600,0xff66ff66,0xff996699,0xffffffff,0xffcc6600,0xff1d1d1d,0xffacacac,
0xff000033,0xff669999,0xff9900cc,0xff999900,0xffcc0033,0xff060606,0xff959595,0xff33cc99,
0xff6633cc,0xff66cc00,0xff993333,0xffffcc99,0xffefefef,0xff00ff99,0xff3366cc,0xff33ff00,
0xff666633,0xffccff99,0xffff66cc,0xffffff00,0xff0099cc,0xff3300ff,0xff339933,0xff660066,
0xffcc99cc,0xffff00ff,0xffff9933,0xff0033ff,0xff00cc33,0xff333366,0xff99cccc,0xffcc33ff,
0xffcccc33,0xffff3366,0xff393939,0xff006666,0xff66ffcc,0xff9966ff,0xff99ff33,0xffcc6666,
0xff939393,0xff222222,0xff000099,0xff6699ff,0xff330000,0xff999966,0xffcc0099,0xff7c7c7c};
static color default_colors2_be[256] = {
0x3333ffff,0x545454ff,0xacacacff,0xcccc00ff,0xcc6600ff,0xffcc66ff,0xff6666ff,0x333366ff,
0x585858ff,0x636363ff,0xff99ccff,0xff33ccff,0x3300ccff,0x8f8f8fff,0x9a9a9aff,0x66ffccff,
0xb0b0b0ff,0xff9933ff,0xff3333ff,0x330033ff,0xdcdcdcff,0xe7e7e7ff,0x66ff33ff,0xff0099ff,
0x3c3c3cff,0x99ff33ff,0xecececff,0x66cc99ff,0x666699ff,0x3f3f3fff,0xff0000ff,0x996699ff,
0xccccffff,0xcc66ffff,0x66cc00ff,0x666600ff,0x8c8c8cff,0x99cc00ff,0x996600ff,0xcccc66ff,
0xcc6666ff,0x003366ff,0xcececeff,0xd9d9d9ff,0xcc99ccff,0xcc33ccff,0x0000ccff,0x242424ff,
0x7c7c7cff,0x33ffccff,0x262626ff,0xcc9933ff,0xcc3333ff,0x000033ff,0x525252ff,0x5d5d5dff,
0x33ff33ff,0xcc0099ff,0x7e7e7eff,0xff00ffff,0xffff99ff,0x33cc99ff,0x336699ff,0x66ccffff,
0xcc0000ff,0xcbcbcbff,0xff0066ff,0xffff00ff,0x33cc00ff,0x336600ff,0x66cc66ff,0x666666ff,
0xbcbcbcff,0x99cc66ff,0x996666ff,0x6699ccff,0x6633ccff,0x4f4f4fff,0x9999ccff,0x9933ccff,
0x707070ff,0x7b7b7bff,0x669933ff,0x663333ff,0x9c9c9cff,0x999933ff,0x993333ff,0xbdbdbdff,
0x660099ff,0xd3d3d3ff,0x00ff33ff,0x990099ff,0xf4f4f4ff,0xcc00ffff,0xccff99ff,0x660000ff,
0xffffffff,0x33ccffff,0x990000ff,0x414141ff,0xcc0066ff,0xccff00ff,0x00cc00ff,0xffff66ff,
0x33cc66ff,0x336666ff,0x8e8e8eff,0x999999ff,0xffccccff,0xff66ccff,0x3333ccff,0xc5c5c5ff,
0xd0d0d0ff,0xdbdbdbff,0xe6e6e6ff,0xffcc33ff,0xff6633ff,0x333333ff,0x8c8c8cff,0xe4e4e4ff,
0xff9999ff,0xff3399ff,0x330099ff,0x494949ff,0x6600ffff,0x66ff99ff,0x6a6a6aff,0xff9900ff,
0xff3300ff,0x330000ff,0xccffffff,0x660066ff,0x66ff00ff,0xb7b7b7ff,0x990066ff,0x99ff00ff,
0xd8d8d8ff,0xccff66ff,0x00cc66ff,0x006666ff,0x1c1c1cff,0x747474ff,0xccccccff,0xcc66ccff,
0x0033ccff,0x3b3b3bff,0x464646ff,0x515151ff,0x5c5c5cff,0xcccc33ff,0xcc6633ff,0x003333ff,
0x888888ff,0x939393ff,0xcc9999ff,0xcc3399ff,0xff99ffff,0xff33ffff,0x3300ffff,0x33ff99ff,
0xe0e0e0ff,0xcc9900ff,0xcc3300ff,0xff9966ff,0xff3366ff,0x330066ff,0x33ff00ff,0x2d2d2dff,
0x66ff66ff,0xff00ccff,0x4e4e4eff,0x99ff66ff,0x646464ff,0x66ccccff,0x6666ccff,0x858585ff,
0xff0033ff,0x9966ccff,0xa6a6a6ff,0xb1b1b1ff,0x66cc33ff,0x666633ff,0xd2d2d2ff,0x99cc33ff,
0x996633ff,0x669999ff,0x663399ff,0x444444ff,0x999999ff,0x993399ff,0xcc99ffff,0xcc33ffff,
0x669900ff,0x663300ff,0x565656ff,0x999900ff,0x993300ff,0xcc9966ff,0xcc3366ff,0x000066ff,
0x00ff00ff,0xa3a3a3ff,0x33ff66ff,0xcc00ccff,0xc4c4c4ff,0xcfcfcfff,0xffffccff,0x33ccccff,
0x3366ccff,0xfbfbfbff,0xcc0033ff,0x848484ff,0xdcdcdcff,0xffff33ff,0x33cc33ff,0x336633ff,
0x484848ff,0x535353ff,0xffcc99ff,0xff6699ff,0x333399ff,0x6699ffff,0x6633ffff,0x959595ff,
0x9999ffff,0xffcc00ff,0xff6600ff,0x333300ff,0x669966ff,0x663366ff,0xe2e2e2ff,0x999966ff,
0x993366ff,0x141414ff,0x6600ccff,0xc4c4c4ff,0x00ff66ff,0x9900ccff,0x3a3a3aff,0x454545ff,
0xccffccff,0x660033ff,0x0066ccff,0x717171ff,0x990033ff,0x878787ff,0x929292ff,0xccff33ff,
0x00cc33ff,0x006633ff,0xbebebeff,0xc9c9c9ff,0xcccc99ff,0xcc6699ff,0xffccffff,0xff66ffff};
static color default_colors2_le[256] = {
0xffff3333,0xff545454,0xffacacac,0xff00cccc,0xff0066cc,0xff66ccff,0xff6666ff,0xff663333,
0xff585858,0xff636363,0xffcc99ff,0xffcc33ff,0xffcc0033,0xff8f8f8f,0xff9a9a9a,0xffccff66,
0xffb0b0b0,0xff3399ff,0xff3333ff,0xff330033,0xffdcdcdc,0xffe7e7e7,0xff33ff66,0xff9900ff,
0xff3c3c3c,0xff33ff99,0xffececec,0xff99cc66,0xff996666,0xff3f3f3f,0xff0000ff,0xff996699,
0xffffcccc,0xffff66cc,0xff00cc66,0xff006666,0xff8c8c8c,0xff00cc99,0xff006699,0xff66cccc,
0xff6666cc,0xff663300,0xffcecece,0xffd9d9d9,0xffcc99cc,0xffcc33cc,0xffcc0000,0xff242424,
0xff7c7c7c,0xffccff33,0xff262626,0xff3399cc,0xff3333cc,0xff330000,0xff525252,0xff5d5d5d,
0xff33ff33,0xff9900cc,0xff7e7e7e,0xffff00ff,0xff99ffff,0xff99cc33,0xff996633,0xffffcc66,
0xff0000cc,0xffcbcbcb,0xff6600ff,0xff00ffff,0xff00cc33,0xff006633,0xff66cc66,0xff666666,
0xffbcbcbc,0xff66cc99,0xff666699,0xffcc9966,0xffcc3366,0xff4f4f4f,0xffcc9999,0xffcc3399,
0xff707070,0xff7b7b7b,0xff339966,0xff333366,0xff9c9c9c,0xff339999,0xff333399,0xffbdbdbd,
0xff990066,0xffd3d3d3,0xff33ff00,0xff990099,0xfff4f4f4,0xffff00cc,0xff99ffcc,0xff000066,
0xffffffff,0xffffcc33,0xff000099,0xff414141,0xff6600cc,0xff00ffcc,0xff00cc00,0xff66ffff,
0xff66cc33,0xff666633,0xff8e8e8e,0xff999999,0xffccccff,0xffcc66ff,0xffcc3333,0xffc5c5c5,
0xffd0d0d0,0xffdbdbdb,0xffe6e6e6,0xff33ccff,0xff3366ff,0xff333333,0xff8c8c8c,0xffe4e4e4,
0xff9999ff,0xff9933ff,0xff990033,0xff494949,0xffff0066,0xff99ff66,0xff6a6a6a,0xff0099ff,
0xff0033ff,0xff000033,0xffffffcc,0xff660066,0xff00ff66,0xffb7b7b7,0xff660099,0xff00ff99,
0xffd8d8d8,0xff66ffcc,0xff66cc00,0xff666600,0xff1c1c1c,0xff747474,0xffcccccc,0xffcc66cc,
0xffcc3300,0xff3b3b3b,0xff464646,0xff515151,0xff5c5c5c,0xff33cccc,0xff3366cc,0xff333300,
0xff888888,0xff939393,0xff9999cc,0xff9933cc,0xffff99ff,0xffff33ff,0xffff0033,0xff99ff33,
0xffe0e0e0,0xff0099cc,0xff0033cc,0xff6699ff,0xff6633ff,0xff660033,0xff00ff33,0xff2d2d2d,
0xff66ff66,0xffcc00ff,0xff4e4e4e,0xff66ff99,0xff646464,0xffcccc66,0xffcc6666,0xff858585,
0xff3300ff,0xffcc6699,0xffa6a6a6,0xffb1b1b1,0xff33cc66,0xff336666,0xffd2d2d2,0xff33cc99,
0xff336699,0xff999966,0xff993366,0xff444444,0xff999999,0xff993399,0xffff99cc,0xffff33cc,
0xff009966,0xff003366,0xff565656,0xff009999,0xff003399,0xff6699cc,0xff6633cc,0xff660000,
0xff00ff00,0xffa3a3a3,0xff66ff33,0xffcc00cc,0xffc4c4c4,0xffcfcfcf,0xffccffff,0xffcccc33,
0xffcc6633,0xfffbfbfb,0xff3300cc,0xff848484,0xffdcdcdc,0xff33ffff,0xff33cc33,0xff336633,
0xff484848,0xff535353,0xff99ccff,0xff9966ff,0xff993333,0xffff9966,0xffff3366,0xff959595,
0xffff9999,0xff00ccff,0xff0066ff,0xff003333,0xff669966,0xff663366,0xffe2e2e2,0xff669999,
0xff663399,0xff141414,0xffcc0066,0xffc4c4c4,0xff66ff00,0xffcc0099,0xff3a3a3a,0xff454545,
0xffccffcc,0xff330066,0xffcc6600,0xff717171,0xff330099,0xff878787,0xff929292,0xff33ffcc,
0xff33cc00,0xff336600,0xffbebebe,0xffc9c9c9,0xff99cccc,0xff9966cc,0xffffccff,0xffff66ff};


int qoig_encode(spng_ctx *ctx, size_t width, FILE *outfile, unsigned long *outlen, qoig_cfg cfg) {
    color cache[64] = {0};
    color longcache1[256];
    color longcache2[256];
    color row[width];
    uint8_t rgbbuffer[516];
    color last;
    color current = (color){.alpha=255};
    color temp,temp2;
    int i,j;
    char k,l;
    uint8_t m;
    uint8_t lastrow = 0;
    uint8_t ret,done;
    uint8_t bufferedrgb = 0;
    uint8_t rgbrun = 0;
    uint32_t run = 0;
    unsigned long ct = 0;
    int rows_read = 0;
    uint8_t colorhash,lcolorhash;
    int cachelengths[31] = QOIG_CACHES;
    int clen;
    
    clen = cachelengths[cfg.clen];
    if (cfg.longindex) {
        if (IS_BIG_ENDIAN) {
			memcpy(longcache1,default_colors_be,256*sizeof(color));
            memcpy(longcache2,default_colors2_be,256*sizeof(color));
        } else {
			memcpy(longcache1,default_colors_le,256*sizeof(color));
            memcpy(longcache2,default_colors2_le,256*sizeof(color));
        }
    }
    if (clen) {
        cache[HASH(current,clen)] = current;
        if (cfg.longindex) longcache1[LHASH(current)] = current;
    }
    /*spng_decode_row is a bad API. a sane API would return 0 after every successful read*/
    while (!(ret = spng_decode_row(ctx, row, 4*width)) || lastrow && ret == SPNG_EOI || run) {
        done = ret == SPNG_EOI && !lastrow;
        lastrow = !ret;
        for (i=0;i<width&&(!cfg.bytecap||i+width*rows_read<cfg.bytecap);i+=1) {
            
            last = current;
            
            //Get next pixel

            current=row[i];

            //Try to make run
            if (!done && EQCOLOR(current,last) && (run<62 || cfg.longruns && run < 32957)) {
                run++;
                continue;
            }
            if (run) {
                if (run <= 62 - cfg.longruns) {
                    QOIG_PRINT(OP_RUN|(run-1));
                } else {
                    QOIG_PRINT(OP_RUN|61);
                    run-=62;
                    if (run < 128) {
                        QOIG_PRINT(run);
                    } else {
                        run-=128;
                        QOIG_PRINT(0x80|LRS(run,8));
                        QOIG_PRINT(0xFF&run);
                    }
                }
                run = 0;
                if (done) break;
                if (EQCOLOR(current,last)) {
                    run++;
                    continue;
                }
            }
            
            

            if (clen) {
                //Try to make exact index into cache
                colorhash = HASH(current,clen);
                temp = cache[colorhash];
                if (EQCOLOR(current,temp)) {
                    QOIG_PRINT(OP_INDEX|colorhash&OP_INDEX_ARG);
                    continue;
                }

                cache[colorhash] = current;
                if (cfg.longindex) {
                    lcolorhash = LHASH(current);
                    temp2 = longcache1[lcolorhash];
                    longcache1[LHASH(temp)] = temp;
                    if (EQCOLOR(current,temp2)) {
                        QOIG_PRINT(OP_INDEX|62&OP_INDEX_ARG);
                        QOIG_PRINT(lcolorhash);
                        continue;
                    }
                }
            }
            
            //Try to make exact diff with previous pixel
            if (COLORRANGES(current,last) &&
                current.alpha == last.alpha) {
                QOIG_PRINT(OP_DIFF|(current.red-last.red+2&3)<<4|
                                    (current.green-last.green+2&3)<<2|
                                        (current.blue-last.blue+2&3));
                continue;
            }


            //Try to make luma diff with previous pixel
            j = current.green-last.green;
            if (j>-33 && j<32 && current.alpha == last.alpha) {
                k = current.red-last.red-j;
                l = current.blue-last.blue-j;
                if (-9<k && -9<l && k<8 && l<8) {
                    QOIG_PRINT(OP_LUMA|(j+32&OP_LUMA_ARG));
                    QOIG_PRINT((k+8&15)<<4|l+8&15);
                    continue;
                }
            }
            if (64-clen-2*cfg.longindex) {
                //Try to make diff index into cache
                colorhash=m=LOCALHASH(current,clen,64-2*cfg.longindex);
                temp = cache[m];
                if (COLORRANGES(current,temp) &&
                    current.alpha == temp.alpha) {
                    smalldiff:QOIG_PRINT(OP_INDEX|m&OP_INDEX_ARG);
                    QOIG_PRINT(OP_DIFF|(current.red-temp.red+2&3)<<4|
                                            (current.green-temp.green+2&3)<<2|
                                            (current.blue-temp.blue+2&3));
                    continue;
                }
                
                //Next just search the entire cache for the nearest color
                if (cfg.searchcache) {
                    for (j=clen;j<64-2*cfg.longindex;j++) {
                        temp2 = cache[j];
                        if (COLORRANGES(current,temp2) && current.alpha == temp2.alpha) {
                            temp = temp2;
                            m=j;
                            goto smalldiff;
                        }
                        k = current.green - temp2.green;
                        if (k>-33 && k<32) {
                            k = current.red-temp.red-j;
                            l = current.blue-temp.blue-j;
                            if (-9<k && -9<l && k<8 && l<8) {
                                temp = temp2;
                                m = j;
                            }
                        }
                    }
                }

                //Try to make luma index into cache
                j = current.green-temp.green;
                if (j>-33 && j<32 && current.alpha == temp.alpha) {
                    k = current.red-temp.red-j;
                    l = current.blue-temp.blue-j;
                    if (-9<k && -9<l && k<8 && l<8) {
                        QOIG_PRINT(OP_INDEX|m&OP_INDEX_ARG);
                        QOIG_PRINT(OP_LUMA|j+32&0x3F);
                        QOIG_PRINT((k+8&15)<<4|l+8&15);
                        continue;
                    }
                }
                //if we are buffering an RGB block, interrupting that to insert an long-indexed diff can cost an extra byte
                if (cfg.longindex && !(rgbrun && bufferedrgb==OP_RGB && current.alpha==last.alpha)) {
                    //Try to make diff index into cache
                    m=LOCALHASH(current,0,256);
                    temp = longcache2[m];
                    if (COLORRANGES(current,temp) &&
                        current.alpha == temp.alpha) {
                        lsmalldiff:QOIG_PRINT(OP_INDEX|63&OP_INDEX_ARG);
                        QOIG_PRINT(m);
                        QOIG_PRINT(OP_DIFF|(current.red-temp.red+2&3)<<4|
                                                (current.green-temp.green+2&3)<<2|
                                                (current.blue-temp.blue+2&3));
                        continue;
                    }
                    
                    //Next just search the entire cache for the nearest color
                    if (cfg.searchcache) {
                        for (j=0;j<256;j++) {
                            temp2 = longcache2[j];
                            if (COLORRANGES(current,temp2) && current.alpha == temp2.alpha) {
                                temp = temp2;
                                m=j;
                                goto lsmalldiff;
                            }
                            if (current.alpha != last.alpha) {
                                k = current.green - temp2.green;
                                if (k>-33 && k<32) {
                                    k = current.red-temp.red-j;
                                    l = current.blue-temp.blue-j;
                                    if (-9<k && -9<l && k<8 && l<8) {
                                        temp = temp2;
                                        m = j;
                                    }
                                }
                            }
                        }
                    }
                    
                    //Try to make luma index into cache
                    //There are no savings here if current alpha matches previous,
                    //and it's faster to just use an OP_RGB
                    //Likewise, interrupting an rgbrun for a long-indexed luma can cost an extra byte
                    if (current.alpha != last.alpha && !rgbrun) {
                        j = current.green-temp.green;
                        if (j>-33 && j<32 && current.alpha == temp.alpha) {
                            k = current.red-temp.red-j;
                            l = current.blue-temp.blue-j;
                            if (-9<k && -9<l && k<8 && l<8) {
                                QOIG_PRINT(OP_INDEX|63&OP_INDEX_ARG);
                                QOIG_PRINT(m);
                                QOIG_PRINT(OP_LUMA|j+32&0x3F);
                                QOIG_PRINT((k+8&15)<<4|l+8&15);
                                continue;
                            }
                        }
                    }
                }
            }

            //Try to make RGB or RGBA pixel
            //If we're buffering a pixel write, switch to raw mode and write it
            if (cfg.rawblocks) {
                if (rgbrun==129 || rgbrun && (bufferedrgb == OP_RGB && current.alpha!=last.alpha ||
                    bufferedrgb == OP_RGBA && current.alpha==last.alpha)) {
                    if (!cfg.simulate) {
                        fprintf(outfile,"%c%c",OP_RGBRUN,rgbrun-2|(bufferedrgb&1)<<7);
                        fwrite(rgbbuffer,1,rgbrun*(bufferedrgb-0xFB),outfile);
                    }
                    ct+=rgbrun*(bufferedrgb-0xFB);
                    rgbrun=0;
                    bufferedrgb = 0;
                }
                if (bufferedrgb||rgbrun) {
                    if (bufferedrgb == OP_RGB && current.alpha!=last.alpha) {
                        bufferedrgb = 0;
                        QOIG_PRINT(OP_RGB);
                        if (!cfg.simulate) {
                            fwrite(&last,1,3,outfile);
                        }
                        ct+=3;
                        bufferedrgb = OP_RGBA;
                    } else {
                        if (!rgbrun) {
                            memcpy(rgbbuffer,&last,3+(bufferedrgb&1));
                            rgbrun=1;
                        }
                        memcpy(rgbbuffer+(3+(bufferedrgb&1))*rgbrun,&current,3+(bufferedrgb&1));
                        rgbrun++;
                    }
                } else {
                    if (current.alpha == last.alpha) {
                        bufferedrgb = OP_RGB;
                    } else {
                        bufferedrgb = OP_RGBA;
                    }
                }
            } else {
                if (current.alpha == last.alpha) {
                    QOIG_PRINT(OP_RGB);
                    j=3;
                } else {
                    QOIG_PRINT(OP_RGBA);
                    j=4;
                }
                if (!cfg.simulate) {
                    fwrite(&current,1,j,outfile);
                }
                ct+=j;
            }
            if (64-clen-2*cfg.longindex) {
                if (cfg.longindex) {
                    temp = cache[colorhash];
                    if (!EQCOLOR(temp,current)) {
                        longcache2[LOCALHASH(temp,0,256)] = temp;
                    }
                }
                cache[colorhash] = current;
            }
        }
        if (cfg.bytecap && 4*i + rows_read*width > cfg.bytecap) break;
        rows_read++;
    }
    //Flush all buffers
    QOIG_PRINT(0);
    *outlen = ct;
    return 0;
}

int qoig_decode(FILE *infile, size_t width, spng_ctx *ctx, size_t *outlen, qoig_cfg cfg) {
    color cache[64] = {0};
    color longcache1[256];
    color longcache2[256];
    color current = (color){.alpha=255};
    color temp;
    uint8_t cbyte = 0;
    uint8_t rgbrun = 0;
    unsigned int i=0;
    char j;
    uint8_t m;
    uint32_t run=0;
    uint8_t row[width*cfg.channels];
    unsigned int rows_read = 0;
    int ret;
    int cachelengths[31] = QOIG_CACHES;
    int clen;
    
    if (!row) {
        return -1;
    }
    clen = cachelengths[cfg.clen];
    *outlen = 0;
    if (cfg.longindex) {
        if (IS_BIG_ENDIAN) {
			memcpy(longcache1,default_colors_be,256*sizeof(color));
            memcpy(longcache2,default_colors2_be,256*sizeof(color));
        } else {
			memcpy(longcache1,default_colors_le,256*sizeof(color));
            memcpy(longcache2,default_colors2_le,256*sizeof(color));
        }
    }
    if (clen) {
        cache[HASH(current,clen)] = current;
        if (cfg.longindex) longcache1[LHASH(current)] = current;
    }
    do { 
        for (i=0;i<cfg.channels*width;i+=cfg.channels) {
            j=0;
            //Add another pixel for current run
            if (run) {
                memcpy(row+i,&current,cfg.channels);
                *outlen += cfg.channels;
                run--;
                continue;
            }
            
            //Fetch next byte
            if (rgbrun) {
                rgbrun--;
            } else {
                QOIG_READ(&cbyte,1,1,infile);
            }

            //Decode next codeword
            switch (cbyte&OP_CODE) {

                case OP_INDEX:
                    j = cbyte&OP_INDEX_ARG;
                    if (cfg.longindex && j>61) {
                        QOIG_READ(&cbyte,1,1,infile);
                        if (j==62) {
                            current = longcache1[cbyte];
                            break;
                        } else {
                            current = longcache2[cbyte];
                        }
                        
                    } else {
                        current = cache[j];
                        if (j<clen) break;
                    }
                    QOIG_READ(&cbyte,1,1,infile);

                case OP_LUMA:
                    if ((cbyte&OP_CODE) == OP_LUMA) {
                        j = (cbyte&OP_LUMA_ARG)-32;
                        QOIG_READ(&cbyte,1,1,infile);
                        current.green += j;
                        current.red += j+(LRS(cbyte,4)&0xF)-8;
                        current.blue += j+(cbyte&0xF)-8;
                        break;
                    }
                case OP_DIFF:
                    if (cfg.rawblocks && !j && cbyte == OP_RGBRUN) {
                        QOIG_READ(&rgbrun,1,1,infile);
                        cbyte = OP_RGB + LRS(rgbrun,7);
                        rgbrun = (rgbrun&0x7F)+1;
                    } else {
                        current.red += (LRS(cbyte,4)&3)-2;
                        current.green += (LRS(cbyte,2)&3)-2;
                        current.blue += (cbyte&3)-2;
                        break;
                    }



                case OP_RUN:
                    if (cbyte == OP_RGB || cbyte == OP_RGBA) {
                        QOIG_READ(&current,1,3+(cbyte == OP_RGBA),infile);
                        if (64-clen-2*cfg.longindex) {
                            if (cfg.longindex) {
                                temp = cache[LOCALHASH(current,clen,64-2*cfg.longindex)];
                                if (!EQCOLOR(temp,current)) {
                                    longcache2[LOCALHASH(temp,0,256)] = temp;
                                }
                            }
                            cache[LOCALHASH(current,clen,64-2*cfg.longindex)] = current;
                        }
                    } else {
                        run = cbyte&OP_ARGS;
                        if (cfg.longruns&&run==61) {
                            QOIG_READ(&cbyte,1,1,infile);
                            if (cbyte < 128) {
                                run+=cbyte;
                            } else {
                                QOIG_READ(&m,1,1,infile);
                                run+=(((cbyte&0x7F)<<8)+m+128);
                            }
                        }
                    }
            }
                
            memcpy(row+i,&current,cfg.channels);
            if (clen) {
                if (cfg.longindex) {
                    temp = cache[HASH(current,clen)];
                    if (!EQCOLOR(temp,current)) {
                        longcache1[LHASH(temp)] = temp;
                    }
                }
                cache[HASH(current,clen)] = current;
            }
            *outlen += cfg.channels;
        }
    
        ret = spng_encode_row(ctx,row,cfg.channels*width);
        rows_read++;
    } while (!ret);
    //If we make it here, we're missing an end of bytestream code,
    //so there is probably something wrong with the file.
    return !(ret==SPNG_EOI);
}


size_t qoig_write(const char *infile, const char *outfile, qoig_cfg cfg) {
    FILE *inf;
	FILE *outf;
	size_t size, width;
    size_t byte_len;
    size_t limit = 1024 * 1024 * 64;
	char *encoded = NULL;
    uint32_t temp;
    int fmt = SPNG_FMT_RGBA8;
    qoig_desc desc;
    spng_ctx *ctx;
    
    inf = fopen(infile,"rb");
    
	if (!cfg.simulate) {
        outf = fopen(outfile,"wb");
    }
    if (!inf||!outf&&!cfg.simulate) {
		goto error;
	}

    ctx = spng_ctx_new(0);

    if (!ctx) {
        goto error;
    }

    // Ignore and don't calculate chunk CRC's
    spng_set_crc_action(ctx, SPNG_CRC_USE, SPNG_CRC_USE);    

    /* Set memory usage limits for storing standard and unknown chunks,
       this is important when reading untrusted files! */
    spng_set_chunk_limits(ctx, limit, limit);

    // Set source PNG
    spng_set_png_file(ctx, inf);

    struct spng_ihdr ihdr;

    if (spng_get_ihdr(ctx, &ihdr)||spng_decoded_image_size(ctx, fmt, &byte_len)||
        spng_decode_image(ctx, NULL, 0, fmt, SPNG_DECODE_PROGRESSIVE)) {
        goto error;
    }
    
    if (cfg.simulate) {
        cfg.bytecap = byte_len/10;
        if (cfg.bytecap < 10000) cfg.bytecap = 10000;
    }
    
    if (cfg.longindex && cfg.clen == 30) {
        cfg.clen = 29;
    }

    width = byte_len / (4*ihdr.height);
    
    //Construct description
    desc.width = width;
    desc.height = ihdr.height;
    desc.channels = 3+(ihdr.color_type>>2&1);
    //since we're just converting from png, probably safe to assume sRBG colorspace
    desc.colorspace = QOIG_SRBG;
    cfg.channels = desc.channels;
    
    if (!cfg.simulate) {
        //Write file header
        fprintf(outf,"qoi%c", (char)(cfg.longruns<<7|(!cfg.longindex)<<6|(!cfg.rawblocks)<<5|(cfg.clen^24)));
        temp = htonl(desc.width);
        fwrite(&temp, 1, 4, outf);
        temp = htonl(desc.height);
        fwrite(&temp, 1, 4, outf);
        fprintf(outf,"%c%c", desc.channels, desc.colorspace);
    }

	if (qoig_encode(ctx, width, outf, &size, cfg)) {
		goto error;
	}
    
    if (!cfg.simulate) {
        //I have no idea what the file footer is for.
        //Only print 7 bytes because we printed 1 coming out of qoig_encode
        fwrite("\0\0\0\0\0\0\1",1,7,outf);
        fclose(outf);
    }
    fclose(inf);
    
    spng_ctx_free(ctx);
	
	return size;
    error:
        fclose(inf);
        if (!cfg.simulate) fclose(outf);
        spng_ctx_free(ctx);
        return -1;
}


size_t qoig_read(const char *infile, const char *outfile) {
	FILE *inf = fopen(infile, "rb");
    FILE *outf = fopen(outfile, "wb");
	size_t size;
    long bytes_read, px_len;
    char magic[4];
    qoig_desc desc;
    struct spng_ihdr ihdr = {0};
    spng_ctx *enc;
    qoig_cfg cfg;
    int fmt;

    if (!inf || !outf) {
        goto error;
    }

    enc = spng_ctx_new(SPNG_CTX_ENCODER);

    if (!enc) {
        goto error;
    }


	//Check magic string
    if (fread(magic,1,4,inf)!=4||memcmp(magic,"qoi",3)) {
        goto error;
    }
    
    //Extract desc from header
    if (fread(&desc, 1, 10, inf)!=10) {
        goto error;
    }
    
    //Create config
    cfg.clen = (magic[3]&0x1F)^24;
    cfg.longruns = magic[3]>>7;
    cfg.longindex = !(magic[3]>>6&1);
    cfg.rawblocks = !(magic[3]>>5&1);
    cfg.channels = desc.channels;
    

    //Fix byte order on dimensions
    desc.width = ntohl(desc.width);
    desc.height = ntohl(desc.height);

    //Create PNG header
    ihdr.width = desc.width;
    ihdr.height = desc.height;
    ihdr.bit_depth = 8;
    ihdr.color_type = 4*desc.channels-10;
    
    if (spng_set_ihdr(enc,&ihdr)) {
        goto error;
    }
    
    //Set file for context
    spng_set_png_file(enc,outf);
    
    //Encoding format
    fmt = SPNG_FMT_PNG;
    
    
	if (spng_encode_image(enc, 0, 0, fmt, SPNG_ENCODE_PROGRESSIVE)||qoig_decode(inf, desc.width, enc, &size, cfg)) {
        goto error;
    }
    
    fclose(inf);
    fclose(outf);
    spng_ctx_free(enc);
	return size;

    error:
        fclose(inf);
        fclose(outf);
        spng_ctx_free(enc);
        return -1;
}
