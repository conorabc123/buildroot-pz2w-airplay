/* vi: set sw=4 ts=4: */
/*
 * awk implementation for busybox
 *
 * Copyright (C) 2002 by Dmitry Zakharov <dmit@crp.bank.gov.ua>
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config AWK
//config:	bool "awk (24 kb)"
//config:	default y
//config:	help
//config:	Awk is used as a pattern scanning and processing language.
//config:
//config:config FEATURE_AWK_LIBM
//config:	bool "Enable math functions (requires libm)"
//config:	default y
//config:	depends on AWK
//config:	help
//config:	Enable math functions of the Awk programming language.
//config:	NOTE: This requires libm to be present for linking.
//config:
//config:config FEATURE_AWK_GNU_EXTENSIONS
//config:	bool "Enable a few GNU extensions"
//config:	default y
//config:	depends on AWK
//config:	help
//config:	Enable a few features from gawk:
//config:	* command line option -e AWK_PROGRAM
//config:	* simultaneous use of -f and -e on the command line.
//config:	This enables the use of awk library files.
//config:	Example: awk -f mylib.awk -e '{print myfunction($1);}' ...

//applet:IF_AWK(APPLET_NOEXEC(awk, awk, BB_DIR_USR_BIN, BB_SUID_DROP, awk))

//kbuild:lib-$(CONFIG_AWK) += awk.o

//usage:#define awk_trivial_usage
//usage:       "[OPTIONS] [AWK_PROGRAM] [FILE]..."
//usage:#define awk_full_usage "\n\n"
//usage:       "	-v VAR=VAL	Set variable"
//usage:     "\n	-F SEP		Use SEP as field separator"
//usage:     "\n	-f/-E FILE	Read program from FILE"
//usage:	IF_FEATURE_AWK_GNU_EXTENSIONS(
//usage:     "\n	-e AWK_PROGRAM"
//usage:	)

#include "libbb.h"
#include "xregex.h"
#include <math.h>

/* This is a NOEXEC applet. Be very careful! */


/* If you comment out one of these below, it will be #defined later
 * to perform debug printfs to stderr: */
#define debug_printf_walker(...)  do {} while (0)
#define debug_printf_eval(...)  do {} while (0)
#define debug_printf_parse(...)  do {} while (0)

#ifndef debug_printf_walker
# define debug_printf_walker(...) (fprintf(stderr, __VA_ARGS__))
#endif
#ifndef debug_printf_eval
# define debug_printf_eval(...) (fprintf(stderr, __VA_ARGS__))
#endif
#ifndef debug_printf_parse
# define debug_printf_parse(...) (fprintf(stderr, __VA_ARGS__))
#else
# define debug_parse_print_tc(...) ((void)0)
#endif


/* "+": stop on first non-option:
 * $ awk 'BEGIN { for(i=1; i<ARGC; ++i) { print i ": " ARGV[i] }}' -argz
 * 1: -argz
 */
#define OPTSTR_AWK "+" \
	"F:v:f:" \
	IF_FEATURE_AWK_GNU_EXTENSIONS("e:E:") \
	"W:"
enum {
	OPTBIT_F,	/* define field separator */
	OPTBIT_v,	/* define variable */
	OPTBIT_f,	/* pull in awk program from file */
	IF_FEATURE_AWK_GNU_EXTENSIONS(OPTBIT_e,) /* -e AWK_PROGRAM */
	OPTBIT_W,	/* -W ignored */
	OPT_F = 1 << OPTBIT_F,
	OPT_v = 1 << OPTBIT_v,
	OPT_f = 1 << OPTBIT_f,
	OPT_e = IF_FEATURE_AWK_GNU_EXTENSIONS((1 << OPTBIT_e)) + 0,
	OPT_W = 1 << OPTBIT_W
};

#define	MAXVARFMT       240

/* variable flags */
#define	VF_NUMBER       0x0001	/* 1 = primary type is number */
#define	VF_ARRAY        0x0002	/* 1 = it's an array */

#define	VF_CACHED       0x0100	/* 1 = num/str value has cached str/num eq */
#define	VF_USER         0x0200	/* 1 = user input (may be numeric string) */
#define	VF_SPECIAL      0x0400	/* 1 = requires extra handling when changed */
#define	VF_WALK         0x0800	/* 1 = variable has alloc'd x.walker list */
#define	VF_FSTR         0x1000	/* 1 = don't free() var::string (not malloced, or is owned by something else) */
#define	VF_CHILD        0x2000	/* 1 = function arg; x.parent points to source */
#define	VF_DIRTY        0x4000	/* 1 = variable was set explicitly */

/* these flags are static, don't change them when value is changed */
#define	VF_DONTTOUCH    (VF_ARRAY | VF_SPECIAL | VF_WALK | VF_CHILD | VF_DIRTY)

typedef struct walker_list {
	char *end;
	char *cur;
	struct walker_list *prev;
	char wbuf[1];
} walker_list;

/* Variable */
typedef struct var_s {
	unsigned type;            /* flags */
	char *string;
	double number;
	union {
		int aidx;               /* func arg idx (for compilation stage) */
		struct xhash_s *array;  /* array ptr */
		struct var_s *parent;   /* for func args, ptr to actual parameter */
		walker_list *walker;    /* list of array elements (for..in) */
	} x;
} var;

/* Node chain (pattern-action chain, BEGIN, END, function bodies) */
typedef struct chain_s {
	struct node_s *first;
	struct node_s *last;
	const char *programname;
} chain;

/* Function */
typedef struct func_s {
	unsigned nargs;
	smallint defined;
	struct chain_s body;
} func;

/* I/O stream */
typedef struct rstream_s {
	FILE *F;
	char *buffer;
	int adv;
	int size;
	int pos;
	smallint is_pipe;
} rstream;

typedef struct hash_item_s {
	union {
		struct var_s v;         /* variable/array hash */
		struct rstream_s rs;    /* redirect streams hash */
		struct func_s f;        /* functions hash */
	} data;
	struct hash_item_s *next;       /* next in chain */
	char name[1];                   /* really it's longer */
} hash_item;

typedef struct xhash_s {
	unsigned nel;           /* num of elements */
	unsigned csize;         /* current hash size */
	unsigned nprime;        /* next hash size in PRIMES[] */
	unsigned glen;          /* summary length of item names */
	struct hash_item_s **items;
} xhash;

/* Tree node */
typedef struct node_s {
	uint32_t info;
	unsigned lineno;
	union {
		struct node_s *n;
		var *v;
		int aidx;
		const char *new_progname;
		/* if TI_REGEXP node, points to regex_t[2] array (case sensitive and insensitive) */
		regex_t *re;
	} l;
	union {
		struct node_s *n;
		func *f;
	} r;
	union {
		struct node_s *n;
	} a;
} node;

typedef struct tsplitter_s {
	node n;
	regex_t re[2];
} tsplitter;

/* simple token classes */
/* order and hex values are very important!!!  See next_token() */
#define TC_LPAREN       (1 << 0)        /* ( */
#define TC_RPAREN       (1 << 1)        /* ) */
#define TC_REGEXP       (1 << 2)        /* /.../ */
#define TC_OUTRDR       (1 << 3)        /* | > >> */
#define TC_UOPPOST      (1 << 4)        /* unary postfix operator ++ -- */
#define TC_UOPPRE1      (1 << 5)        /* unary prefix operator ++ -- $ */
#define TC_BINOPX       (1 << 6)        /* two-opnd operator */
#define TC_IN           (1 << 7)        /* 'in' */
#define TC_COMMA        (1 << 8)        /* , */
#define TC_PIPE         (1 << 9)        /* input redirection pipe | */
#define TC_UOPPRE2      (1 << 10)       /* unary prefix operator + - ! */
#define TC_ARRTERM      (1 << 11)       /* ] */
#define TC_LBRACE       (1 << 12)       /* { */
#define TC_RBRACE       (1 << 13)       /* } */
#define TC_SEMICOL      (1 << 14)       /* ; */
#define TC_NEWLINE      (1 << 15)
#define TC_STATX        (1 << 16)       /* ctl statement (for, next...) */
#define TC_WHILE        (1 << 17)       /* 'while' */
#define TC_ELSE         (1 << 18)       /* 'else' */
#define TC_BUILTIN      (1 << 19)
/* This costs ~50 bytes of code.
 * A separate class to support deprecated "length" form. If we don't need that
 * (i.e. if we demand that only "length()" with () is valid), then TC_LENGTH
 * can be merged with TC_BUILTIN:
 */
#define TC_LENGTH       (1 << 20)       /* 'length' */
#define TC_GETLINE      (1 << 21)       /* 'getline' */
#define TC_FUNCDECL     (1 << 22)       /* 'function' 'func' */
#define TC_BEGIN        (1 << 23)       /* 'BEGIN' */
#define TC_END          (1 << 24)       /* 'END' */
#define TC_EOF          (1 << 25)
#define TC_VARIABLE     (1 << 26)       /* name */
#define TC_ARRAY        (1 << 27)       /* name[ */
#define TC_FUNCTION     (1 << 28)       /* name( */
#define TC_STRING       (1 << 29)       /* "..." */
#define TC_NUMBER       (1 << 30)

#ifndef debug_parse_print_tc
static void debug_parse_print_tc(uint32_t n)
{
	if (n & TC_LPAREN  ) debug_printf_parse(" LPAREN"  );
	if (n & TC_RPAREN  ) debug_printf_parse(" RPAREN"  );
	if (n & TC_REGEXP  ) debug_printf_parse(" REGEXP"  );
	if (n & TC_OUTRDR  ) debug_printf_parse(" OUTRDR"  );
	if (n & TC_UOPPOST ) debug_printf_parse(" UOPPOST" );
	if (n & TC_UOPPRE1 ) debug_printf_parse(" UOPPRE1" );
	if (n & TC_BINOPX  ) debug_printf_parse(" BINOPX"  );
	if (n & TC_IN      ) debug_printf_parse(" IN"      );
	if (n & TC_COMMA   ) debug_printf_parse(" COMMA"   );
	if (n & TC_PIPE    ) debug_printf_parse(" PIPE"    );
	if (n & TC_UOPPRE2 ) debug_printf_parse(" UOPPRE2" );
	if (n & TC_ARRTERM ) debug_printf_parse(" ARRTERM" );
	if (n & TC_LBRACE  ) debug_printf_parse(" LBRACE"  );
	if (n & TC_RBRACE  ) debug_printf_parse(" RBRACE"  );
	if (n & TC_SEMICOL ) debug_printf_parse(" SEMICOL" );
	if (n & TC_NEWLINE ) debug_printf_parse(" NEWLINE" );
	if (n & TC_STATX   ) debug_printf_parse(" STATX"   );
	if (n & TC_WHILE   ) debug_printf_parse(" WHILE"   );
	if (n & TC_ELSE    ) debug_printf_parse(" ELSE"    );
	if (n & TC_BUILTIN ) debug_printf_parse(" BUILTIN" );
	if (n & TC_LENGTH  ) debug_printf_parse(" LENGTH"  );
	if (n & TC_GETLINE ) debug_printf_parse(" GETLINE" );
	if (n & TC_FUNCDECL) debug_printf_parse(" FUNCDECL");
	if (n & TC_BEGIN   ) debug_printf_parse(" BEGIN"   );
	if (n & TC_END     ) debug_printf_parse(" END"     );
	if (n & TC_EOF     ) debug_printf_parse(" EOF"     );
	if (n & TC_VARIABLE) debug_printf_parse(" VARIABLE");
	if (n & TC_ARRAY   ) debug_printf_parse(" ARRAY"   );
	if (n & TC_FUNCTION) debug_printf_parse(" FUNCTION");
	if (n & TC_STRING  ) debug_printf_parse(" STRING"  );
	if (n & TC_NUMBER  ) debug_printf_parse(" NUMBER"  );
}
#endif

/* combined token classes ("token [class] sets") */
#define	TS_UOPPRE   (TC_UOPPRE1 | TC_UOPPRE2)

#define	TS_BINOP    (TC_BINOPX | TC_COMMA | TC_PIPE | TC_IN)
//#define TS_UNARYOP (TS_UOPPRE | TC_UOPPOST)
#define	TS_OPERAND  (TC_VARIABLE | TC_ARRAY | TC_FUNCTION \
                    | TC_BUILTIN | TC_LENGTH | TC_GETLINE \
                    | TC_LPAREN | TC_STRING | TC_NUMBER)

#define	TS_LVALUE   (TC_VARIABLE | TC_ARRAY)
#define	TS_STATEMNT (TC_STATX | TC_WHILE)

/* word tokens, cannot mean something else if not expected */
#define	TS_WORD     (TC_IN | TS_STATEMNT | TC_ELSE \
                    | TC_BUILTIN | TC_LENGTH | TC_GETLINE \
                    | TC_FUNCDECL | TC_BEGIN | TC_END)

/* discard newlines after these */
#define	TS_NOTERM   (TS_BINOP | TC_COMMA | TC_LBRACE | TC_RBRACE \
                    | TC_SEMICOL | TC_NEWLINE)

/* what can expression begin with */
#define	TS_OPSEQ    (TS_OPERAND | TS_UOPPRE | TC_REGEXP)
/* what can group begin with */
#define	TS_GRPSEQ   (TS_OPSEQ | TS_STATEMNT \
                    | TC_SEMICOL | TC_NEWLINE | TC_LBRACE)

/* if previous token class is CONCAT_L and next is CONCAT_R, concatenation */
/* operator is inserted between them */
#define	TS_CONCAT_L (TC_VARIABLE | TC_ARRTERM | TC_RPAREN \
                   | TC_STRING | TC_NUMBER | TC_UOPPOST \
                   | TC_LENGTH)
#define	TS_CONCAT_R (TS_OPERAND | TS_UOPPRE)

#define	OF_RES1     0x010000    /* evaluate(left_node) */
#define	OF_RES2     0x020000    /* evaluate(right_node) */
#define	OF_STR1     0x040000    /* ...and use its string value */
#define	OF_STR2     0x080000    /* ...and use its string value */
#define	OF_NUM1     0x100000    /* ...and use its numeric value */
#define	OF_REQUIRED 0x200000    /* left_node must not be NULL */
#define	OF_CHECKED  0x400000    /* range pattern flip-flop bit */

/* combined operator flags */
#define	xx	0
#define	xV	OF_RES2
#define	xS	(OF_RES2 | OF_STR2)
#define	Vx	OF_RES1
#define	Rx	OF_REQUIRED
#define	VV	(OF_RES1 | OF_RES2)
#define	Nx	(OF_RES1 | OF_NUM1)
#define	NV	(OF_RES1 | OF_NUM1 | OF_RES2)
#define	Sx	(OF_RES1 | OF_STR1)
#define	SV	(OF_RES1 | OF_STR1 | OF_RES2)
#define	SS	(OF_RES1 | OF_STR1 | OF_RES2 | OF_STR2)

#define	OPCLSMASK 0xFF00
#define	OPNMASK   0x007F

/* operator precedence is the highest byte (even: r->l, odd: l->r grouping)
 * (for builtins the byte has a different meaning)
 */
#undef P
#undef PRIMASK
#undef PRIMASK2
#define PRIMASK   0x7F000000
#define PRIMASK2  0x7E000000
/* Smaller 'x' means _higher_ operator precedence */
#define PRECEDENCE(x) (x << 24)
#define P(x)      PRECEDENCE(x)
#define LOWEST_PRECEDENCE PRIMASK

/* Operation classes */
#define	SHIFT_TIL_THIS	0x0600
#define	RECUR_FROM_THIS	0x1000
enum {
	OC_DELETE = 0x0100,     OC_EXEC = 0x0200,       OC_NEWSOURCE = 0x0300,
	OC_PRINT = 0x0400,      OC_PRINTF = 0x0500,     OC_WALKINIT = 0x0600,

	OC_BR = 0x0700,         OC_BREAK = 0x0800,      OC_CONTINUE = 0x0900,
	OC_EXIT = 0x0a00,       OC_NEXT = 0x0b00,       OC_NEXTFILE = 0x0c00,
	OC_TEST = 0x0d00,       OC_WALKNEXT = 0x0e00,

	OC_BINARY = 0x1000,     OC_BUILTIN = 0x1100,    OC_COLON = 0x1200,
	OC_COMMA = 0x1300,      OC_COMPARE = 0x1400,    OC_CONCAT = 0x1500,
	OC_FBLTIN = 0x1600,     OC_FIELD = 0x1700,      OC_FNARG = 0x1800,
	OC_FUNC = 0x1900,       OC_GETLINE = 0x1a00,    OC_IN = 0x1b00,
	OC_LAND = 0x1c00,       OC_LOR = 0x1d00,        OC_MATCH = 0x1e00,
	OC_MOVE = 0x1f00,       OC_PGETLINE = 0x2000,   OC_REGEXP = 0x2100,
	OC_REPLACE = 0x2200,    OC_RETURN = 0x2300,     OC_SPRINTF = 0x2400,
	OC_TERNARY = 0x2500,    OC_UNARY = 0x2600,      OC_VAR = 0x2700,
	OC_CONST = 0x2800,      OC_DONE = 0x2900,

	ST_IF = 0x3000,         ST_DO = 0x3100,         ST_FOR = 0x3200,
	ST_WHILE = 0x3300
};

/* simple builtins */
enum {
	F_in,	F_rn,	F_co,	F_ex,	F_lg,	F_si,	F_sq,	F_sr,
	F_ti,	F_le,	F_sy,	F_ff,	F_cl
};

/* builtins */
enum {
	B_a2,	B_ix,	B_ma,	B_sp,	B_ss,	B_ti,   B_mt,	B_lo,	B_up,
	B_ge,	B_gs,	B_su,
	B_an,	B_co,	B_ls,	B_or,	B_rs,	B_xo,
};

/* tokens and their corresponding info values */

#define NTC     "\377"  /* switch to next token class (tc<<1) */
#define NTCC    '\377'

static const char tokenlist[] ALIGN1 =
	"\1("         NTC                                   /* TC_LPAREN */
	"\1)"         NTC                                   /* TC_RPAREN */
	"\1/"         NTC                                   /* TC_REGEXP */
	"\2>>"        "\1>"         "\1|"       NTC         /* TC_OUTRDR */
	"\2++"        "\2--"        NTC                     /* TC_UOPPOST */
	"\2++"        "\2--"        "\1$"       NTC         /* TC_UOPPRE1 */
	"\2=="        "\1="         "\2+="      "\2-="      /* TC_BINOPX */
	"\2*="        "\2/="        "\2%="      "\2^="
	"\1+"         "\1-"         "\3**="     "\2**"
	"\1/"         "\1%"         "\1^"       "\1*"
	"\2!="        "\2>="        "\2<="      "\1>"
	"\1<"         "\2!~"        "\1~"       "\2&&"
	"\2||"        "\1?"         "\1:"       NTC
	"\2in"        NTC                                   /* TC_IN */
	"\1,"         NTC                                   /* TC_COMMA */
	"\1|"         NTC                                   /* TC_PIPE */
	"\1+"         "\1-"         "\1!"       NTC         /* TC_UOPPRE2 */
	"\1]"         NTC                                   /* TC_ARRTERM */
	"\1{"         NTC                                   /* TC_LBRACE */
	"\1}"         NTC                                   /* TC_RBRACE */
	"\1;"         NTC                                   /* TC_SEMICOL */
	"\1\n"        NTC                                   /* TC_NEWLINE */
	"\2if"        "\2do"        "\3for"     "\5break"   /* TC_STATX */
	"\10continue" "\6delete"    "\5print"
	"\6printf"    "\4next"      "\10nextfile"
	"\6return"    "\4exit"      NTC
	"\5while"     NTC                                   /* TC_WHILE */
	"\4else"      NTC                                   /* TC_ELSE */
	"\3and"       "\5compl"     "\6lshift"  "\2or"      /* TC_BUILTIN */
	"\6rshift"    "\3xor"
	"\5close"     "\6system"    "\6fflush"  "\5atan2"
	"\3cos"       "\3exp"       "\3int"     "\3log"
	"\4rand"      "\3sin"       "\4sqrt"    "\5srand"
	"\6gensub"    "\4gsub"      "\5index"   /* "\6length" was here */
	"\5match"     "\5split"     "\7sprintf" "\3sub"
	"\6substr"    "\7systime"   "\10strftime" "\6mktime"
	"\7tolower"   "\7toupper"   NTC
	"\6length"    NTC                                   /* TC_LENGTH */
	"\7getline"   NTC                                   /* TC_GETLINE */
	"\4func"      "\10function" NTC                     /* TC_FUNCDECL */
	"\5BEGIN"     NTC                                   /* TC_BEGIN */
	"\3END"                                             /* TC_END */
	/* compiler adds trailing "\0" */
	;

static const uint32_t tokeninfo[] ALIGN4 = {
	0, /* ( */
	0, /* ) */
#define TI_REGEXP OC_REGEXP
	TI_REGEXP, /* / */
	/* >> > | */
	xS|'a',                  xS|'w',                  xS|'|',
	/* ++ -- */
	OC_UNARY|xV|P(9)|'p',    OC_UNARY|xV|P(9)|'m',
#define TI_PREINC (OC_UNARY|xV|P(9)|'P')
#define TI_PREDEC (OC_UNARY|xV|P(9)|'M')
	/* ++ -- $ */
	TI_PREINC,               TI_PREDEC,               OC_FIELD|xV|P(5),
	/* == = += -= */
	OC_COMPARE|VV|P(39)|5,   OC_MOVE|VV|P(74),        OC_REPLACE|NV|P(74)|'+', OC_REPLACE|NV|P(74)|'-',
	/* *= /= %= ^= (^ is exponentiation, NOT xor) */
	OC_REPLACE|NV|P(74)|'*', OC_REPLACE|NV|P(74)|'/', OC_REPLACE|NV|P(74)|'%', OC_REPLACE|NV|P(74)|'&',
	/* + - **= ** */
	OC_BINARY|NV|P(29)|'+',  OC_BINARY|NV|P(29)|'-',  OC_REPLACE|NV|P(74)|'&', OC_BINARY|NV|P(15)|'&',
	/* / % ^ * */
	OC_BINARY|NV|P(25)|'/',  OC_BINARY|NV|P(25)|'%',  OC_BINARY|NV|P(15)|'&',  OC_BINARY|NV|P(25)|'*',
	/* != >= <= > */
	OC_COMPARE|VV|P(39)|4,   OC_COMPARE|VV|P(39)|3,   OC_COMPARE|VV|P(39)|0,   OC_COMPARE|VV|P(39)|1,
#define TI_LESS     (OC_COMPARE|VV|P(39)|2)
	/* < !~ ~ && */
	TI_LESS,                 OC_MATCH|Sx|P(45)|'!',   OC_MATCH|Sx|P(45)|'~',   OC_LAND|Vx|P(55),
#define TI_TERNARY  (OC_TERNARY|Vx|P(64)|'?')
#define TI_COLON    (OC_COLON|xx|P(67)|':')
	/* || ? : */
	OC_LOR|Vx|P(59),         TI_TERNARY,              TI_COLON,
#define TI_IN       (OC_IN|SV|P(49))
	TI_IN,
#define TI_COMMA    (OC_COMMA|SS|P(80))
	TI_COMMA,
#define TI_PGETLINE (OC_PGETLINE|SV|P(37))
	TI_PGETLINE, /* | */
	/* + - ! */
	OC_UNARY|xV|P(19)|'+',   OC_UNARY|xV|P(19)|'-',   OC_UNARY|xV|P(19)|'!',
	0, /* ] */
	0, /* { */
	0, /* } */
	0, /* ; */
	0, /* \n */
	ST_IF,        ST_DO,        ST_FOR,      OC_BREAK,
	OC_CONTINUE,  OC_DELETE|Rx, OC_PRINT,
	OC_PRINTF,    OC_NEXT,      OC_NEXTFILE,
	OC_RETURN|Vx, OC_EXIT|Nx,
	ST_WHILE,
	0, /* else */
// OC_B's are builtins with enforced minimum number of arguments (two upper bits).
//  Highest byte bit pattern: nn s3s2s1 v3v2v1
//  nn - min. number of args, sN - resolve Nth arg to string, vN - resolve to var
// OC_F's are builtins with zero or one argument.
//  |Rx| enforces that arg is present for: system, close, cos, sin, exp, int, log, sqrt
//  Check for no args is present in builtins' code (not in this table): rand, systime
//  Have one _optional_ arg: fflush, srand, length
#define OC_B   OC_BUILTIN
#define OC_F   OC_FBLTIN
#define A1     P(0x40) /*one arg*/
#define A2     P(0x80) /*two args*/
#define A3     P(0xc0) /*three args*/
#define __v    P(1)
#define _vv    P(3)
#define __s__v P(9)
#define __s_vv P(0x0b)
#define __svvv P(0x0f)
#define _ss_vv P(0x1b)
#define _s_vv_ P(0x16)
#define ss_vv_ P(0x36)
	OC_B|B_an|_vv|A2,   OC_B|B_co|__v|A1,   OC_B|B_ls|_vv|A2,   OC_B|B_or|_vv|A2,   // and    compl   lshift   or
	OC_B|B_rs|_vv|A2,   OC_B|B_xo|_vv|A2,                                           // rshift xor
	OC_F|F_cl|Sx|Rx,    OC_F|F_sy|Sx|Rx,    OC_F|F_ff|Sx,       OC_B|B_a2|_vv|A2,   // close  system  fflush   atan2
	OC_F|F_co|Nx|Rx,    OC_F|F_ex|Nx|Rx,    OC_F|F_in|Nx|Rx,    OC_F|F_lg|Nx|Rx,    // cos    exp     int      log
	OC_F|F_rn,          OC_F|F_si|Nx|Rx,    OC_F|F_sq|Nx|Rx,    OC_F|F_sr|Nx,       // rand   sin     sqrt     srand
	OC_B|B_ge|_s_vv_|A3,OC_B|B_gs|ss_vv_|A2,OC_B|B_ix|_ss_vv|A2,                    // gensub gsub    index  /*length was here*/
	OC_B|B_ma|__s__v|A2,OC_B|B_sp|__s_vv|A2,OC_SPRINTF,         OC_B|B_su|ss_vv_|A2,// match  split   sprintf  sub
	OC_B|B_ss|__svvv|A2,OC_F|F_ti,          OC_B|B_ti|__s_vv,   OC_B|B_mt|__s_vv|A1,// substr systime strftime mktime
	OC_B|B_lo|__s__v|A1,OC_B|B_up|__s__v|A1,                                        // tolower toupper
	OC_F|F_le|Sx,   // length
	OC_GETLINE|SV,  // getline
	0, 0, // func function
	0, // BEGIN
	0  // END
#undef A1
#undef A2
#undef A3
#undef OC_B
#undef OC_F
};

/* gawk 5.1.1 manpage says the precedence of comparisons and assignments are as follows:
 *  ......
 *  < > <= >= == !=
 *  ~ !~
 *  in
 *  &&
 *  ||
 *  ?:
 *  = += -= *= /= %= ^=
 * But there are some abnormalities:
 * awk 'BEGIN { print v=3==3,v }' - ok:
 * 1 1
 * awk 'BEGIN { print 3==v=3,v }' - wrong, (3==v)=3 is not a valid assignment:
 * 1 3
 * This also unexpectedly works: echo "foo" | awk '$1==$1="foo" {print $1}'
 * More than one comparison op fails to parse:
 * awk 'BEGIN { print 3==3==3 }' - syntax error (wrong, should work)
 * awk 'BEGIN { print 3==3!=3 }' - syntax error (wrong, should work)
 *
 * The ternary a?b:c works as follows in gawk: "a" can't be assignment
 * ("= has lower precedence than ?") but inside "b" or "c", assignment
 * is higher precedence:
 * awk 'BEGIN { u=v=w=1; print u=0?v=4:w=5; print u,v,w }'
 * 5
 * 5 1 5
 * This differs from C and shell's "test" rules for ?: which have implicit ()
 * around "b" in ?:, but not around "c" - they would barf on "w=5" above.
 * gawk allows nesting of ?: - this works:
 * u=0?v=4?5:6:w=7?8:9 means u=0?(v=4?5:6):(w=7?8:9)
 * bbox is buggy here, requires parens: "u=0?(v=4):(w=5)"
 */

/* internal variable names and their initial values       */
/* asterisk marks SPECIAL vars; $ is just no-named Field0 */
enum {
	CONVFMT,    OFMT,       FS,         OFS,
	ORS,        RS,         RT,         FILENAME,
	SUBSEP,     F0,         ARGIND,     ARGC,
	ARGV,       ERRNO,      FNR,        NR,
	NF,         IGNORECASE, ENVIRON,    NUM_INTERNAL_VARS
};

static const char vNames[] ALIGN1 =
	"CONVFMT\0" "OFMT\0"    "FS\0*"     "OFS\0"
	"ORS\0"     "RS\0*"     "RT\0"      "FILENAME\0"
	"SUBSEP\0"  "$\0*"      "ARGIND\0"  "ARGC\0"
	"ARGV\0"    "ERRNO\0"   "FNR\0"     "NR\0"
	"NF\0*"     "IGNORECASE\0*" "ENVIRON\0" "\0";

static const char vValues[] ALIGN1 =
	"%.6g\0"    "%.6g\0"    " \0"       " \0"
	"\n\0"      "\n\0"      "\0"        "\0"
	"\034\0"    "\0"        "\377";
#define str_percent_dot_6g vValues

/* hash size may grow to these values */
#define FIRST_PRIME 61
static const uint16_t PRIMES[] ALIGN2 = { 251, 1021, 4093, 16381, 65521 };


/* Globals. Split in two parts so that first one is addressed
 * with (mostly short) negative offsets.
 * NB: it's unsafe to put members of type "double"
 * into globals2 (gcc may fail to align them).
 */
struct globals {
	double t_double;
	chain beginseq, mainseq, endseq;
	chain *seq;
	node *break_ptr, *continue_ptr;
	xhash *ahash;  /* argument names, used only while parsing function bodies */
	xhash *fnhash; /* function names, used only in parsing stage */
	xhash *vhash;  /* variables and arrays */
	//xhash *fdhash; /* file objects, used only in execution stage */
	//we are reusing ahash as fdhash, via define (see later)
	const char *g_progname;
	int g_lineno;
	int num_fields;             /* number of existing $N's */
	unsigned num_alloc_fields;  /* current size of Fields[] */
	/* NB: Fields[0] corresponds to $1, not to $0 */
	var *Fields;
	char *g_pos;
	char g_saved_ch;
	smallint got_program;
	smallint icase;
	smallint exiting;
	smallint nextrec;
	smallint nextfile;
	smallint is_f0_split;
	smallint t_rollback;

	/* former statics from various functions */
	smallint next_token__concat_inserted;
	uint32_t next_token__save_tclass;
	uint32_t next_token__save_info;
};
struct globals2 {
	uint32_t t_info; /* often used */
	uint32_t t_tclass;
	char *t_string;
	int t_lineno;

	var *intvar[NUM_INTERNAL_VARS]; /* often used */

	rstream iF;

	/* former statics from various functions */
	char *split_f0__fstrings;

	unsigned next_input_file__argind;
	smallint next_input_file__input_file_seen;

	smalluint exitcode;

	unsigned evaluate__seed;
	var *evaluate__fnargs;
	regex_t evaluate__sreg;

	var ptest__tmpvar;
	var awk_printf__tmpvar;
	var as_regex__tmpvar;
	var exit__tmpvar;
	var main__tmpvar;

	tsplitter exec_builtin__tspl;

	/* biggest and least used members go last */
	tsplitter fsplitter, rsplitter;

	char g_buf[MAXVARFMT + 1];
};
#define G1 (ptr_to_globals[-1])
#define G (*(struct globals2 *)ptr_to_globals)
/* For debug. nm --size-sort awk.o | grep -vi ' [tr] ' */
//char G1size[sizeof(G1)]; // 0x70
//char Gsize[sizeof(G)]; // 0x2f8
/* Trying to keep most of members accessible with short offsets: */
//char Gofs_seed[offsetof(struct globals2, evaluate__seed)]; // 0x7c
#define t_double     (G1.t_double    )
#define beginseq     (G1.beginseq    )
#define mainseq      (G1.mainseq     )
#define endseq       (G1.endseq      )
#define seq          (G1.seq         )
#define break_ptr    (G1.break_ptr   )
#define continue_ptr (G1.continue_ptr)
#define ahash        (G1.ahash       )
#define fnhash       (G1.fnhash      )
#define vhash        (G1.vhash       )
#define fdhash       ahash
//^^^^^^^^^^^^^^^^^^ ahash is cleared after every function parsing,
// and ends up empty after parsing phase. Thus, we can simply reuse it
// for fdhash in execution stage.
#define g_progname   (G1.g_progname  )
#define g_lineno     (G1.g_lineno    )
#define num_fields   (G1.num_fields  )
#define num_alloc_fields (G1.num_alloc_fields)
#define Fields       (G1.Fields      )
#define g_pos        (G1.g_pos       )
#define g_saved_ch   (G1.g_saved_ch  )
#define got_program  (G1.got_program )
#define icase        (G1.icase       )
#define exiting      (G1.exiting     )
#define nextrec      (G1.nextrec     )
#define nextfile     (G1.nextfile    )
#define is_f0_split  (G1.is_f0_split )
#define t_rollback   (G1.t_rollback  )
#define t_info       (G.t_info      )
#define t_tclass     (G.t_tclass    )
#define t_string     (G.t_string    )
#define t_lineno     (G.t_lineno    )
#define intvar       (G.intvar      )
#define iF           (G.iF          )
#define fsplitter    (G.fsplitter   )
#define rsplitter    (G.rsplitter   )
#define g_buf        (G.g_buf       )
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS((char*)xzalloc(sizeof(G1)+sizeof(G)) + sizeof(G1)); \
	t_tclass = TC_NEWLINE; \
	G.evaluate__seed = 1; \
} while (0)

static const char EMSG_UNEXP_EOS[] ALIGN1 = "Unexpected end of string";
static const char EMSG_UNEXP_TOKEN[] ALIGN1 = "Unexpected token";
static const char EMSG_DIV_BY_ZERO[] ALIGN1 = "Division by zero";
static const char EMSG_INV_FMT[] ALIGN1 = "Invalid format specifier";
static const char EMSG_TOO_FEW_ARGS[] ALIGN1 = "Too few arguments";
static const char EMSG_NOT_ARRAY[] ALIGN1 = "Not an array";
static const char EMSG_POSSIBLE_ERROR[] ALIGN1 = "Possible syntax error";
static const char EMSG_UNDEF_FUNC[] ALIGN1 = "Call to undefined function";
static const char EMSG_NO_MATH[] ALIGN1 = "Math support is not compiled in";
static const char EMSG_NEGATIVE_FIELD[] ALIGN1 = "Access to negative field";

static int awk_exit(void) NORETURN;

static void syntax_error(const char *message) NORETURN;
static void syntax_error(const char *message)
{
	bb_error_msg_and_die("%s:%i: %s", g_progname, g_lineno, message);
}

/* ---- hash stuff ---- */

static unsigned hashidx(const char *name)
{
	unsigned idx = 0;

	while (*name)
		idx = *name++ + (idx << 6) - idx;
	return idx;
}

/* create new hash */
static xhash *hash_init(void)
{
	xhash *newhash;

	newhash = xzalloc(sizeof(*newhash));
	newhash->csize = FIRST_PRIME;
	newhash->items = xzalloc(FIRST_PRIME * sizeof(newhash->items[0]));

	return newhash;
}

static void hash_clear(xhash *hash)
{
	unsigned i;
	hash_item *hi, *thi;

	for (i = 0; i < hash->csize; i++) {
		hi = hash->items[i];
		while (hi) {
			thi = hi;
			hi = hi->next;
//FIXME: this assumes that it's a hash of *variables*:
			free(thi->data.v.string);
			free(thi);
		}
		hash->items[i] = NULL;
	}
	hash->glen = hash->nel = 0;
}

#if 0 //UNUSED
static void hash_free(xhash *hash)
{
	hash_clear(hash);
	free(hash->items);
	free(hash);
}
#endif

/* find item in hash, return ptr to data, NULL if not found */
static NOINLINE void *hash_search3(xhash *hash, const char *name, unsigned idx)
{
	hash_item *hi;

	hi = hash->items[idx % hash->csize];
	while (hi) {
		if (strcmp(hi->name, name) == 0)
			return &hi->data;
		hi = hi->next;
	}
	return NULL;
}

static void *hash_search(xhash *hash, const char *name)
{
	return hash_search3(hash, name,	hashidx(name));
}

/* grow hash if it becomes too big */
static void hash_rebuild(xhash *hash)
{
	unsigned newsize, i, idx;
	hash_item **newitems, *hi, *thi;

	if (hash->nprime == ARRAY_SIZE(PRIMES))
		return;

	newsize = PRIMES[hash->nprime++];
	newitems = xzalloc(newsize * sizeof(newitems[0]));

	for (i = 0; i < hash->csize; i++) {
		hi = hash->items[i];
		while (hi) {
			thi = hi;
			hi = thi->next;
			idx = hashidx(thi->name) % newsize;
			thi->next = newitems[idx];
			newitems[idx] = thi;
		}
	}

	free(hash->items);
	hash->csize = newsize;
	hash->items = newitems;
}

/* find item in hash, add it if necessary. Return ptr to data */
static void *hash_find(xhash *hash, const char *name)
{
	hash_item *hi;
	unsigned idx;
	int l;

	idx = hashidx(name);
	hi = hash_search3(hash, name, idx);
	if (!hi) {
		if (++hash->nel > hash->csize * 8)
			hash_rebuild(hash);

		l = strlen(name) + 1;
		hi = xzalloc(sizeof(*hi) + l);
		strcpy(hi->name, name);

		idx = idx % hash->csize;
		hi->next = hash->items[idx];
		hash->items[idx] = hi;
		hash->glen += l;
	}
	return &hi->data;
}

#define findvar(hash, name) ((var*)    hash_find((hash), (name)))
#define newvar(name)        ((var*)    hash_find(vhash, (name)))
#define newfile(name)       ((rstream*)hash_find(fdhash, (name)))
#define newfunc(name)       ((func*)   hash_find(fnhash, (name)))

static void hash_remove(xhash *hash, const char *name)
{
	hash_item *hi, **phi;

	phi = &hash->items[hashidx(name) % hash->csize];
	while (*phi) {
		hi = *phi;
		if (strcmp(hi->name, name) == 0) {
			hash->glen -= (strlen(name) + 1);
			hash->nel--;
			*phi = hi->next;
			free(hi);
			break;
		}
		phi = &hi->next;
	}
}

/* ------ some useful functions ------ */

static char *skip_spaces(char *p)
{
	for (;;) {
		if (*p == '\\' && p[1] == '\n') {
			p++;
			t_lineno++;
		} else if (*p != ' ' && *p != '\t') {
			break;
		}
		p++;
	}
	return p;
}

/* returns old *s, advances *s past word and terminating NUL */
static char *nextword(char **s)
{
	char *p = *s;
	char *q = p;
	while (*q++ != '\0')
		continue;
	*s = q;
	return p;
}

static char nextchar(char **s)
{
	char c, *pps;
 again:
	c = *(*s)++;
	pps = *s;
	if (c == '\\')
		c = bb_process_escape_sequence((const char**)s);
	/* Example awk statement:
	 * s = "abc\"def"
	 * we must treat \" as "
	 */
	if (c == '\\' && *s == pps) { /* unrecognized \z? */
		c = *(*s); /* yes, fetch z */
		if (c) { /* advance unless z = NUL */
			(*s)++;
			if (c == '\n') /* \<newline>? eat it */
				goto again;
		}
	}
	return c;
}

/* TODO: merge with strcpy_and_process_escape_sequences()?
 */
static void unescape_string_in_place(char *s1)
{
	char *s = s1;
	while ((*s1 = nextchar(&s)) != '\0')
		s1++;
}

static ALWAYS_INLINE int isalnum_(int c)
{
	return (isalnum(c) || c == '_');
}

static double my_strtod(char **pp)
{
	char *cp = *pp;
	return strtod(cp, pp);
}
#if ENABLE_DESKTOP
static double my_strtod_or_hexoct(char **pp)
{
	char *cp = *pp;
	if (cp[0] == '0') {
		/* Might be hex or octal integer: 0x123abc or 07777 */
		char c = (cp[1] | 0x20);
		if (c == 'x' || isdigit(cp[1])) {
			unsigned long long ull = strtoull(cp, pp, 0);
			if (c == 'x')
				return ull;
			c = **pp;
			if (!isdigit(c) && c != '.')
				return ull;
			/* else: it may be a floating number. Examples:
			 * 009.123 (*pp points to '9')
			 * 000.123 (*pp points to '.')
			 * fall through to strtod.
			 */
		}
	}
	return strtod(cp, pp);
}
#else
# define my_strtod_or_hexoct(p) my_strtod(p)
#endif

/* -------- working with variables (set/get/copy/etc) -------- */

static const char *fmt_num(const char *format, double n)
{
	if (n == (long long)n) {
		snprintf(g_buf, MAXVARFMT, "%lld", (long long)n);
	} else {
		const char *s = format;
		char c;

		do { c = *s; } while (c && *++s);
		if (strchr("diouxX", c)) {
			snprintf(g_buf, MAXVARFMT, format, (int)n);
		} else if (strchr("eEfFgGaA", c)) {
			snprintf(g_buf, MAXVARFMT, format, n);
		} else {
			syntax_error(EMSG_INV_FMT);
		}
	}
	return g_buf;
}

static xhash *iamarray(var *a)
{
	while (a->type & VF_CHILD)
		a = a->x.parent;

	if (!(a->type & VF_ARRAY)) {
		a->type |= VF_ARRAY;
		a->x.array = hash_init();
	}
	return a->x.array;
}

#define clear_array(array) hash_clear(array)

/* clear a variable */
static var *clrvar(var *v)
{
	if (!(v->type & VF_FSTR))
		free(v->string);

	v->type &= VF_DONTTOUCH;
	v->type |= VF_DIRTY;
	v->string = NULL;
	return v;
}

static void handle_special(var *);

/* assign string value to variable */
static var *setvar_p(var *v, char *value)
{
	clrvar(v);
	v->string = value;
	handle_special(v);
	return v;
}

/* same as setvar_p but make a copy of string */
static var *setvar_s(var *v, const char *value)
{
	return setvar_p(v, (value && *value) ? xstrdup(value) : NULL);
}

static var *setvar_sn(var *v, const char *value, int len)
{
	return setvar_p(v, (value && *value && len > 0) ? xstrndup(value, len) : NULL);
}

/* same as setvar_s but sets USER flag */
static var *setvar_u(var *v, const char *value)
{
	v = setvar_s(v, value);
	v->type |= VF_USER;
	return v;
}

/* set array element to user string */
static void setari_u(var *a, int idx, const char *s)
{
	var *v;

	v = findvar(iamarray(a), itoa(idx));
	setvar_u(v, s);
}

/* assign numeric value to variable */
static var *setvar_i(var *v, double value)
{
	clrvar(v);
	v->type |= VF_NUMBER;
	v->number = value;
	handle_special(v);
	return v;
}

static void setvar_ERRNO(void)
{
	setvar_i(intvar[ERRNO], errno);
}

static const char *getvar_s(var *v)
{
	/* if v is numeric and has no cached string, convert it to string */
	if ((v->type & (VF_NUMBER | VF_CACHED)) == VF_NUMBER) {
		const char *convfmt = str_percent_dot_6g; /* "%.6g" */
		/* Get CONVFMT, unless we already recursed on it:
		 * someone might try to cause stack overflow by setting
		 * CONVFMT=9 (a numeric, not string, value)
		 */
		if (v != intvar[CONVFMT])
			convfmt = getvar_s(intvar[CONVFMT]);
		/* Convert the value */
		v->string = xstrdup(fmt_num(convfmt, v->number));
		v->type |= VF_CACHED;
	}
	return (v->string == NULL) ? "" : v->string;
}

static double getvar_i(var *v)
{
	char *s;

	if ((v->type & (VF_NUMBER | VF_CACHED)) == 0) {
		v->number = 0;
		s = v->string;
		if (s && *s) {
			debug_printf_eval("getvar_i: '%s'->", s);
			v->number = my_strtod(&s);
			/* ^^^ hex/oct NOT allowed here! */
			debug_printf_eval("%f (s:'%s')\n", v->number, s);
			if (v->type & VF_USER) {
//TODO: skip_spaces() also skips backslash+newline, is it intended here?
				s = skip_spaces(s);
				if (*s != '\0')
					v->type &= ~VF_USER;
			}
		} else {
			debug_printf_eval("getvar_i: '%s'->zero\n", s);
			v->type &= ~VF_USER;
		}
		v->type |= VF_CACHED;
	}
	debug_printf_eval("getvar_i: %f\n", v->number);
	return v->number;
}

/* Used for operands of bitwise ops */
static unsigned long getvar_i_int(var *v)
{
	double d = getvar_i(v);

	/* Casting doubles to longs is undefined for values outside
	 * of target type range. Try to widen it as much as possible */
	if (d >= 0)
		return (unsigned long)d;
	/* Why? Think about d == -4294967295.0 (assuming 32bit longs) */
	return - (long) (unsigned long) (-d);
}

static var *copyvar(var *dest, const var *src)
{
	if (dest != src) {
		clrvar(dest);
		dest->type |= (src->type & ~(VF_DONTTOUCH | VF_FSTR));
		debug_printf_eval("copyvar: number:%f string:'%s'\n", src->number, src->string);
		dest->number = src->number;
		if (src->string)
			dest->string = xstrdup(src->string);
	}
	handle_special(dest);
	return dest;
}

static var *incvar(var *v)
{
	return setvar_i(v, getvar_i(v) + 1.0);
}

/* return true if v is number or numeric string */
static int is_numeric(var *v)
{
	getvar_i(v);
	return ((v->type ^ VF_DIRTY) & (VF_NUMBER | VF_USER | VF_DIRTY));
}

/* return 1 when value of v corresponds to true, 0 otherwise */
static int istrue(var *v)
{
	if (is_numeric(v))
		return (v->number != 0);
	return (v->string && v->string[0]);
}

/* ------- awk program text parsing ------- */

/* Parse next token pointed by global pos, place results into global t_XYZ variables.
 * If token isn't expected, print error message and die.
 * Return token class (also store it in t_tclass).
 */
static uint32_t next_token(uint32_t expected)
{
#define concat_inserted (G1.next_token__concat_inserted)
#define save_tclass     (G1.next_token__save_tclass)
#define save_info       (G1.next_token__save_info)

	char *p;
	const char *tl;
	const uint32_t *ti;
	uint32_t tc, last_token_class;

	last_token_class = t_tclass; /* t_tclass is initialized to TC_NEWLINE */

	debug_printf_parse("%s() expected(%x):", __func__, expected);
	debug_parse_print_tc(expected);
	debug_printf_parse("\n");

	if (t_rollback) {
		debug_printf_parse("%s: using rolled-back token\n", __func__);
		t_rollback = FALSE;
	} else if (concat_inserted) {
		debug_printf_parse("%s: using concat-inserted token\n", __func__);
		concat_inserted = FALSE;
		t_tclass = save_tclass;
		t_info = save_info;
	} else {
		p = g_pos;
		if (g_saved_ch != '\0') {
			*p = g_saved_ch;
			g_saved_ch = '\0';
		}
 readnext:
		p = skip_spaces(p);
		g_lineno = t_lineno;
		if (*p == '#')
			while (*p != '\n' && *p != '\0')
				p++;

		if (*p == '\0') {
			tc = TC_EOF;
			debug_printf_parse("%s: token found: TC_EOF\n", __func__);
		} else if (*p == '"') {
			/* it's a string */
			char *s = t_string = ++p;
			while (*p != '"') {
				char *pp;
				if (*p == '\0' || *p == '\n')
					syntax_error(EMSG_UNEXP_EOS);
				pp = p;
				*s++ = nextchar(&pp);
				p = pp;
			}
			p++;
			*s = '\0';
			tc = TC_STRING;
			debug_printf_parse("%s: token found:'%s' TC_STRING\n", __func__, t_string);
		} else if ((expected & TC_REGEXP) && *p == '/') {
			/* it's regexp */
			char *s	= t_string = ++p;
			while (*p != '/') {
				if (*p == '\0' || *p == '\n')
					syntax_error(EMSG_UNEXP_EOS);
				*s = *p++;
				if (*s++ == '\\') {
					char *pp = p;
					s[-1] = bb_process_escape_sequence((const char **)&pp);
					if (*p == '\\')
						*s++ = '\\';
					if (pp == p) {
						if (*p == '\0')
							syntax_error(EMSG_UNEXP_EOS);
						*s++ = *p++;
					} else
						p = pp;
				}
			}
			p++;
			*s = '\0';
			tc = TC_REGEXP;
			debug_printf_parse("%s: token found:'%s' TC_REGEXP\n", __func__, t_string);

		} else if (*p == '.' || isdigit(*p)) {
			/* it's a number */
			char *pp = p;
			t_double = my_strtod_or_hexoct(&pp);
			/* ^^^ awk only allows hex/oct consts in _program_, not in _input_ */
			p = pp;
			if (*p == '.')
				syntax_error(EMSG_UNEXP_TOKEN);
			tc = TC_NUMBER;
			debug_printf_parse("%s: token found:%f TC_NUMBER\n", __func__, t_double);
		} else {
			char *end_of_name;

			if (*p == '\n')
				t_lineno++;

			/* search for something known */
			tl = tokenlist;
			tc = 0x00000001;
			ti = tokeninfo;
			while (*tl) {
				int l = (unsigned char) *tl++;
				if (l == (unsigned char) NTCC) {
					tc <<= 1;
					continue;
				}
				/* if token class is expected,
				 * token matches,
				 * and it's not a longer word,
				 */
				if ((tc & (expected | TS_WORD | TC_NEWLINE))
				 && strncmp(p, tl, l) == 0
				 && !((tc & TS_WORD) && isalnum_(p[l]))
				) {
					/* then this is what we are looking for */
					t_info = *ti;
					debug_printf_parse("%s: token found:'%.*s' t_info:%x\n", __func__, l, p, t_info);
					p += l;
					goto token_found;
				}
				ti++;
				tl += l;
			}
			/* not a known token */

			/* is it a name? (var/array/function) */
			if (!isalnum_(*p))
				syntax_error(EMSG_UNEXP_TOKEN); /* no */
			/* yes */
			t_string = p;
			while (isalnum_(*p))
				p++;
			end_of_name = p;

			if (last_token_class == TC_FUNCDECL)
				/* eat space in "function FUNC (...) {...}" declaration */
				p = skip_spaces(p);
			else if (expected & TC_ARRAY) {
				/* eat space between array name and [ */
				char *s = skip_spaces(p);
				if (*s == '[') /* array ref, not just a name? */
					p = s;
			}
			/* else: do NOT consume whitespace after variable name!
			 * gawk allows definition "function FUNC (p) {...}" - note space,
			 * but disallows the call "FUNC (p)" because it isn't one -
			 * expression "v (a)" should NOT be parsed as TC_FUNCTION:
			 * it is a valid concatenation if "v" is a variable,
			 * not a function name (and type of name is not known at parse time).
			 */

			if (*p == '(') {
				p++;
				tc = TC_FUNCTION;
				debug_printf_parse("%s: token found:'%s' TC_FUNCTION\n", __func__, t_string);
			} else if (*p == '[') {
				p++;
				tc = TC_ARRAY;
				debug_printf_parse("%s: token found:'%s' TC_ARRAY\n", __func__, t_string);
			} else {
				tc = TC_VARIABLE;
				debug_printf_parse("%s: token found:'%s' TC_VARIABLE\n", __func__, t_string);
				if (end_of_name == p) {
					/* there is no space for trailing NUL in t_string!
					 * We need to save the char we are going to NUL.
					 * (we'll use it in future call to next_token())
					 */
					g_saved_ch = *end_of_name;
// especially pathological example is V="abc"; V.2 - it's V concatenated to .2
// (it evaluates to "abc0.2"). Because of this case, we can't simply cache
// '.' and analyze it later: we also have to *store it back* in next
// next_token(), in order to give my_strtod() the undamaged ".2" string.
				}
			}
			*end_of_name = '\0'; /* terminate t_string */
		}
 token_found:
		g_pos = p;

		/* skipping newlines in some cases */
		if ((last_token_class & TS_NOTERM) && (tc & TC_NEWLINE))
			goto readnext;

		/* insert concatenation operator when needed */
		debug_printf_parse("%s: concat_inserted if all nonzero: %x %x %x %x\n", __func__,
			(last_token_class & TS_CONCAT_L), (tc & TS_CONCAT_R), (expected & TS_BINOP),
			!(last_token_class == TC_LENGTH && tc == TC_LPAREN));
		if ((last_token_class & TS_CONCAT_L) && (tc & TS_CONCAT_R) && (expected & TS_BINOP)
		 && !(last_token_class == TC_LENGTH && tc == TC_LPAREN) /* but not for "length(..." */
		) {
			concat_inserted = TRUE;
			save_tclass = tc;
			save_info = t_info;
			tc = TC_BINOPX;
			t_info = OC_CONCAT | SS | PRECEDENCE(35);
		}

		t_tclass = tc;
		debug_printf_parse("%s: t_tclass=tc=%x\n", __func__, tc);
	}
	/* Are we ready for this? */
	if (!(t_tclass & expected)) {
		syntax_error((last_token_class & (TC_NEWLINE | TC_EOF)) ?
				EMSG_UNEXP_EOS : EMSG_UNEXP_TOKEN);
	}

	debug_printf_parse("%s: returning, t_double:%f t_tclass:", __func__, t_double);
	debug_parse_print_tc(t_tclass);
	debug_printf_parse("\n");

	return t_tclass;
#undef concat_inserted
#undef save_tclass
#undef save_info
}

static ALWAYS_INLINE void rollback_token(void)
{
	t_rollback = TRUE;
}

static node *new_node(uint32_t info)
{
	node *n;

	n = xzalloc(sizeof(node));
	n->info = info;
	n->lineno = g_lineno;
	return n;
}

static void mk_re_node(const char *s, node *n, regex_t *re)
{
	n->info = TI_REGEXP;
	n->l.re = re;
	xregcomp(re, s, REG_EXTENDED);
	xregcomp(re + 1, s, REG_EXTENDED | REG_ICASE);
}

static node *parse_expr(uint32_t);

static node *parse_lrparen_list(void)
{
	next_token(TC_LPAREN);
	return parse_expr(TC_RPAREN);
}

/* Parse expression terminated by given token, return ptr
 * to built subtree. Terminator is eaten by parse_expr */
static node *parse_expr(uint32_t term_tc)
{
	node sn;
	node *cn = &sn;
	node *getline_node;
	uint32_t tc, expected_tc;

	debug_printf_parse("%s() term_tc(%x):", __func__, term_tc);
	debug_parse_print_tc(term_tc);
	debug_printf_parse("\n");

	sn.info = LOWEST_PRECEDENCE;
	sn.r.n = sn.a.n = getline_node = NULL;
	expected_tc = TS_OPERAND | TS_UOPPRE | TC_REGEXP | term_tc;

	while (!((tc = next_token(expected_tc)) & term_tc)) {
		node *vn;

		if (getline_node && (t_info == TI_LESS)) {
			/* Attach input redirection (<) to getline node */
			debug_printf_parse("%s: input redir\n", __func__);
			cn = getline_node->l.n = new_node(OC_CONCAT | SS | PRECEDENCE(37));
			cn->a.n = getline_node;
			expected_tc = TS_OPERAND | TS_UOPPRE;
			getline_node = NULL;
			continue;
		}
		if (tc & (TS_BINOP | TC_UOPPOST)) {
			debug_printf_parse("%s: TS_BINOP | TC_UOPPOST tc:%x\n", __func__, tc);
			/* for binary and postfix-unary operators, jump back over
			 * previous operators with higher precedence */
			vn = cn;
			while (((t_info & PRIMASK) > (vn->a.n->info & PRIMASK2))
			    || (t_info == vn->info && t_info == TI_COLON)
			) {
				vn = vn->a.n;
				if (!vn->a.n) syntax_error(EMSG_UNEXP_TOKEN);
			}
			if (t_info == TI_TERNARY) /* "?" token */
//TODO: why?
				t_info += PRECEDENCE(6);
			cn = vn->a.n->r.n = new_node(t_info);
			cn->a.n = vn->a.n;
			if (tc & TS_BINOP) {
				cn->l.n = vn;

				/* Prevent:
				 * awk 'BEGIN { "qwe" = 1 }'
				 * awk 'BEGIN { 7 *= 7 }'
				 * awk 'BEGIN { length("qwe") = 1 }'
				 * awk 'BEGIN { (1+1) += 3 }'
				 */
				/* Assignment? (including *= and friends) */
				if (((t_info & OPCLSMASK) == OC_MOVE)
				 || ((t_info & OPCLSMASK) == OC_REPLACE)
				) {
					debug_printf_parse("%s: MOVE/REPLACE vn->info:%08x\n", __func__, vn->info);
					/* Left side is a (variable or array element)
					 * or function argument
					 * or $FIELD ?
					 */
					if ((vn->info & OPCLSMASK) != OC_VAR
					 && (vn->info & OPCLSMASK) != OC_FNARG
					 && (vn->info & OPCLSMASK) != OC_FIELD
					) {
						syntax_error(EMSG_UNEXP_TOKEN); /* no. bad */
					}
				}

				expected_tc = TS_OPERAND | TS_UOPPRE | TC_REGEXP;
				if (t_info == TI_PGETLINE) { /* "|" token */
					next_token(TC_GETLINE); /* must be folowed by "getline" */
					/* give maximum precedence to this pipe */
					cn->info &= ~PRIMASK; /* sets PRECEDENCE(0) */
					expected_tc = TS_OPERAND | TS_UOPPRE | TS_BINOP | term_tc;
				}
			} else {
				/* It was an unary postfix operator */
				cn->r.n = vn;
				expected_tc = TS_OPERAND | TS_UOPPRE | TS_BINOP | term_tc;
			}
			vn->a.n = cn;
			continue;
		}
		/* It wasn't a binary or postfix-unary operator */

		debug_printf_parse("%s: other, t_info:%x\n", __func__, t_info);
		/* for operands and prefix-unary operators, attach them
		 * to last node */
		vn = cn;
		cn = vn->r.n = new_node(t_info);
		cn->a.n = vn;

		expected_tc = TS_OPERAND | TS_UOPPRE | TC_REGEXP;
		if (t_info == TI_PREINC || t_info == TI_PREDEC)
			expected_tc = TS_LVALUE | TC_UOPPRE1;

		if (!(tc & (TS_OPERAND | TC_REGEXP)))
			continue;

		debug_printf_parse("%s: TS_OPERAND | TC_REGEXP\n", __func__);
		expected_tc = TS_UOPPRE | TC_UOPPOST | TS_BINOP | TS_OPERAND | term_tc;
		/* one should be very careful with switch on tclass -
		 * only simple tclasses should be used (TC_xyz, not TS_xyz) */
		switch (tc) {
			var *v;

		case TC_VARIABLE:
		case TC_ARRAY:
			debug_printf_parse("%s: TC_VARIABLE | TC_ARRAY\n", __func__);
			cn->info = OC_VAR;
			v = hash_search(ahash, t_string);
			if (v != NULL) {
				cn->info = OC_FNARG;
				cn->l.aidx = v->x.aidx;
			} else {
				cn->l.v = newvar(t_string);
			}
			if (tc & TC_ARRAY) {
				cn->info |= xS;
				cn->r.n = parse_expr(TC_ARRTERM);
			}
			break;

		case TC_NUMBER:
		case TC_STRING:
			debug_printf_parse("%s: TC_NUMBER | TC_STRING\n", __func__);
			cn->info = OC_CONST;
			v = cn->l.v = xzalloc(sizeof(var));
			if (tc & TC_NUMBER) {
				setvar_i(v, t_double);
			 } else {
				setvar_s(v, t_string);
			}
			expected_tc &= ~TC_UOPPOST; /* NUM++, "str"++ not allowed */
			break;

		case TC_REGEXP:
			debug_printf_parse("%s: TC_REGEXP\n", __func__);
			mk_re_node(t_string, cn, xzalloc(sizeof(regex_t)*2));
			break;

		case TC_FUNCTION:
			debug_printf_parse("%s: TC_FUNCTION\n", __func__);
			cn->info = OC_FUNC;
			cn->r.f = newfunc(t_string);
			cn->l.n = parse_expr(TC_RPAREN);
			break;

		case TC_LPAREN:
			debug_printf_parse("%s: TC_LPAREN\n", __func__);
			cn = vn->r.n = parse_expr(TC_RPAREN);
			if (!cn)
				syntax_error("Empty sequence");
			cn->a.n = vn;
			break;

		case TC_GETLINE:
			/* "getline" is a function, not a statement.
			 * Works in gawk:
			 *  r = ["SHELL CMD" | ] getline [VAR] [<"FILE"]
			 *  if (getline <"FILE" < 0) print "Can't read FILE"
			 *  while ("SHELL CMD" | getline > 0) ...
			 * Returns: 1 successful read, 0 EOF, -1 error (sets ERRNO)
			 */
			debug_printf_parse("%s: TC_GETLINE\n", __func__);
			getline_node = cn;
			expected_tc = TS_OPERAND | TS_UOPPRE | TS_BINOP | term_tc;
			break;

		case TC_BUILTIN:
			debug_printf_parse("%s: TC_BUILTIN\n", __func__);
			cn->l.n = parse_lrparen_list();
			break;

		case TC_LENGTH:
			debug_printf_parse("%s: TC_LENGTH\n", __func__);
			tc = next_token(TC_LPAREN /* length(...) */
				| TC_SEMICOL   /* length; */
				| TC_NEWLINE   /* length<newline> */
				| TC_RBRACE    /* length } */
				| TC_BINOPX    /* length <op> NUM */
				| TC_COMMA     /* print length, 1 */
			);
			if (tc != TC_LPAREN)
				rollback_token();
			else {
				/* It was a "(" token. Handle just like TC_BUILTIN */
				cn->l.n = parse_expr(TC_RPAREN);
			}
			break;
		}
	} /* while() */

	debug_printf_parse("%s() returns %p\n", __func__, sn.r.n);
	return sn.r.n;
}

/* add node to chain. Return ptr to alloc'd node */
static node *chain_node(uint32_t info)
{
	node *n;

	if (!seq->first)
		seq->first = seq->last = new_node(0);

	if (seq->programname != g_progname) {
		seq->programname = g_progname;
		n = chain_node(OC_NEWSOURCE);
		n->l.new_progname = g_progname;
	}

	n = seq->last;
	n->info = info;
	seq->last = n->a.n = new_node(OC_DONE);

	return n;
}

static void chain_expr(uint32_t info)
{
	node *n;

	n = chain_node(info);

	n->l.n = parse_expr(TC_SEMICOL | TC_NEWLINE | TC_RBRACE);
	if ((info & OF_REQUIRED) && !n->l.n)
		syntax_error(EMSG_TOO_FEW_ARGS);

	if (t_tclass & TC_RBRACE)
		rollback_token();
}

static void chain_group(void);

static node *chain_loop(node *nn)
{
	node *n, *n2, *save_brk, *save_cont;

	save_brk = break_ptr;
	save_cont = continue_ptr;

	n = chain_node(OC_BR | Vx);
	continue_ptr = new_node(OC_EXEC);
	break_ptr = new_node(OC_EXEC);
	chain_group();
	n2 = chain_node(OC_EXEC | Vx);
	n2->l.n = nn;
	n2->a.n = n;
	continue_ptr->a.n = n2;
	break_ptr->a.n = n->r.n = seq->last;

	continue_ptr = save_cont;
	break_ptr = save_brk;

	return n;
}

static void chain_until_rbrace(void)
{
	uint32_t tc;
	while ((tc = next_token(TS_GRPSEQ | TC_RBRACE)) != TC_RBRACE) {
		debug_printf_parse("%s: !TC_RBRACE\n", __func__);
		if (tc == TC_NEWLINE)
			continue;
		rollback_token();
		chain_group();
	}
	debug_printf_parse("%s: TC_RBRACE\n", __func__);
}

/* parse group and attach it to chain */
static void chain_group(void)
{
	uint32_t tc;
	node *n, *n2, *n3;

	do {
		tc = next_token(TS_GRPSEQ);
	} while (tc == TC_NEWLINE);

	if (tc == TC_LBRACE) {
		debug_printf_parse("%s: TC_LBRACE\n", __func__);
		chain_until_rbrace();
		return;
	}
	if (tc & (TS_OPSEQ | TC_SEMICOL)) {
		debug_printf_parse("%s: TS_OPSEQ | TC_SEMICOL\n", __func__);
		rollback_token();
		chain_expr(OC_EXEC | Vx);
		return;
	}

	/* TS_STATEMNT */
	debug_printf_parse("%s: TS_STATEMNT(?)\n", __func__);
	switch (t_info & OPCLSMASK) {
	case ST_IF:
		debug_printf_parse("%s: ST_IF\n", __func__);
		n = chain_node(OC_BR | Vx);
		n->l.n = parse_lrparen_list();
		chain_group();
		n2 = chain_node(OC_EXEC);
		n->r.n = seq->last;
		if (next_token(TS_GRPSEQ | TC_RBRACE | TC_ELSE) == TC_ELSE) {
			chain_group();
			n2->a.n = seq->last;
		} else {
			rollback_token();
		}
		break;

	case ST_WHILE:
		debug_printf_parse("%s: ST_WHILE\n", __func__);
		n2 = parse_lrparen_list();
		n = chain_loop(NULL);
		n->l.n = n2;
		break;

	case ST_DO:
		debug_printf_parse("%s: ST_DO\n", __func__);
		n2 = chain_node(OC_EXEC);
		n = chain_loop(NULL);
		n2->a.n = n->a.n;
		next_token(TC_WHILE);
		n->l.n = parse_lrparen_list();
		break;

	case ST_FOR:
		debug_printf_parse("%s: ST_FOR\n", __func__);
		next_token(TC_LPAREN);
		n2 = parse_expr(TC_SEMICOL | TC_RPAREN);
		if (t_tclass & TC_RPAREN) {	/* for (I in ARRAY) */
			if (!n2 || n2->info != TI_IN)
				syntax_error(EMSG_UNEXP_TOKEN);
			n = chain_node(OC_WALKINIT | VV);
			n->l.n = n2->l.n;
			n->r.n = n2->r.n;
			n = chain_loop(NULL);
			n->info = OC_WALKNEXT | Vx;
			n->l.n = n2->l.n;
		} else {			/* for (;;) */
			n = chain_node(OC_EXEC | Vx);
			n->l.n = n2;
			n2 = parse_expr(TC_SEMICOL);
			n3 = parse_expr(TC_RPAREN);
			n = chain_loop(n3);
			n->l.n = n2;
			if (!n2)
				n->info = OC_EXEC;
		}
		break;

	case OC_PRINT:
	case OC_PRINTF:
		debug_printf_parse("%s: OC_PRINT[F]\n", __func__);
		n = chain_node(t_info);
		n->l.n = parse_expr(TC_SEMICOL | TC_NEWLINE | TC_OUTRDR | TC_RBRACE);
		if (t_tclass & TC_OUTRDR) {
			n->info |= t_info;
			n->r.n = parse_expr(TC_SEMICOL | TC_NEWLINE | TC_RBRACE);
		}
		if (t_tclass & TC_RBRACE)
			rollback_token();
		break;

	case OC_BREAK:
		debug_printf_parse("%s: OC_BREAK\n", __func__);
		n = chain_node(OC_EXEC);
		if (!break_ptr)
			syntax_error("'break' not in a loop");
		n->a.n = break_ptr;
		chain_expr(t_info);
		break;

	case OC_CONTINUE:
		debug_printf_parse("%s: OC_CONTINUE\n", __func__);
		n = chain_node(OC_EXEC);
		if (!continue_ptr)
			syntax_error("'continue' not in a loop");
		n->a.n = continue_ptr;
		chain_expr(t_info);
		break;

	/* delete, next, nextfile, return, exit */
	default:
		debug_printf_parse("%s: default\n", __func__);
		chain_expr(t_info);
	}
}

static void parse_program(char *p)
{
	debug_printf_parse("%s()\n", __func__);

	g_pos = p;
	t_lineno = 1;
	for (;;) {
		uint32_t tclass;

		tclass = next_token(TS_OPSEQ | TC_LBRACE | TC_BEGIN | TC_END | TC_FUNCDECL
			| TC_EOF | TC_NEWLINE /* but not TC_SEMICOL */);
 got_tok:
		if (tclass == TC_EOF) {
			debug_printf_parse("%s: TC_EOF\n", __func__);
			break;
		}
		if (tclass == TC_NEWLINE) {
			debug_printf_parse("%s: TC_NEWLINE\n", __func__);
			continue;
		}
		if (tclass == TC_BEGIN) {
			debug_printf_parse("%s: TC_BEGIN\n", __func__);
			seq = &beginseq;
			/* ensure there is no newline between BEGIN and { */
			next_token(TC_LBRACE);
			chain_until_rbrace();
			goto next_tok;
		}
		if (tclass == TC_END) {
			debug_printf_parse("%s: TC_END\n", __func__);
			seq = &endseq;
			/* ensure there is no newline between END and { */
			next_token(TC_LBRACE);
			chain_until_rbrace();
			goto next_tok;
		}
		if (tclass == TC_FUNCDECL) {
			func *f;

			debug_printf_parse("%s: TC_FUNCDECL\n", __func__);
			next_token(TC_FUNCTION);
			f = newfunc(t_string);
			if (f->defined)
				syntax_error("Duplicate function");
			f->defined = 1;
			//f->body.first = NULL; - already is
			//f->nargs = 0; - already is
			/* func arg list: comma sep list of args, and a close paren */
			for (;;) {
				var *v;
				if (next_token(TC_VARIABLE | TC_RPAREN) == TC_RPAREN) {
					if (f->nargs == 0)
						break; /* func() is ok */
					/* func(a,) is not ok */
					syntax_error(EMSG_UNEXP_TOKEN);
				}
				v = findvar(ahash, t_string);
				v->x.aidx = f->nargs++;
				/* Arg followed either by end of arg list or 1 comma */
				if (next_token(TC_COMMA | TC_RPAREN) == TC_RPAREN)
					break;
				/* it was a comma, we ate it */
			}
			seq = &f->body;
			/* ensure there is { after "func F(...)" - but newlines are allowed */
			while (next_token(TC_LBRACE | TC_NEWLINE) == TC_NEWLINE)
				continue;
			chain_until_rbrace();
			hash_clear(ahash);
			goto next_tok;
		}
		seq = &mainseq;
		if (tclass & TS_OPSEQ) {
			node *cn;

			debug_printf_parse("%s: TS_OPSEQ\n", __func__);
			rollback_token();
			cn = chain_node(OC_TEST);
			cn->l.n = parse_expr(TC_SEMICOL | TC_NEWLINE | TC_EOF | TC_LBRACE);
			if (t_tclass == TC_LBRACE) {
				debug_printf_parse("%s: TC_LBRACE\n", __func__);
				chain_until_rbrace();
			} else {
				/* no action, assume default "{ print }" */
				debug_printf_parse("%s: !TC_LBRACE\n", __func__);
				chain_node(OC_PRINT);
			}
			cn->r.n = mainseq.last;
			goto next_tok;
		}
		/* tclass == TC_LBRACE */
		debug_printf_parse("%s: TC_LBRACE(?)\n", __func__);
		chain_until_rbrace();
 next_tok:
		/* Same as next_token() at the top of the loop, + TC_SEMICOL */
		tclass = next_token(TS_OPSEQ | TC_LBRACE | TC_BEGIN | TC_END | TC_FUNCDECL
			| TC_EOF | TC_NEWLINE | TC_SEMICOL);
		/* gawk allows many newlines, but does not allow more than one semicolon:
		 *  BEGIN {...}<newline>;<newline>;
		 * would complain "each rule must have a pattern or an action part".
		 * Same message for
		 *  ; BEGIN {...}
		 */
		if (tclass != TC_SEMICOL)
			goto got_tok; /* use this token */
		/* else: loop back - ate the semicolon, get and use _next_ token */
	} /* for (;;) */
}

/* -------- program execution part -------- */

/* temporary variables allocator */
static var *nvalloc(int sz)
{
	return xzalloc(sz * sizeof(var));
}

static void nvfree(var *v, int sz)
{
	var *p = v;

	while (--sz >= 0) {
		if ((p->type & (VF_ARRAY | VF_CHILD)) == VF_ARRAY) {
			clear_array(iamarray(p));
			free(p->x.array->items);
			free(p->x.array);
		}
		if (p->type & VF_WALK) {
			walker_list *n;
			walker_list *w = p->x.walker;
			debug_printf_walker("nvfree: freeing walker @%p\n", &p->x.walker);
			p->x.walker = NULL;
			while (w) {
				n = w->prev;
				debug_printf_walker(" free(%p)\n", w);
				free(w);
				w = n;
			}
		}
		clrvar(p);
		p++;
	}

	free(v);
}

static node *mk_splitter(const char *s, tsplitter *spl)
{
	regex_t *re;
	node *n;

	re = spl->re;
	n = &spl->n;
	if (n->info == TI_REGEXP) {
		regfree(re);
		regfree(re + 1);
	}
	if (s[0] && s[1]) { /* strlen(s) > 1 */
		mk_re_node(s, n, re);
	} else {
		n->info = (uint32_t) s[0];
	}

	return n;
}

static var *evaluate(node *, var *);

/* Use node as a regular expression. Supplied with node ptr and regex_t
 * storage space. Return ptr to regex (if result points to preg, it should
 * be later regfree'd manually).
 */
static regex_t *as_regex(node *op, regex_t *preg)
{
	int cflags;
	const char *s;

	if (op->info == TI_REGEXP) {
		return &op->l.re[icase];
	}

	//tmpvar = nvalloc(1);
#define TMPVAR (&G.as_regex__tmpvar)
	// We use a single "static" tmpvar (instead of on-stack or malloced one)
	// to decrease memory consumption in deeply-recursive awk programs.
	// The rule to work safely is to never call evaluate() while our static
	// TMPVAR's value is still needed.
	s = getvar_s(evaluate(op, TMPVAR));

	cflags = icase ? REG_EXTENDED | REG_ICASE : REG_EXTENDED;
	/* Testcase where REG_EXTENDED fails (unpaired '{'):
	 * echo Hi | awk 'gsub("@(samp|code|file)\{","");'
	 * gawk 3.1.5 eats this. We revert to ~REG_EXTENDED
	 * (maybe gsub is not supposed to use REG_EXTENDED?).
	 */
	if (regcomp(preg, s, cflags)) {
		cflags &= ~REG_EXTENDED;
		xregcomp(preg, s, cflags);
	}
	//nvfree(tmpvar, 1);
#undef TMPVAR
	return preg;
}

/* gradually increasing buffer.
 * note that we reallocate even if n == old_size,
 * and thus there is at least one extra allocated byte.
 */
static char* qrealloc(char *b, int n, int *size)
{
	if (!b || n >= *size) {
		*size = n + (n>>1) + 80;
		b = xrealloc(b, *size);
	}
	return b;
}

/* resize field storage space */
static void fsrealloc(int size)
{
	int i, newsize;

	if ((unsigned)size >= num_alloc_fields) {
		/* Sanity cap, easier than catering for over/underflows */
		if ((unsigned)size > 0xffffff)
			bb_die_memory_exhausted();

		i = num_alloc_fields;
		num_alloc_fields = size + 16;

		newsize = num_alloc_fields * sizeof(Fields[0]);
		debug_printf_eval("fsrealloc: xrealloc(%p, %u)\n", Fields, newsize);
		Fields = xrealloc(Fields, newsize);
		debug_printf_eval("fsrealloc: Fields=%p..%p\n", Fields, (char*)Fields + newsize - 1);
		/* ^^^ did Fields[] move? debug aid for L.v getting "upstaged" by R.v in evaluate() */

		for (; i < num_alloc_fields; i++) {
			Fields[i].type = VF_SPECIAL | VF_DIRTY;
			Fields[i].string = NULL;
		}
	}
	/* if size < num_fields, clear extra field variables */
	for (i = size; i < num_fields; i++) {
		clrvar(Fields + i);
	}
	num_fields = size;
}

static int regexec1_nonempty(const regex_t *preg, const char *s, regmatch_t pmatch[])
{
	int r = regexec(preg, s, 1, pmatch, 0);
	if (r == 0 && pmatch[0].rm_eo == 0) {
		/* For example, happens when FS can match
		 * an empty string (awk -F ' *'). Logically,
		 * this should split into one-char fields.
		 * However, gawk 5.0.1 searches for first
		 * _non-empty_ separator string match:
		 */
		size_t ofs = 0;
		do {
			ofs++;
			if (!s[ofs])
				return REG_NOMATCH;
			regexec(preg, s + ofs, 1, pmatch, 0);
		} while (pmatch[0].rm_eo == 0);
		pmatch[0].rm_so += ofs;
		pmatch[0].rm_eo += ofs;
	}
	return r;
}

static int awk_split(const char *s, node *spl, char **slist)
{
	int n;
	char c[4];
	char *s1;

	/* in worst case, each char would be a separate field */
	*slist = s1 = xzalloc(strlen(s) * 2 + 3);
	strcpy(s1, s);

	c[0] = c[1] = (char)spl->info;
	c[2] = c[3] = '\0';
	if (*getvar_s(intvar[RS]) == '\0')
		c[2] = '\n';

	n = 0;
	if (spl->info == TI_REGEXP) {  /* regex split */
		if (!*s)
			return n; /* "": zero fields */
		n++; /* at least one field will be there */
		do {
			int l;
			regmatch_t pmatch[1];

			l = strcspn(s, c+2); /* len till next NUL or \n */
			if (regexec1_nonempty(&spl->l.re[icase], s, pmatch) == 0
			 && pmatch[0].rm_so <= l
			) {
				/* if (pmatch[0].rm_eo == 0) ... - impossible */
				l = pmatch[0].rm_so;
				n++; /* we saw yet another delimiter */
			} else {
				pmatch[0].rm_eo = l;
				if (s[l])
					pmatch[0].rm_eo++;
			}
			s1 = mempcpy(s1, s, l);
			*s1++ = '\0';
			s += pmatch[0].rm_eo;
		} while (*s);

		/* echo a-- | awk -F-- '{ print NF, length($NF), $NF }'
		 * should print "2 0 ":
		 */
		*s1 = '\0';

		return n;
	}
	if (c[0] == '\0') {  /* null split */
		while (*s) {
			*s1++ = *s++;
			*s1++ = '\0';
			n++;
		}
		return n;
	}
	if (c[0] != ' ') {  /* single-character split */
		if (icase) {
			c[0] = toupper(c[0]);
			c[1] = tolower(c[1]);
		}
		if (*s1)
			n++;
		while ((s1 = strpbrk(s1, c)) != NULL) {
			*s1++ = '\0';
			n++;
		}
		return n;
	}
	/* space split: "In the special case that FS is a single space,
	 * fields are separated by runs of spaces and/or tabs and/or newlines"
	 */
	while (*s) {
		/* s = skip_whitespace(s); -- WRONG (also skips \v \f \r) */
		while (*s == ' ' || *s == '\t' || *s == '\n')
			s++;
		if (!*s)
			break;
		n++;
		while (*s && !(*s == ' ' || *s == '\t' || *s == '\n'))
			*s1++ = *s++;
		*s1++ = '\0';
	}
	return n;
}

static void split_f0(void)
{
/* static char *fstrings; */
#define fstrings (G.split_f0__fstrings)

	int i, n;
	char *s;

	if (is_f0_split)
		return;

	is_f0_split = TRUE;
	free(fstrings);
	fsrealloc(0);
	n = awk_split(getvar_s(intvar[F0]), &fsplitter.n, &fstrings);
	fsrealloc(n);
	s = fstrings;
	for (i = 0; i < n; i++) {
		Fields[i].string = nextword(&s);
		Fields[i].type |= (VF_FSTR | VF_USER | VF_DIRTY);
	}

	/* set NF manually to avoid side effects */
	clrvar(intvar[NF]);
	intvar[NF]->type = VF_NUMBER | VF_SPECIAL;
	intvar[NF]->number = num_fields;
#undef fstrings
}

/* perform additional actions when some internal variables changed */
static void handle_special(var *v)
{
	int n;
	char *b;
	const char *sep, *s;
	int sl, l, len, i, bsize;

	if (!(v->type & VF_SPECIAL))
		return;

	if (v == intvar[NF]) {
		n = (int)getvar_i(v);
		if (n < 0)
			syntax_error("NF set to negative value");
		fsrealloc(n);

		/* recalculate $0 */
		sep = getvar_s(intvar[OFS]);
		sl = strlen(sep);
		b = NULL;
		len = 0;
		for (i = 0; i < n; i++) {
			s = getvar_s(&Fields[i]);
			l = strlen(s);
			if (b) {
				memcpy(b+len, sep, sl);
				len += sl;
			}
			b = qrealloc(b, len+l+sl, &bsize);
			memcpy(b+len, s, l);
			len += l;
		}
		if (b)
			b[len] = '\0';
		setvar_p(intvar[F0], b);
		is_f0_split = TRUE;

	} else if (v == intvar[F0]) {
		is_f0_split = FALSE;

	} else if (v == intvar[FS]) {
		/*
		 * The POSIX-2008 standard says that changing FS should have no effect on the
		 * current input line, but only on the next one. The language is:
		 *
		 * > Before the first reference to a field in the record is evaluated, the record
		 * > shall be split into fields, according to the rules in Regular Expressions,
		 * > using the value of FS that was current at the time the record was read.
		 *
		 * So, split up current line before assignment to FS:
		 */
		split_f0();

		mk_splitter(getvar_s(v), &fsplitter);
	} else if (v == intvar[RS]) {
		mk_splitter(getvar_s(v), &rsplitter);
	} else if (v == intvar[IGNORECASE]) {
		icase = istrue(v);
	} else {				/* $n */
		n = getvar_i(intvar[NF]);
		setvar_i(intvar[NF], n > v-Fields ? n : v-Fields+1);
		/* right here v is invalid. Just to note... */
	}
}

/* step through func/builtin/etc arguments */
static node *nextarg(node **pn)
{
	node *n;

	n = *pn;
	if (n && n->info == TI_COMMA) {
		*pn = n->r.n;
		n = n->l.n;
	} else {
		*pn = NULL;
	}
	return n;
}

static void hashwalk_init(var *v, xhash *array)
{
	hash_item *hi;
	unsigned i;
	walker_list *w;
	walker_list *prev_walker;

	if (v->type & VF_WALK) {
		prev_walker = v->x.walker;
	} else {
		v->type |= VF_WALK;
		prev_walker = NULL;
	}
	debug_printf_walker("hashwalk_init: prev_walker:%p\n", prev_walker);

	w = v->x.walker = xzalloc(sizeof(*w) + array->glen + 1); /* why + 1? */
	debug_printf_walker(" walker@%p=%p\n", &v->x.walker, w);
	w->cur = w->end = w->wbuf;
	w->prev = prev_walker;
	for (i = 0; i < array->csize; i++) {
		hi = array->items[i];
		while (hi) {
			w->end = stpcpy(w->end, hi->name) + 1;
			hi = hi->next;
		}
	}
}

static int hashwalk_next(var *v)
{
	walker_list *w = v->x.walker;

	if (w->cur >= w->end) {
		walker_list *prev_walker = w->prev;

		debug_printf_walker("end of iteration, free(walker@%p:%p), prev_walker:%p\n", &v->x.walker, w, prev_walker);
		free(w);
		v->x.walker = prev_walker;
		return FALSE;
	}

	setvar_s(v, nextword(&w->cur));
	return TRUE;
}

/* evaluate node, return 1 when result is true, 0 otherwise */
static int ptest(node *pattern)
{
	// We use a single "static" tmpvar (instead of on-stack or malloced one)
	// to decrease memory consumption in deeply-recursive awk programs.
	// The rule to work safely is to never call evaluate() while our static
	// TMPVAR's value is still needed.
	return istrue(evaluate(pattern, &G.ptest__tmpvar));
}

/* read next record from stream rsm into a variable v */
static int awk_getline(rstream *rsm, var *v)
{
	char *b;
	regmatch_t pmatch[1];
	int p, pp;
	int fd, so, eo, retval, rp;
	char *m, *s;

	debug_printf_eval("entered %s()\n", __func__);

	/* we're using our own buffer since we need access to accumulating
	 * characters
	 */
	fd = fileno(rsm->F);
	m = rsm->buffer;
	if (!m)
		m = qrealloc(m, 256, &rsm->size);
	p = rsm->pos;
	rp = 0;
	pp = 0;

	do {
		b = m + rsm->adv;
		so = eo = p;
		retval = 1;
		if (p > 0) {
			char c = (char) rsplitter.n.info;
			if (rsplitter.n.info == TI_REGEXP) {
				if (regexec(&rsplitter.n.l.re[icase],
							b, 1, pmatch, 0) == 0
				) {
					so = pmatch[0].rm_so;
					eo = pmatch[0].rm_eo;
					if (b[eo] != '\0')
						break;
				}
			} else if (c != '\0') {
				s = strchr(b+pp, c);
				if (!s)
					s = memchr(b+pp, '\0', p - pp);
				if (s) {
					so = eo = s-b;
					eo++;
					break;
				}
			} else {
				while (b[rp] == '\n')
					rp++;
				s = strstr(b+rp, "\n\n");
				if (s) {
					so = eo = s-b;
					while (b[eo] == '\n')
						eo++;
					if (b[eo] != '\0')
						break;
				}
			}
		}

		if (rsm->adv > 0) {
			memmove(m, m+rsm->adv, p+1);
			b = m;
			rsm->adv = 0;
		}

		b = m = qrealloc(m, p+128, &rsm->size);
		pp = p;
		p += safe_read(fd, b+p, rsm->size - p - 1);
		if (p < pp) {
			p = 0;
			retval = 0;
			setvar_ERRNO();
		}
		b[p] = '\0';
	} while (p > pp);

	if (p == 0) {
		retval--;
	} else {
		setvar_sn(v, b+rp, so-rp);
		v->type |= VF_USER;
		setvar_sn(intvar[RT], b+so, eo-so);
	}

	rsm->buffer = m;
	rsm->adv += eo;
	rsm->pos = p - eo;

	debug_printf_eval("returning from %s(): %d\n", __func__, retval);

	return retval;
}

/* formatted output into an allocated buffer, return ptr to buffer */
#if !ENABLE_FEATURE_AWK_GNU_EXTENSIONS
# define awk_printf(a, b) awk_printf(a)
#endif
static char *awk_printf(node *n, size_t *len)
{
	char *b;
	char *fmt, *f;
	size_t i;

	//tmpvar = nvalloc(1);
#define TMPVAR (&G.awk_printf__tmpvar)
	// We use a single "static" tmpvar (instead of on-stack or malloced one)
	// to decrease memory consumption in deeply-recursive awk programs.
	// The rule to work safely is to never call evaluate() while our static
	// TMPVAR's value is still needed.
	fmt = f = xstrdup(getvar_s(evaluate(nextarg(&n), TMPVAR)));
	// ^^^^^^^^^ here we immediately strdup() the value, so the later call
	// to evaluate() potentially recursing into another awk_printf() can't
	// mangle the value.

	b = NULL;
	i = 0;
	while (1) { /* "print one format spec" loop */
		char *s;
		char c;
		char sv;
		var *arg;
		size_t slen;

		/* Find end of the next format spec, or end of line */
		s = f;
		while (1) {
			c = *f;
			if (!c) /* no percent chars found at all */
				goto nul;
			f++;
			if (c == '%')
				break;
		}
		/* we are past % in "....%..." */
		c = *f;
		if (!c) /* "....%" */
			goto nul;
		if (c == '%') { /* "....%%...." */
			slen = f - s;
			s = xstrndup(s, slen);
			f++;
			goto append; /* print "....%" part verbatim */
		}
		while (1) {
			if (isalpha(c))
				break;
			if (c == '*') /* gawk supports %*d and %*.*f, we don't... */
				syntax_error("%*x formats are not supported");
			c = *++f;
			if (!c) { /* "....%...." and no letter found after % */
				/* Example: awk 'BEGIN { printf "^^^%^^^\n"; }' */
 nul:
				slen = f - s;
				goto tail; /* print remaining string, exit loop */
			}
		}
		/* we are at A in "....%...A..." */

		arg = evaluate(nextarg(&n), TMPVAR);

		/* Result can be arbitrarily long. Example:
		 *  printf "%99999s", "BOOM"
		 */
		sv = *++f;
		*f = '\0';
		if (c == 'c') {
			char cc = is_numeric(arg) ? getvar_i(arg) : *getvar_s(arg);
			char *r = xasprintf(s, cc ? cc : '^' /* else strlen will be wrong */);
			slen = strlen(r);
			if (cc == '\0') /* if cc is NUL, re-format the string with it */
				sprintf(r, s, cc);
			s = r;
		} else {
			if (c == 's') {
				s = xasprintf(s, getvar_s(arg));
			} else {
				double d = getvar_i(arg);
				if (strchr("diouxX", c)) {
//TODO: make it wider here (%x -> %llx etc)?
//Can even print the value into a temp string with %.0f,
//then replace diouxX with s and print that string.
//This will correctly print even very large numbers,
//but some replacements are not equivalent:
//%09d -> %09s: breaks zero-padding;
//%+d -> %+s: won't prepend +; etc
					s = xasprintf(s, (int)d);
				} else if (strchr("eEfFgGaA", c)) {
					s = xasprintf(s, d);
				} else {
					/* gawk 5.1.1 printf("%W") prints "%W", does not error out */
					s = xstrndup(s, f - s);
				}
			}
			slen = strlen(s);
		}
		*f = sv;
 append:
		if (i == 0) {
			b = s;
			i = slen;
			continue;
		}
 tail:
		b = xrealloc(b, i + slen + 1);
		strcpy(b + i, s);
		i += slen;
		if (!c) /* s is NOT allocated and this is the last part of string? */
			break;
		free(s);
	}

	free(fmt);
	//nvfree(tmpvar, 1);
#undef TMPVAR

#if ENABLE_FEATURE_AWK_GNU_EXTENSIONS
	if (len)
		*len = i;
#endif
	return b;
}

/* Common substitution routine.
 * Replace (nm)'th substring of (src) that matches (rn) with (repl),
 * store result into (dest), return number of substitutions.
 * If nm = 0, replace all matches.
 * If src or dst is NULL, use $0.
 * If subexp != 0, enable subexpression matching (\0-\9).
 */
static int awk_sub(node *rn, const char *repl, int nm, var *src, var *dest /*,int subexp*/)
{
	char *resbuf;
	const char *sp;
	int match_no, residx, replen, resbufsize;
	int regexec_flags;
	regmatch_t pmatch[10];
	regex_t sreg, *regex;
	/* True only if called to implement gensub(): */
	int subexp = (src != dest);
#if defined(REG_STARTEND)
	const char *src_string;
	size_t src_strlen;
	regexec_flags = REG_STARTEND;
#else
	regexec_flags = 0;
#endif
	resbuf = NULL;
	residx = 0;
	match_no = 0;
	regex = as_regex(rn, &sreg);
	sp = getvar_s(src ? src : intvar[F0]);
#if defined(REG_STARTEND)
	src_string = sp;
	src_strlen = strlen(src_string);
#endif
	replen = strlen(repl);
	for (;;) {
		int so, eo;

#if defined(REG_STARTEND)
// REG_STARTEND: "This flag is a BSD extension, not present in POSIX"
		size_t start_ofs = sp - src_string;
		pmatch[0].rm_so = start_ofs;
		pmatch[0].rm_eo = src_strlen;
		if (regexec(regex, src_string, 10, pmatch, regexec_flags) != 0)
			break;
		eo = pmatch[0].rm_eo - start_ofs;
		so = pmatch[0].rm_so - start_ofs;
#else
// BUG:
// gsub(/\<b*/,"") on "abc" matches empty string at "a...",
// advances sp one char (see "Empty match" comment later) to "bc"
// ... and erroneously matches "b" even though it is NOT at the word start.
		enum { start_ofs = 0 };
		if (regexec(regex, sp, 10, pmatch, regexec_flags) != 0)
			break;
		so = pmatch[0].rm_so;
		eo = pmatch[0].rm_eo;
#endif

		//bb_error_msg("match %u: [%u,%u] '%s'%p", match_no+1, so, eo, sp,sp);
		resbuf = qrealloc(resbuf, residx + eo + replen, &resbufsize);
		memcpy(resbuf + residx, sp, eo);
		residx += eo;
		if (++match_no >= nm) {
			const char *s;
			int bslash;

			/* replace */
			residx -= (eo - so);
			bslash = 0;
			for (s = repl; *s; s++) {
				char c = *s;
				if (c == '\\' && s[1]) {
					bslash ^= 1;
					if (bslash)
						continue;
				}
				if ((!bslash && c == '&')
				 || (subexp && bslash && c >= '0' && c <= '9')
				) {
					int n, j = 0;
					if (c != '&') {
						j = c - '0';
					}
					n = pmatch[j].rm_eo - pmatch[j].rm_so;
					resbuf = qrealloc(resbuf, residx + replen + n, &resbufsize);
					memcpy(resbuf + residx, sp + pmatch[j].rm_so - start_ofs, n);
					residx += n;
				} else
					resbuf[residx++] = c;
				bslash = 0;
			}
		}

		sp += eo;
		if (match_no == nm)
			break;
		if (eo == so) {
			/* Empty match (e.g. "b*" will match anywhere).
			 * Advance by one char. */
			/* Subtle: this is safe only because
			 * qrealloc allocated at least one extra byte */
			resbuf[residx] = *sp;
			if (*sp == '\0')
				goto ret;
			sp++;
			residx++;
		}
		regexec_flags |= REG_NOTBOL;
	}

	resbuf = qrealloc(resbuf, residx + strlen(sp), &resbufsize);
	strcpy(resbuf + residx, sp);
 ret:
	//bb_error_msg("end sp:'%s'%p", sp,sp);
	setvar_p(dest ? dest : intvar[F0], resbuf);
	if (regex == &sreg)
		regfree(regex);
	return match_no;
}

static NOINLINE int do_mktime(const char *ds)
{
	struct tm then;
	int count;

	/*memset(&then, 0, sizeof(then)); - not needed */
	then.tm_isdst = -1; /* default is unknown */

	/* manpage of mktime says these fields are ints,
	 * so we can sscanf stuff directly into them */
	count = sscanf(ds, "%u %u %u %u %u %u %d",
		&then.tm_year, &then.tm_mon, &then.tm_mday,
		&then.tm_hour, &then.tm_min, &then.tm_sec,
		&then.tm_isdst);

	if (count < 6
	 || (unsigned)then.tm_mon < 1
	 || (unsigned)then.tm_year < 1900
	) {
		return -1;
	}

	then.tm_mon -= 1;
	then.tm_year -= 1900;

	return mktime(&then);
}

/* Reduce stack usage in exec_builtin() by keeping match() code separate */
static NOINLINE var *do_match(node *an1, const char *as0)
{
	regmatch_t pmatch[1];
	regex_t sreg, *re;
	int n, start, len;

	re = as_regex(an1, &sreg);
	n = regexec(re, as0, 1, pmatch, 0);
	if (re == &sreg)
		regfree(re);
	start = 0;
	len = -1;
	if (n == 0) {
		start = pmatch[0].rm_so + 1;
		len = pmatch[0].rm_eo - pmatch[0].rm_so;
	}
	setvar_i(newvar("RLENGTH"), len);
	return setvar_i(newvar("RSTART"), start);
}

/* Reduce stack usage in evaluate() by keeping builtins' code separate */
static NOINLINE var *exec_builtin(node *op, var *res)
{
#define tspl (G.exec_builtin__tspl)

	var *tmpvars;
	node *an[4];
	var *av[4];
	const char *as[4];
	node *spl;
	uint32_t isr, info;
	int nargs;
	time_t tt;
	int i, l, ll, n;

	tmpvars = nvalloc(4);
#define TMPVAR0 (tmpvars)
#define TMPVAR1 (tmpvars + 1)
#define TMPVAR2 (tmpvars + 2)
#define TMPVAR3 (tmpvars + 3)
#define TMPVAR(i) (tmpvars + (i))
	isr = info = op->info;
	op = op->l.n;

	av[2] = av[3] = NULL;
	for (i = 0; i < 4 && op; i++) {
		an[i] = nextarg(&op);
		if (isr & 0x09000000) {
			av[i] = evaluate(an[i], TMPVAR(i));
			if (isr & 0x08000000)
				as[i] = getvar_s(av[i]);
		}
		isr >>= 1;
	}

	nargs = i;
	if ((uint32_t)nargs < (info >> 30))
		syntax_error(EMSG_TOO_FEW_ARGS);

	info &= OPNMASK;
	switch (info) {

	case B_a2:
		if (ENABLE_FEATURE_AWK_LIBM)
			setvar_i(res, atan2(getvar_i(av[0]), getvar_i(av[1])));
		else
			syntax_error(EMSG_NO_MATH);
		break;

	case B_sp: {
		char *s, *s1;

		if (nargs > 2) {
			spl = (an[2]->info == TI_REGEXP) ? an[2]
				: mk_splitter(getvar_s(evaluate(an[2], TMPVAR2)), &tspl);
		} else {
			spl = &fsplitter.n;
		}

		n = awk_split(as[0], spl, &s);
		s1 = s;
		clear_array(iamarray(av[1]));
		for (i = 1; i <= n; i++)
			setari_u(av[1], i, nextword(&s));
		free(s1);
		setvar_i(res, n);
		break;
	}

	case B_ss: {
		l = strlen(as[0]);
		i = getvar_i(av[1]) - 1;
		if (i > l)
			i = l;
		if (i < 0)
			i = 0;
		n = (nargs > 2) ? getvar_i(av[2]) : l-i;
		if (n < 0)
			n = 0;
		setvar_sn(res, as[0]+i, n);
		break;
	}

	/* Bitwise ops must assume that operands are unsigned. GNU Awk 3.1.5:
	 * awk '{ print or(-1,1) }' gives "4.29497e+09", not "-2.xxxe+09" */
	case B_an:
		setvar_i(res, getvar_i_int(av[0]) & getvar_i_int(av[1]));
		break;

	case B_co:
		setvar_i(res, ~getvar_i_int(av[0]));
		break;

	case B_ls:
		setvar_i(res, getvar_i_int(av[0]) << getvar_i_int(av[1]));
		break;

	case B_or:
		setvar_i(res, getvar_i_int(av[0]) | getvar_i_int(av[1]));
		break;

	case B_rs:
		setvar_i(res, getvar_i_int(av[0]) >> getvar_i_int(av[1]));
		break;

	case B_xo:
		setvar_i(res, getvar_i_int(av[0]) ^ getvar_i_int(av[1]));
		break;

	case B_lo:
	case B_up: {
		char *s, *s1;
		s1 = s = xstrdup(as[0]);
		while (*s1) {
			//*s1 = (info == B_up) ? toupper(*s1) : tolower(*s1);
			if ((unsigned char)((*s1 | 0x20) - 'a') <= ('z' - 'a'))
				*s1 = (info == B_up) ? (*s1 & 0xdf) : (*s1 | 0x20);
			s1++;
		}
		setvar_p(res, s);
		break;
	}

	case B_ix:
		n = 0;
		ll = strlen(as[1]);
		l = strlen(as[0]) - ll;
		if (ll > 0 && l >= 0) {
			if (!icase) {
				char *s = strstr(as[0], as[1]);
				if (s)
					n = (s - as[0]) + 1;
			} else {
				/* this piece of code is terribly slow and
				 * really should be rewritten
				 */
				for (i = 0; i <= l; i++) {
					if (strncasecmp(as[0]+i, as[1], ll) == 0) {
						n = i+1;
						break;
					}
				}
			}
		}
		setvar_i(res, n);
		break;

	case B_ti:
		if (nargs > 1)
			tt = getvar_i(av[1]);
		else
			time(&tt);
		i = strftime(g_buf, MAXVARFMT,
			((nargs > 0) ? as[0] : "%a %b %d %H:%M:%S %Z %Y"),
			localtime(&tt));
		setvar_sn(res, g_buf, i);
		break;

	case B_mt:
		setvar_i(res, do_mktime(as[0]));
		break;

	case B_ma:
		res = do_match(an[1], as[0]);
		break;

	case B_ge: /* gensub(regex, repl, matchnum, string) */
		awk_sub(an[0], as[1], /*matchnum:*/getvar_i(av[2]), /*src:*/av[3], /*dst:*/res/*, TRUE*/);
		break;

	case B_gs: /* gsub(regex, repl, string) */
		setvar_i(res, awk_sub(an[0], as[1], /*matchnum:all*/0, /*src:*/av[2], /*dst:*/av[2]/*, FALSE*/));
		break;

	case B_su: /* sub(regex, repl, string) */
		setvar_i(res, awk_sub(an[0], as[1], /*matchnum:first*/1, /*src:*/av[2], /*dst:*/av[2]/*, FALSE*/));
		break;
	}

	nvfree(tmpvars, 4);
#undef TMPVAR0
#undef TMPVAR1
#undef TMPVAR2
#undef TMPVAR3
#undef TMPVAR

	return res;
#undef tspl
}

/* if expr looks like "var=value", perform assignment and return 1,
 * otherwise return 0 */
static int try_to_assign(const char *expr)
{
	char *exprc, *val;

	val = (char*)endofname(expr);
	if (val == (char*)expr || *val != '=') {
		return FALSE;
	}

	exprc = xstrdup(expr);
	val = exprc + (val - expr);
	*val++ = '\0';

	unescape_string_in_place(val);
	setvar_u(newvar(exprc), val);
	free(exprc);
	return TRUE;
}

/* switch to next input file */
static int next_input_file(void)
{
#define input_file_seen (G.next_input_file__input_file_seen)
#define argind (G.next_input_file__argind)
	const char *fname;

	if (iF.F) {
		fclose(iF.F);
		iF.F = NULL;
		iF.pos = iF.adv = 0;
	}

	for (;;) {
		/* GNU Awk 5.1.1 does not _read_ ARGIND (but does read ARGC).
		 * It only sets ARGIND to 1, 2, 3... for every command-line filename
		 * (VAR=VAL params cause a gap in numbering).
		 * If there are none and stdin is used, then ARGIND is not modified:
		 * if it is set by e.g. 'BEGIN { ARGIND="foo" }', that value will
		 * still be there.
		 */
		argind++;
		if (argind >= getvar_i(intvar[ARGC])) {
			if (input_file_seen)
				return FALSE;
			fname = "-";
			iF.F = stdin;
			break;
		}
		fname = getvar_s(findvar(iamarray(intvar[ARGV]), utoa(argind)));
		if (fname && *fname) {
			if (got_program != 2) { /* there was no -E option */
				/* "If a filename on the command line has the form
				 * var=val it is treated as a variable assignment"
				 */
				if (try_to_assign(fname))
					continue;
			}
			iF.F = xfopen_stdin(fname);
			setvar_i(intvar[ARGIND], argind);
			break;
		}
	}

	setvar_s(intvar[FILENAME], fname);
	input_file_seen = TRUE;
	return TRUE;
#undef argind
#undef input_file_seen
}

/*
 * Evaluate node - the heart of the program. Supplied with subtree
 * and "res" variable to assign the result to if we evaluate an expression.
 * If node refers to e.g. a variable or a field, no assignment happens.
 * Return ptr to the result (which may or may not be the "res" variable!)
 */
#define XC(n) ((n) >> 8)

static var *evaluate(node *op, var *res)
{
/* This procedure is recursive so we should count every byte */
#define fnargs (G.evaluate__fnargs)
/* seed is initialized to 1 */
#define seed   (G.evaluate__seed)
#define sreg   (G.evaluate__sreg)

	var *tmpvars;

	if (!op)
		return setvar_s(res, NULL);

	debug_printf_eval("entered %s()\n", __func__);

	tmpvars = nvalloc(2);
#define TMPVAR0 (tmpvars)
#define TMPVAR1 (tmpvars + 1)

	while (op) {
		struct {
			var *v;
			const char *s;
		} L = L; /* for compiler */
		struct {
			var *v;
			const char *s;
		} R = R;
		double L_d = L_d;
		uint32_t opinfo;
		int opn;
		node *op1;
		var *old_Fields_ptr;

		opinfo = op->info;
		opn = (opinfo & OPNMASK);
		g_lineno = op->lineno;
		op1 = op->l.n;
		debug_printf_eval("opinfo:%08x opn:%08x\n", opinfo, opn);

		/* execute inevitable things */
		old_Fields_ptr = NULL;
		if (opinfo & OF_RES1) {
			if ((opinfo & OF_REQUIRED) && !op1)
				syntax_error(EMSG_TOO_FEW_ARGS);
			L.v = evaluate(op1, TMPVAR0);
			/* Does L.v point to $n variable? */
			if ((size_t)(L.v - Fields) < num_alloc_fields) {
				/* yes, remember where Fields[] is */
				old_Fields_ptr = Fields;
			}
			if (opinfo & OF_NUM1) {
				L_d = getvar_i(L.v);
				debug_printf_eval("L_d:%f\n", L_d);
			}
		}
		/* NB: if both L and R are $NNNs, and right one is large,
		 * then at this pint L.v points to Fields[NNN1], second
		 * evaluate() below reallocates and moves (!) Fields[],
		 * R.v points to Fields[NNN2] but L.v now points to freed mem!
		 * (Seen trying to evaluate "$444 $44444")
		 */
		if (opinfo & OF_RES2) {
			R.v = evaluate(op->r.n, TMPVAR1);
			/* Seen in $5=$$5=$0:
			 * Evaluation of R.v ($$5=$0 expression)
			 * made L.v ($5) invalid. It's detected here.
			 */
			if (old_Fields_ptr) {
				//if (old_Fields_ptr != Fields)
				//	debug_printf_eval("L.v moved\n");
				L.v = Fields + (L.v - old_Fields_ptr);
			}
			if (opinfo & OF_STR2) {
				R.s = getvar_s(R.v);
				debug_printf_eval("R.s:'%s'\n", R.s);
			}
		}
		/* Get L.s _after_ R.v is evaluated: it may have realloc'd L.v
		 * so we must get the string after "old_Fields_ptr" correction
		 * above. Testcase: x = (v = "abc", gsub("b", "X", v));
		 */
		if (opinfo & OF_RES1) {
			if (opinfo & OF_STR1) {
				L.s = getvar_s(L.v);
				debug_printf_eval("L.s:'%s'\n", L.s);
			}
		}

		debug_printf_eval("switch(0x%x)\n", XC(opinfo & OPCLSMASK));
		switch (XC(opinfo & OPCLSMASK)) {

		/* -- iterative node type -- */

		/* test pattern */
		case XC( OC_TEST ):
			debug_printf_eval("TEST\n");
			if (op1->info == TI_COMMA) {
				/* it's range pattern */
				if ((opinfo & OF_CHECKED) || ptest(op1->l.n)) {
					op->info |= OF_CHECKED;
					if (ptest(op1->r.n))
						op->info &= ~OF_CHECKED;
					op = op->a.n;
				} else {
					op = op->r.n;
				}
			} else {
				op = ptest(op1) ? op->a.n : op->r.n;
			}
			break;

		/* just evaluate an expression, also used as unconditional jump */
		case XC( OC_EXEC ):
			debug_printf_eval("EXEC\n");
			break;

		/* branch, used in if-else and various loops */
		case XC( OC_BR ):
			debug_printf_eval("BR\n");
			op = istrue(L.v) ? op->a.n : op->r.n;
			break;

		/* initialize for-in loop */
		case XC( OC_WALKINIT ):
			debug_printf_eval("WALKINIT\n");
			hashwalk_init(L.v, iamarray(R.v));
			break;

		/* get next array item */
		case XC( OC_WALKNEXT ):
			debug_printf_eval("WALKNEXT\n");
			op = hashwalk_next(L.v) ? op->a.n : op->r.n;
			break;

		case XC( OC_PRINT ):
			debug_printf_eval("PRINT /\n");
		case XC( OC_PRINTF ):
			debug_printf_eval("PRINTF\n");
		{
			FILE *F = stdout;

			if (op->r.n) {
				rstream *rsm = newfile(R.s);
				if (!rsm->F) {
					if (opn == '|') {
						rsm->F = popen(R.s, "w");
						if (rsm->F == NULL)
							bb_simple_perror_msg_and_die("popen");
						rsm->is_pipe = 1;
					} else {
						rsm->F = xfopen(R.s, opn=='w' ? "w" : "a");
					}
				}
				F = rsm->F;
			}

			/* Can't just check 'opinfo == OC_PRINT' here, parser ORs
			 * additional bits to opinfos of print/printf with redirects
			 */
			if ((opinfo & OPCLSMASK) == OC_PRINT) {
				if (!op1) {
					fputs(getvar_s(intvar[F0]), F);
				} else {
					for (;;) {
						var *v = evaluate(nextarg(&op1), TMPVAR0);
						if (v->type & VF_NUMBER) {
							fputs(fmt_num(getvar_s(intvar[OFMT]), getvar_i(v)),
								F);
						} else {
							fputs(getvar_s(v), F);
						}
						if (!op1)
							break;
						fputs(getvar_s(intvar[OFS]), F);
					}
				}
				fputs(getvar_s(intvar[ORS]), F);
			} else {	/* PRINTF */
				IF_FEATURE_AWK_GNU_EXTENSIONS(size_t len;)
				char *s = awk_printf(op1, &len);
#if ENABLE_FEATURE_AWK_GNU_EXTENSIONS
				fwrite(s, len, 1, F);
#else
				fputs(s, F);
#endif
				free(s);
			}
			fflush(F);
			break;
		}

		case XC( OC_DELETE ):
			debug_printf_eval("DELETE\n");
		{
			/* "delete" is special:
			 * "delete array[var--]" must evaluate index expr only once.
			 */
			uint32_t info = op1->info & OPCLSMASK;
			var *v;

			if (info == OC_VAR) {
				v = op1->l.v;
			} else if (info == OC_FNARG) {
				v = &fnargs[op1->l.aidx];
			} else {
				syntax_error(EMSG_NOT_ARRAY);
			}
			if (op1->r.n) { /* array ref? */
				const char *s;
				s = getvar_s(evaluate(op1->r.n, TMPVAR0));
				hash_remove(iamarray(v), s);
			} else {
				clear_array(iamarray(v));
			}
			break;
		}

		case XC( OC_NEWSOURCE ):
			debug_printf_eval("NEWSOURCE\n");
			g_progname = op->l.new_progname;
			break;

		case XC( OC_RETURN ):
			debug_printf_eval("RETURN\n");
			copyvar(res, L.v);
			break;

		case XC( OC_NEXTFILE ):
			debug_printf_eval("NEXTFILE\n");
			nextfile = TRUE;
		case XC( OC_NEXT ):
			debug_printf_eval("NEXT\n");
			nextrec = TRUE;
		case XC( OC_DONE ):
			debug_printf_eval("DONE\n");
			clrvar(res);
			break;

		case XC( OC_EXIT ):
			debug_printf_eval("EXIT\n");
			if (op1)
				G.exitcode = (int)L_d;
			awk_exit();

		/* -- recursive node type -- */

		case XC( OC_CONST ):
			debug_printf_eval("CONST ");
		case XC( OC_VAR ):
			debug_printf_eval("VAR\n");
			L.v = op->l.v;
			if (L.v == intvar[NF])
				split_f0();
			goto v_cont;

		case XC( OC_FNARG ):
			debug_printf_eval("FNARG[%d]\n", op->l.aidx);
			L.v = &fnargs[op->l.aidx];
 v_cont:
			res = op->r.n ? findvar(iamarray(L.v), R.s) : L.v;
			break;

		case XC( OC_IN ):
			debug_printf_eval("IN\n");
			setvar_i(res, hash_search(iamarray(R.v), L.s) ? 1 : 0);
			break;

		case XC( OC_REGEXP ):
			debug_printf_eval("REGEXP\n");
			op1 = op;
			L.s = getvar_s(intvar[F0]);
			goto re_cont;

		case XC( OC_MATCH ):
			debug_printf_eval("MATCH\n");
			op1 = op->r.n;
 re_cont:
			{
				regex_t *re = as_regex(op1, &sreg);
				int i = regexec(re, L.s, 0, NULL, 0);
				if (re == &sreg)
					regfree(re);
				setvar_i(res, (i == 0) ^ (opn == '!'));
			}
			break;

		case XC( OC_MOVE ):
			debug_printf_eval("MOVE\n");
			/* make sure that we never return a temp var */
			if (L.v == TMPVAR0)
				L.v = res;
			/* if source is a temporary string, just relink it to dest */
			if (R.v == TMPVAR1
			 && !(R.v->type & VF_NUMBER)
				/* Why check !NUMBER? if R.v is a number but has cached R.v->string,
				 * L.v ends up a string, which is wrong */
			 /*&& R.v->string - always not NULL (right?) */
			) {
				res = setvar_p(L.v, R.v->string); /* avoids strdup */
				R.v->string = NULL;
			} else {
				res = copyvar(L.v, R.v);
			}
			break;

		case XC( OC_TERNARY ):
			debug_printf_eval("TERNARY\n");
			if (op->r.n->info != TI_COLON)
				syntax_error(EMSG_POSSIBLE_ERROR);
			res = evaluate(istrue(L.v) ? op->r.n->l.n : op->r.n->r.n, res);
			break;

		case XC( OC_FUNC ): {
			var *argvars, *sv_fnargs;
			const char *sv_progname;
			int nargs, i;

			debug_printf_eval("FUNC\n");

			if (!op->r.f->defined)
				syntax_error(EMSG_UNDEF_FUNC);

			/* The body might be empty, still has to eval the args */
			nargs = op->r.f->nargs;
			argvars = nvalloc(nargs);
			i = 0;
			while (op1) {
				var *arg = evaluate(nextarg(&op1), TMPVAR0);
				if (i == nargs) {
					/* call with more arguments than function takes.
					 * (gawk warns: "warning: function 'f' called with more arguments than declared").
					 * They are still evaluated, but discarded: */
					clrvar(arg);
					continue;
				}
				copyvar(&argvars[i], arg);
				argvars[i].type |= VF_CHILD;
				argvars[i].x.parent = arg;
				i++;
			}

			sv_fnargs = fnargs;
			sv_progname = g_progname;

			fnargs = argvars;
			res = evaluate(op->r.f->body.first, res);
			nvfree(argvars, nargs);

			g_progname = sv_progname;
			fnargs = sv_fnargs;

			break;
		}

		case XC( OC_GETLINE ):
			debug_printf_eval("GETLINE /\n");
		case XC( OC_PGETLINE ):
			debug_printf_eval("PGETLINE\n");
		{
			rstream *rsm;
			int i;

			if (op1) {
				rsm = newfile(L.s);
				if (!rsm->F) {
					/* NB: can't use "opinfo == TI_PGETLINE", would break "cmd" | getline */
					if ((opinfo & OPCLSMASK) == OC_PGETLINE) {
						rsm->F = popen(L.s, "r");
						rsm->is_pipe = TRUE;
					} else {
						rsm->F = fopen_for_read(L.s);  /* not xfopen! */
					}
				}
			} else {
				if (!iF.F)
					next_input_file();
				rsm = &iF;
			}

			if (!rsm->F) {
				setvar_ERRNO();
				setvar_i(res, -1);
				break;
			}

			if (!op->r.n)
				R.v = intvar[F0];

			i = awk_getline(rsm, R.v);
			if (i > 0 && !op1) {
				incvar(intvar[FNR]);
				incvar(intvar[NR]);
			}
			setvar_i(res, i);
			break;
		}

		/* simple builtins */
		case XC( OC_FBLTIN ): {
			double R_d = R_d; /* for compiler */
			debug_printf_eval("FBLTIN\n");

			if (op1 && op1->info == TI_COMMA)
				/* Simple builtins take one arg maximum */
				syntax_error("Too many arguments");

			switch (opn) {
			case F_in:
				R_d = (long long)L_d;
				break;

			case F_rn: /*rand*/
				if (op1)
					syntax_error("Too many arguments");
			{
#if RAND_MAX >= 0x7fffffff
				uint32_t u = ((uint32_t)rand() << 16) ^ rand();
				uint64_t v = ((uint64_t)rand() << 32) | u;
				/* the above shift+or is optimized out on 32-bit arches */
# if RAND_MAX > 0x7fffffff
				v &= 0x7fffffffffffffffULL;
# endif
				R_d = (double)v / 0x8000000000000000ULL;
#else
# error Not implemented for this value of RAND_MAX
#endif
				break;
			}
			case F_co:
				if (ENABLE_FEATURE_AWK_LIBM) {
					R_d = cos(L_d);
					break;
				}

			case F_ex:
				if (ENABLE_FEATURE_AWK_LIBM) {
					R_d = exp(L_d);
					break;
				}

			case F_lg:
				if (ENABLE_FEATURE_AWK_LIBM) {
					R_d = log(L_d);
					break;
				}

			case F_si:
				if (ENABLE_FEATURE_AWK_LIBM) {
					R_d = sin(L_d);
					break;
				}

			case F_sq:
				if (ENABLE_FEATURE_AWK_LIBM) {
					R_d = sqrt(L_d);
					break;
				}

				syntax_error(EMSG_NO_MATH);
				break;

			case F_sr:
				R_d = (double)seed;
				seed = op1 ? (unsigned)L_d : (unsigned)time(NULL);
				srand(seed);
				break;

			case F_ti: /*systime*/
				if (op1)
					syntax_error("Too many arguments");
				R_d = time(NULL);
				break;

			case F_le:
				debug_printf_eval("length: L.s:'%s'\n", L.s);
				if (!op1) {
					L.s = getvar_s(intvar[F0]);
					debug_printf_eval("length: L.s='%s'\n", L.s);
				}
				else if (L.v->type & VF_ARRAY) {
					R_d = L.v->x.array->nel;
					debug_printf_eval("length: array_len:%d\n", L.v->x.array->nel);
					break;
				}
				R_d = strlen(L.s);
				break;

			case F_sy:
				fflush_all();
				R_d = (ENABLE_FEATURE_ALLOW_EXEC && L.s && *L.s)
						? (system(L.s) >> 8) : 0;
				break;

			case F_ff:
				if (!op1) {
					fflush(stdout);
				} else if (L.s && *L.s) {
					rstream *rsm = newfile(L.s);
					fflush(rsm->F);
				} else {
					fflush_all();
				}
				break;

			case F_cl: {
				rstream *rsm;
				int err = 0;
				rsm = (rstream *)hash_search(fdhash, L.s);
				debug_printf_eval("OC_FBLTIN close: op1:%p s:'%s' rsm:%p\n", op1, L.s, rsm);
				if (rsm) {
					debug_printf_eval("OC_FBLTIN F_cl "
						"rsm->is_pipe:%d, ->F:%p\n",
						rsm->is_pipe, rsm->F);
					/* Can be NULL if open failed. Example:
					 * getline line <"doesnt_exist";
					 * close("doesnt_exist"); <--- here rsm->F is NULL
					 */
					if (rsm->F)
						err = rsm->is_pipe ? pclose(rsm->F) : fclose(rsm->F);
					free(rsm->buffer);
					hash_remove(fdhash, L.s);
				} else {
					err = -1;
					/* gawk 'BEGIN { print close(""); print ERRNO }'
					 * -1
					 * close of redirection that was never opened
					 */
					errno = ENOENT;
				}
				if (err)
					setvar_ERRNO();
				R_d = (double)err;
				break;
			}
			} /* switch */
			setvar_i(res, R_d);
			break;
		}

		case XC( OC_BUILTIN ):
			debug_printf_eval("BUILTIN\n");
			res = exec_builtin(op, res);
			break;

		case XC( OC_SPRINTF ):
			debug_printf_eval("SPRINTF\n");
			setvar_p(res, awk_printf(op1, NULL));
			break;

		case XC( OC_UNARY ):
			debug_printf_eval("UNARY\n");
		{
			double Ld, R_d;

			Ld = R_d = getvar_i(R.v);
			switch (opn) {
			case 'P':
				Ld = ++R_d;
				goto r_op_change;
			case 'p':
				R_d++;
				goto r_op_change;
			case 'M':
				Ld = --R_d;
				goto r_op_change;
			case 'm':
				R_d--;
 r_op_change:
				setvar_i(R.v, R_d);
				break;
			case '!':
				Ld = !istrue(R.v);
				break;
			case '-':
				Ld = -R_d;
				break;
			}
			setvar_i(res, Ld);
			break;
		}

		case XC( OC_FIELD ):
			debug_printf_eval("FIELD\n");
		{
			int i = (int)getvar_i(R.v);
			if (i < 0)
				syntax_error(EMSG_NEGATIVE_FIELD);
			if (i == 0) {
				res = intvar[F0];
			} else {
				split_f0();
				if (i > num_fields)
					fsrealloc(i);
				res = &Fields[i - 1];
			}
			break;
		}

		/* concatenation (" ") and index joining (",") */
		case XC( OC_CONCAT ):
			debug_printf_eval("CONCAT /\n");
		case XC( OC_COMMA ): {
			const char *sep = "";
			debug_printf_eval("COMMA\n");
			if (opinfo == TI_COMMA)
				sep = getvar_s(intvar[SUBSEP]);
			setvar_p(res, xasprintf("%s%s%s", L.s, sep, R.s));
			break;
		}

		case XC( OC_LAND ):
			debug_printf_eval("LAND\n");
			setvar_i(res, istrue(L.v) ? ptest(op->r.n) : 0);
			break;

		case XC( OC_LOR ):
			debug_printf_eval("LOR\n");
			setvar_i(res, istrue(L.v) ? 1 : ptest(op->r.n));
			break;

		case XC( OC_BINARY ):
			debug_printf_eval("BINARY /\n");
		case XC( OC_REPLACE ):
			debug_printf_eval("REPLACE\n");
		{
			double R_d = getvar_i(R.v);
			debug_printf_eval("R_d:%f opn:%c\n", R_d, opn);
			switch (opn) {
			case '+':
				L_d += R_d;
				break;
			case '-':
				L_d -= R_d;
				break;
			case '*':
				L_d *= R_d;
				break;
			case '/':
				if (R_d == 0)
					syntax_error(EMSG_DIV_BY_ZERO);
				L_d /= R_d;
				break;
			case '&':
				if (ENABLE_FEATURE_AWK_LIBM)
					L_d = pow(L_d, R_d);
				else
					syntax_error(EMSG_NO_MATH);
				break;
			case '%':
				if (R_d == 0)
					syntax_error(EMSG_DIV_BY_ZERO);
				L_d -= (long long)(L_d / R_d) * R_d;
				break;
			}
			debug_printf_eval("BINARY/REPLACE result:%f\n", L_d);
			res = setvar_i(((opinfo & OPCLSMASK) == OC_BINARY) ? res : L.v, L_d);
			break;
		}

		case XC( OC_COMPARE ): {
			int i = i; /* for compiler */
			double Ld;
			debug_printf_eval("COMPARE\n");

			if (is_numeric(L.v) && is_numeric(R.v)) {
				Ld = getvar_i(L.v) - getvar_i(R.v);
			} else {
				const char *l = getvar_s(L.v);
				const char *r = getvar_s(R.v);
				Ld = icase ? strcasecmp(l, r) : strcmp(l, r);
			}
			switch (opn & 0xfe) {
			case 0:
				i = (Ld > 0);
				break;
			case 2:
				i = (Ld >= 0);
				break;
			case 4:
				i = (Ld == 0);
				break;
			}
			debug_printf_eval("COMPARE result: %d\n", (i == 0) ^ (opn & 1));
			setvar_i(res, (i == 0) ^ (opn & 1));
			break;
		}

		default:
			syntax_error(EMSG_POSSIBLE_ERROR);
		} /* switch */

		if ((opinfo & OPCLSMASK) <= SHIFT_TIL_THIS)
			op = op->a.n;
		if ((opinfo & OPCLSMASK) >= RECUR_FROM_THIS)
			break;
		if (nextrec)
			break;
	} /* while (op) */

	nvfree(tmpvars, 2);
#undef TMPVAR0
#undef TMPVAR1

	debug_printf_eval("returning from %s(): %p\n", __func__, res);
	return res;
#undef fnargs
#undef seed
#undef sreg
}

static int awk_exit(void)
{
	unsigned i;

	if (!exiting) {
		exiting = TRUE;
		nextrec = FALSE;
		evaluate(endseq.first, &G.exit__tmpvar);
	}

	/* waiting for children */
	for (i = 0; i < fdhash->csize; i++) {
		hash_item *hi;
		hi = fdhash->items[i];
		while (hi) {
			if (hi->data.rs.F && hi->data.rs.is_pipe)
				pclose(hi->data.rs.F);
			hi = hi->next;
		}
	}

	exit(G.exitcode);
}

int awk_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int awk_main(int argc UNUSED_PARAM, char **argv)
{
	int ch;
	int i;

	INIT_G();

	/* Undo busybox.c, or else strtod may eat ','! This breaks parsing:
	 * $1,$2 == '$1,' '$2', NOT '$1' ',' '$2' */
	if (ENABLE_LOCALE_SUPPORT)
		setlocale(LC_NUMERIC, "C");

	/* initialize variables */
	vhash = hash_init();
	{
		char *vnames = (char *)vNames; /* cheat */
		char *vvalues = (char *)vValues;
		for (i = 0; *vnames; i++) {
			var *v;
			intvar[i] = v = newvar(nextword(&vnames));
			if (*vvalues != '\377')
				setvar_s(v, nextword(&vvalues));
			else
				setvar_i(v, 0);

			if (*vnames == '*') {
				v->type |= VF_SPECIAL;
				vnames++;
			}
		}
	}

	handle_special(intvar[FS]);
	handle_special(intvar[RS]);

	/* Huh, people report that sometimes environ is NULL. Oh well. */
	if (environ) {
		char **envp;
		for (envp = environ; *envp; envp++) {
			/* environ is writable, thus we don't strdup it needlessly */
			char *s = *envp;
			char *s1 = strchr(s, '=');
			if (s1) {
				*s1 = '\0';
				/* Both findvar and setvar_u take const char*
				 * as 2nd arg -> environment is not trashed */
				setvar_u(findvar(iamarray(intvar[ENVIRON]), s), s1 + 1);
				*s1 = '=';
			}
		}
	}

	fnhash = hash_init();
	ahash = hash_init();

	/* Cannot use getopt32: need to preserve order of -e / -f / -E / -i */
	while ((ch = getopt(argc, argv, OPTSTR_AWK)) >= 0) {
		switch (ch) {
		case 'F':
			unescape_string_in_place(optarg);
			setvar_s(intvar[FS], optarg);
			break;
		case 'v':
			if (!try_to_assign(optarg))
				bb_show_usage();
			break;
//TODO: implement -i LIBRARY, it is easy-ish
		case 'E':
		case 'f':  {
			int fd;
			char *s;
			g_progname = optarg;
			fd = xopen_stdin(g_progname);
			s = xmalloc_read(fd, NULL); /* it's NUL-terminated */
			if (!s)
				bb_perror_msg_and_die("read error from '%s'", g_progname);
			close(fd);
			parse_program(s);
			free(s);
			got_program = 1;
			if (ch == 'E') {
				got_program = 2;
				goto stop_option_parsing;
			}
			break;
		}
#if ENABLE_FEATURE_AWK_GNU_EXTENSIONS
		case 'e':
			g_progname = "cmd. line";
			parse_program(optarg);
			got_program = 1;
			break;
#endif
		case 'W':
			bb_simple_error_msg("warning: option -W is ignored");
			break;
		default:
//bb_error_msg("ch:%d", ch);
			bb_show_usage();
		}
	}
 stop_option_parsing:

	argv += optind;
	//argc -= optind;

	if (!got_program) {
		if (!*argv)
			bb_show_usage();
		g_progname = "cmd. line";
		parse_program(*argv++);
	}

	/* Free unused parse structures */
	//hash_free(fnhash); // ~250 bytes when empty, used only for function names
	//^^^^^^^^^^^^^^^^^ does not work, hash_clear() inside SEGVs
	// (IOW: hash_clear() assumes it's a hash of variables. fnhash is not).
	free(fnhash->items);
	free(fnhash);
	fnhash = NULL; // debug
	//hash_free(ahash); // empty after parsing, will reuse as fdhash instead of freeing

	/* Parsing done, on to executing */

	/* fill in ARGV array */
	setari_u(intvar[ARGV], 0, "awk");
	i = 0;
	while (*argv)
		setari_u(intvar[ARGV], ++i, *argv++);
	setvar_i(intvar[ARGC], i + 1);

	//fdhash = ahash; // done via define
	newfile("/dev/stdin")->F = stdin;
	newfile("/dev/stdout")->F = stdout;
	newfile("/dev/stderr")->F = stderr;

	evaluate(beginseq.first, &G.main__tmpvar);
	if (!mainseq.first && !endseq.first)
		awk_exit();

	/* input file could already be opened in BEGIN block */
	if (!iF.F)
		goto next_file; /* no, it wasn't, go try opening */
	/* Iterate over input files */
	for (;;) {
		nextfile = FALSE;
		setvar_i(intvar[FNR], 0);

		while ((i = awk_getline(&iF, intvar[F0])) > 0) {
			nextrec = FALSE;
			incvar(intvar[NR]);
			incvar(intvar[FNR]);
			evaluate(mainseq.first, &G.main__tmpvar);

			if (nextfile)
				break;
		}
		if (i < 0)
			syntax_error(strerror(errno));
 next_file:
		if (!next_input_file())
			break;
	}

	awk_exit();
	/*return 0;*/
}
