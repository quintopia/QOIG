#include <stdlib.h>
#include <string.h>
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
  the fourth byte of the file are now used to encode this place (with 
  a bias of 7). Setting the cache length parameter to 30 reproduces the 
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
#define OP_RGB (uint8_t)0xFE
#define OP_RGBA (uint8_t)0xFF
#define OP_INDEX (uint8_t)0
#define OP_DIFF (uint8_t)0x40
#define OP_LUMA (uint8_t)0x80
#define OP_RUN (uint8_t)0xC0
#define OP_CODE (uint8_t)0xC0
#define OP_ARGS (uint8_t)0x3F
#define OP_LUMA_ARG (uint8_t)0x3F
#define OP_INDEX_ARG (uint8_t)0x3F
#define HASH(C,H) ((C.red*3+C.green*5+C.blue*7+C.alpha*11)%H)
#define LRS(a,b) ((unsigned)a>>b)
#define LOCALHASH(C,H,L) (H+(LRS(C.red+8,3)*37+LRS(C.green+8,3)*59+\
                     LRS(C.blue+8,3)*67)%(L-H))
#define TUBITRANGE(a,b) ((char)(a-b)>-3 && (char)(a-b)<2)
#define COLORRANGES(a,b) TUBITRANGE(a.red,b.red) && \
                         TUBITRANGE(a.green,b.green) && \
                         TUBITRANGE(a.blue,b.blue)
#define EQCOLOR(a,b) (a.red == b.red && a.green == b.green && \
                       a.blue == b.blue && a.alpha == b.alpha)
#define QOIG_PRINT(b) if (!cfg.simulate) fprintf(outfile,"%c",b);\
                      ct++
#define QOIG_READ(a,b,c,d) if (fread(a,b,c,d)!=c) return -1

typedef struct {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    unsigned char alpha;
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
} qoig_cfg;


int qoig_encode(spng_ctx *ctx, size_t width, FILE *outfile, unsigned long *outlen, qoig_cfg cfg) {
    color cache[64] = {0};
    color longcache1[256] = {0};
    color longcache2[256] = {0};
    color last;
    color current = (color){.alpha=255};
    color temp,temp2;
    int i,j;
    char k,l,m;
    uint8_t lastrow = 0;
    uint8_t ret,done;
    uint32_t run = 0;
    unsigned long ct = 0;
    int rows_read = 0;
    char *row = malloc(4*width);
    uint8_t colorhash,lcolorhash;
    int cachelengths[31] = QOIG_CACHES;
    int clen;
    
    if (!row) {
        return -1;
    }
    clen = cachelengths[cfg.clen];
    /*spng_decode_row is a bad API. a sane API would return 0 after every successful read*/
    while (!(ret = spng_decode_row(ctx, row, 4*width)) || lastrow && ret == SPNG_EOI || run) {
        done = ret == SPNG_EOI && !lastrow;
        lastrow = !ret;
        for (i=0;i<4*width&&(!cfg.bytecap||i+width*rows_read<cfg.bytecap);i+=4) {
            
            last = current;
            
            //Get next pixel

            memcpy(&current,row+i,4);

            //Try to make run
            if (EQCOLOR(current,last) && (run<62 || cfg.longruns && run < 32957)) {
                run++;
                continue;
            }
            if (run) {
                if (run <= 62) {
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
                    lcolorhash = HASH(current,256);
                    temp2 = longcache1[colorhash];
                    longcache1[HASH(temp,256)] = temp;
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
                if (cfg.longindex) {
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
                    if (current.alpha != last.alpha) {
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
            if (64-clen-2*cfg.longindex) {
                if (cfg.longindex) {
                    temp = cache[colorhash];
                    longcache2[LOCALHASH(temp,0,256)] = temp;
                }
                cache[colorhash] = current;
            }
        }
        if (cfg.bytecap && i + rows_read*width > cfg.bytecap) break;
        rows_read++;
    }
    *outlen = ct;
    free(row);
    return 0;
}

int qoig_decode(FILE *infile, size_t width, spng_ctx *ctx, size_t *outlen, qoig_cfg cfg) {
    color cache[64] = {0};
    color longcache1[256] = {0};
    color longcache2[256] = {0};
    color current = (color){.alpha=255};
    color temp;
    uint8_t cbyte = 0;
    int i=0;
    uint8_t j;
    int buflen;
    char *tempbuf;
    uint32_t run=0;
    char *row = malloc(cfg.channels*width);
    int ret;
    int cachelengths[31] = QOIG_CACHES;
    int clen;
    
    if (!row) {
        return -1;
    }
    clen = cachelengths[cfg.clen];
    *outlen = 0;
    if (clen) {
        cache[HASH(current,clen)] = current;
        if (cfg.longindex) longcache1[HASH(current,256)] = current;
    }
    
    do { 
        for (i=0;i<cfg.channels*width;i+=cfg.channels) {
            
            //Add another pixel for current run
            if (run) {
                memcpy(row+i,&current,cfg.channels);
                *outlen += cfg.channels;
                run--;
                continue;
            }
            
            //Fetch next byte
            QOIG_READ(&cbyte,1,1,infile);

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

                case OP_DIFF:
                    if ((cbyte&OP_CODE) == OP_DIFF) {
                        current.red += (LRS(cbyte,4)&3)-2;
                        current.green += (LRS(cbyte,2)&3)-2;
                        current.blue += (cbyte&3)-2;
                        break;
                    }

                case OP_LUMA:
                    j = (cbyte&OP_LUMA_ARG)-32;
                    QOIG_READ(&cbyte,1,1,infile);
                    current.green += j;
                    current.red += j+(LRS(cbyte,4)&0xF)-8;
                    current.blue += j+(cbyte&0xF)-8;
                break;

                case OP_RUN:
                    if (cbyte == OP_RGB || cbyte == OP_RGBA) {
                        QOIG_READ(&current,1,3+(cbyte == OP_RGBA),infile);
                        if (64-clen-2*cfg.longindex) {
                            if (cfg.longindex) {
                                temp = cache[LOCALHASH(current,clen,64-2*cfg.longindex)];
                                longcache2[LOCALHASH(temp,0,256)] = temp;
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
                                QOIG_READ(&j,1,1,infile);
                                run+=(((cbyte&0x7F)<<8)+j+128);
                            }
                        }
                    }
            }
            memcpy(row+i,&current,cfg.channels);
            if (clen) {
                if (cfg.longindex) {
                    temp = cache[HASH(current,clen)];
                    longcache1[HASH(temp,256)] = temp;
                }
                cache[HASH(current,clen)] = current;
                
            }
            *outlen += cfg.channels;
        }
    
        ret = spng_encode_row(ctx,row,cfg.channels*width);
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
    
    if (!cfg.simulate) {
        //Write file header
        fprintf(outf,"qoi%c", (char)(cfg.longruns<<7|(!cfg.longindex)<<6|cfg.clen+7));
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
        fwrite("\0\0\0\0\0\0\0\1",1,8,outf);
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
    cfg.clen = (magic[3]&0x3F)-7;
    cfg.longruns = magic[3]>>7;
    cfg.longindex = !(magic[3]>>6&1);
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
