#include "qoig.h"
#include <argp.h>
#include <stdlib.h>
#include <limits.h>


#define STR_ENDS_WITH(S, E) (strcmp(S + strlen(S) - (sizeof(E)-1), E) == 0)

typedef struct {

} params;

const char *argp_program_version =
  "qoigconv 0.1";
static char doc[] = 
  "Converter to QOIG -- convert images between PNG and QOIG. Options only for converting to QOIG.";
static char args_doc[] =
  "filename_to_convert filename_for_result";
/* The options we understand. */
static struct argp_option options[] = {
  {"plainqoi", 'q', 0, 0, "Use options for plain backwards-compatible QOI" },
  {"maxcomp", 'm', "clen", OPTION_ARG_OPTIONAL, "Max compression. Equiv. to -cclen -irs. If clen omitted, use -n31 (slow)." },
  {"fast", 'f', "clen", OPTION_ARG_OPTIONAL, "Good fast compression. Equiv. to -cclen -ir. clen defaults to 26."},
  {"cachesize", 'c', "clen", OPTION_ARG_OPTIONAL, "Set size of exact-match cache (0<=clen<=30)" },
  {"simnum", 'n', "num", OPTION_ARG_OPTIONAL, "Set number of cache lengths to test (0<=num<=31) for best compression (higher is slower)" },
  {"longruns", 'r', 0, 0, "Use extra compression on long runs"},
  {"longindex", 'i', 0, 0, "Use larger secondary color caches"},
  {"rawblocks", 'b', 0, 0, "Allow blocks of uncompressed colors"},
  {"search", 's', 0, 0, "Search entire local cache for similar colors (slower but slight compression improvement)"},
  { 0 }
};
struct arguments
{
    char *filenames[2];
    unsigned char longruns;
    unsigned char longindex;
    unsigned char rawblocks;
    unsigned char clen;
    unsigned char simnum;
    unsigned char plainqoi;
    unsigned char search;
};
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = state->input;
    switch (key) {
        case 'q':
            arguments->plainqoi = 1;
            arguments->clen = 64;
            arguments->longruns = 0;
            break;
        case 'm':
            if (!arguments->plainqoi) {
                if (arg!=NULL) {
                    arguments->clen = atoi(arg);
                    if (arguments->clen<0||arguments->clen>30) {
                        argp_error(state,"Cache length must be in the range 0 to 30.");
                    }
                } else {
                    arguments->simnum = 31;
                }
                arguments->longruns = 1;
                arguments->longindex = 1;
                arguments->rawblocks = 1;
                arguments->search = 1;
            }
            break;
        case 'f':
            if (!arguments->plainqoi) {
                if (arg!=NULL) {
                    arguments->clen = atoi(arg);
                    if (arguments->clen<0||arguments->clen>30) {
                        argp_error(state,"Cache length must be in the range 0 to 30.");
                    }
                } else {
                    arguments->clen = 26;
                }
                arguments->longruns = 1;
                arguments->longindex = 1;
                arguments->rawblocks = 1;
            }
            break;
        case 'c':
            if (!arguments->plainqoi) {
                if (arg == NULL) {
                    argp_error(state,"No argument provided for cache length option.");
                }
                arguments->clen = atoi(arg);
                if (arguments->clen<0||arguments->clen>30) {
                    argp_error(state,"Cache length must be in the range 0 to 30.");
                }
            }
            break;
        case 'n':
            if (!arguments->plainqoi) {
                 if (arg == NULL) {
                    argp_error(state,"No argument provided for number of cache lengths to try.");
                }
                arguments->simnum = atoi(arg);
                if (arguments->simnum<0||arguments->simnum>31) {
                    argp_error(state,"Number of cache lengths to try must be in the range 0 to 31.");
                }
            }
            break;
        case 'r':
            if (!arguments->plainqoi) arguments->longruns = 1;
            break;
        case 'i':
            if (!arguments->plainqoi) arguments->longindex = 1;
            break;
        case 's':
            arguments->search = 1;
            break;
        case 'b':
            if (!arguments->plainqoi) arguments->rawblocks = 1;
            break;
        case ARGP_KEY_ARG:
            if (state->arg_num >= 2) {
                argp_error(state, "Too many arguments. Provide one input and one output filename.");
            }
            if (!STR_ENDS_WITH(arg,".png")&&!STR_ENDS_WITH(arg,".qog")&&!STR_ENDS_WITH(arg,".qoi")) {
                argp_error(state, "Input and output files must be .png, .qog, or .qoi");
            }
            arguments->filenames[state->arg_num] = arg;
            break;
        case ARGP_KEY_END:
            if (state->arg_num < 2) {
                argp_error(state, "Too few arguments. Provide one input and one output filename.");
            }
            if (!STR_ENDS_WITH(arguments->filenames[0],".png")&&!STR_ENDS_WITH(arguments->filenames[1],".png")) {
                argp_error(state, "Either the input file or output file must be a .png file.");
            }
            if (!STR_ENDS_WITH(arguments->filenames[0],".qog")&&!STR_ENDS_WITH(arguments->filenames[1],".qog")&&
                !STR_ENDS_WITH(arguments->filenames[0],".qoi")&&!STR_ENDS_WITH(arguments->filenames[1],".qoi")) {
                argp_error(state, "Either the input file or output file must be a .qoi or .qog file.");
            }
            if (STR_ENDS_WITH(arguments->filenames[1],".qoi")) {
                arguments->plainqoi = 1;
                arguments->clen = 64;
                arguments->longruns = 0;
                arguments->simnum = 0;
                arguments->search = 0;
                arguments->rawblocks = 0;
            }
            break;

        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char **argv) {
	const char a236206[31] = {23,18,26,13,28,7,30,0,22,27,20,25,15,29,10,24,5,19,16,12,8,3,21,17,14,11,9,6,4,2,1};
    struct arguments arguments = {0};
    qoig_cfg cfg = {0};
    char i,bestclen;
    int size;
    int compsize=INT_MAX;
    
    
    
    argp_parse (&argp, argc, argv, 0, 0, &arguments);
    
    
	if (STR_ENDS_WITH(arguments.filenames[0], ".png")) {
        //Encode to QOIG
        cfg.searchcache = arguments.search;
        cfg.longruns = arguments.longruns;
        cfg.longindex = arguments.longindex;
        cfg.rawblocks = arguments.rawblocks;
        bestclen = arguments.clen;
        cfg.simulate = 1;
        for (i=0;i<arguments.simnum;i++) {
            if (cfg.longindex && i==6) continue;
            cfg.clen = a236206[i];
            size = qoig_write(arguments.filenames[0],arguments.filenames[1],cfg);
            if (size<compsize) {
                compsize = size;
                bestclen = cfg.clen;
            }
        }
        if (arguments.simnum) {
            printf("Best cache size was %d.\n",bestclen);
        }
        cfg.simulate = 0;
        cfg.clen = bestclen;
        cfg.bytecap = 0;
        return qoig_write(arguments.filenames[0],arguments.filenames[1],cfg)<0;
	} else {
        //Decode from QOIG
        return qoig_read(arguments.filenames[0],arguments.filenames[1])<0;
    }
}
