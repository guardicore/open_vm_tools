/* **********************************************************
 * Copyright (C) 2006-2016 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Copyright (c) 1990, 1993
 *   The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Note - this code originated as the file vfprintf.c in the FreeBSD
 * source code, location src/lib/libc/stdio/vfprintf.c, revision
 * 1.72. It has been borrowed and modified to act like vsnprintf
 * instead. For now, it only works for Windows. See bsd_output.h for
 * more.
 *
 * If you care to compare, the original is checked into this directory
 * as bsd_vsnprintf_orig.c.
 */

#if !defined(STR_NO_WIN32_LIBS) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)

/*
 * Actual printf innards.
 *
 * This code is large and complicated...
 */

#include <sys/types.h>

#include <ctype.h>
#include <limits.h>
#include <locale.h>
#ifndef _WIN32
#include <stddef.h>
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "vmware.h"
#include "bsd_output_int.h"
#include "codeset.h"
#include "convertutf.h"
#include "str.h"


#if defined __ANDROID__
/*
 * Android doesn't support dtoa() or ldtoa().
 */
#define NO_DTOA
#define NO_LDTOA
#endif

static char   *__ultoa(u_long, char *, int, int, const char *, int, char,
                       const char *);
static void   __find_arguments(const char *, va_list, union arg **);
static void   __grow_type_table(int, enum typeid **, int *);

char blanks[PADSIZE] =
   {' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
char zeroes[PADSIZE] =
   {'0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0'};

const char xdigs_lower[17] = "0123456789abcdef?";
const char xdigs_upper[17] = "0123456789ABCDEF?";

static Bool isLenientConversion = TRUE;

int
BSDFmt_SFVWrite(BSDFmt_StrBuf *sbuf, BSDFmt_UIO *uio)
{
   int i;
   BSDFmt_IOV *siov;

   /*
    * If asprintf(), then grow the buffer as necessary.
    */

   if (sbuf->alloc) {
      size_t n = sbuf->index + uio->uio_resid + 1;	// +1 for \0

      if (n > sbuf->size) {
	 char *p;

	 ASSERT(sbuf->size > 0);
	 n = ROUNDUP(n, sbuf->size);
	 if ((p = realloc(sbuf->buf, n)) == NULL) {
	    sbuf->error = TRUE;
	    return 1;
	 }
	 sbuf->buf = p;
	 sbuf->size = n;
      }
   }

   for (i = 0, siov = uio->uio_iov; i < uio->uio_iovcnt; i++, siov++) {
      int numToWrite = sbuf->size - sbuf->index - 1;  // -1 for \0

      /*
       * Overflowing the buffer is not an error.
       * We just silently truncate because that's what snprintf() does.
       *
       * Always leave space for null termination.
       */

      if (numToWrite > siov->iov_len) {
	 numToWrite = siov->iov_len;
      }

      memcpy(sbuf->buf + sbuf->index, siov->iov_base, numToWrite);
      sbuf->index += numToWrite;
   }

   return 0;
}

/*
 * Flush out all the vectors defined by the given uio,
 * then reset it so that it can be reused.
 */
int
BSDFmt_SPrint(BSDFmt_StrBuf *sbuf, BSDFmt_UIO *uio)
{
   int err;

   if (uio->uio_resid == 0) {
      uio->uio_iovcnt = 0;
      return (0);
   }

   err = BSDFmt_SFVWrite(sbuf, uio);
   uio->uio_resid = 0;
   uio->uio_iovcnt = 0;

   return err;
}

/*
 * Convert an unsigned long to ASCII for printf purposes, returning
 * a pointer to the first character of the string representation.
 * Octal numbers can be forced to have a leading zero; hex numbers
 * use the given digits.
 */
static char *
__ultoa(u_long val, char *endp, int base, int octzero, const char *xdigs,
        int needgrp, char thousep, const char *grp)
{
   char *cp = endp;
   long sval;
   int ndig;

   /*
    * Handle the three cases separately, in the hope of getting
    * better/faster code.
    */
   switch (base) {
   case 10:
      if (val < 10) {   /* many numbers are 1 digit */
         *--cp = to_char(val);
         return (cp);
      }
      ndig = 0;
      /*
       * On many machines, unsigned arithmetic is harder than
       * signed arithmetic, so we do at most one unsigned mod and
       * divide; this is sufficient to reduce the range of
       * the incoming value to where signed arithmetic works.
       */
      if (val > LONG_MAX) {
         *--cp = to_char(val % 10);
         ndig++;
         sval = val / 10;
      } else {
         sval = val;
      }

      do {
         *--cp = to_char(sval % 10);
         ndig++;

         /*
          * If (*grp == CHAR_MAX) then no more grouping
          * should be performed.
          */

         if (needgrp && ndig == *grp && *grp != CHAR_MAX && sval > 9) {
            *--cp = thousep;
            ndig = 0;

            /*
             * If (*(grp+1) == '\0') then we have to* use *grp character
             * (last grouping rule) for all next cases
             */

            if (*(grp+1) != '\0') {
               grp++;
            }
         }
         sval /= 10;
      } while (sval != 0);
      break;

   case 8:
      do {
         *--cp = to_char(val & 7);
         val >>= 3;
      } while (val);

      if (octzero && *cp != '0') {
         *--cp = '0';
      }
      break;

   case 16:
      do {
         *--cp = xdigs[val & 15];
         val >>= 4;
      } while (val);
      break;

   default:         /* oops */
      abort();
   }
   return (cp);
}

/* Identical to __ultoa, but for intmax_t. */
char *
BSDFmt_UJToA(uintmax_t val, char *endp, int base, int octzero,
             const char *xdigs, int needgrp, char thousep, const char *grp)
{
   char *cp = endp;
   intmax_t sval;
   int ndig;

   /* quick test for small values; __ultoa is typically much faster */
   /* (perhaps instead we should run until small, then call __ultoa?) */
   if (val <= ULONG_MAX) {
      return (__ultoa((u_long)val, endp, base, octzero, xdigs,
                      needgrp, thousep, grp));
   }

   switch (base) {
   case 10:
      if (val < 10) {
         *--cp = to_char(val % 10);
         return (cp);
      }
      ndig = 0;
      if (val > INTMAX_MAX) {
         *--cp = to_char(val % 10);
         ndig++;
         sval = val / 10;
      } else {
         sval = val;
      }
      do {
         *--cp = to_char(sval % 10);
         ndig++;
         /*
          * If (*grp == CHAR_MAX) then no more grouping should be performed.
          */

         if (needgrp && *grp != CHAR_MAX && ndig == *grp && sval > 9) {
            *--cp = thousep;
            ndig = 0;

            /*
             * If (*(grp+1) == '\0') then we have to use *grp character
            * (last grouping rule) for all next cases
             */

            if (*(grp+1) != '\0') {
               grp++;
            }
         }
         sval /= 10;
      } while (sval != 0);
      break;

   case 8:
      do {
         *--cp = to_char(val & 7);
         val >>= 3;
      } while (val);

      if (octzero && *cp != '0') {
         *--cp = '0';
      }
      break;

   case 16:
      do {
         *--cp = xdigs[val & 15];
         val >>= 4;
      } while (val);
      break;

   default:
      abort();
   }
   return (cp);
}

/*
 * Convert a wide character string argument to a UTF-8 string
 * representation. If not -1, 'prec' specifies the maximum number of
 * bytes to output. The returned string is always NUL-terminated, even
 * if that results in the string exceeding 'prec' bytes.
 */
char *
BSDFmt_WCharToUTF8(wchar_t *wcsarg, int prec)
{
   ConversionResult cres;
   char *sourceStart, *sourceEnd;
   char *targStart, *targEnd;
   char *targ = NULL;
   size_t targSize;
   size_t sourceSize = wcslen(wcsarg) * sizeof(wchar_t);

   targSize = (-1 == prec) ? sourceSize : MIN(sourceSize, prec);

   while (TRUE) {
      /*
       * Pad by 4, because we need to NUL-terminate.
       */
      targ = realloc(targ, targSize + 4);
      if (!targ) {
         goto exit;
      }

      targStart = targ;
      targEnd = targStart + targSize;
      sourceStart = (char *) wcsarg;
      sourceEnd = sourceStart + sourceSize;

      if (2 == sizeof(wchar_t)) {
         cres = ConvertUTF16toUTF8((const UTF16 **) &sourceStart,
                                   (const UTF16 *) sourceEnd,
                                   (UTF8 **) &targStart,
                                   (UTF8 *) targEnd,
                                   isLenientConversion);
      } else if (4 == sizeof(wchar_t)) {
         cres = ConvertUTF32toUTF8((const UTF32 **) &sourceStart,
                                   (const UTF32 *) sourceEnd,
                                   (UTF8 **) &targStart,
                                   (UTF8 *) targEnd,
                                   isLenientConversion);
      } else {
         NOT_IMPLEMENTED();
      }

      if (targetExhausted == cres) {
         if (targSize == prec) {
            /*
             * We've got all the caller wants.
             */
            break;
         } else {
            /*
             * Double buffer.
             */
            targSize = (-1 == prec) ? targSize * 2 : MIN(targSize * 2, prec);
         }
      } else if ((sourceExhausted == cres) ||
                 (sourceIllegal == cres)) {
         /*
          * If lenient, the API converted all it could, so just
          * proceed, otherwise, barf.
          */
         if (isLenientConversion) {
            break;
         } else {
            free(targ);
            targ = NULL;
            goto exit;
         }
      } else if (conversionOK == cres) {
         break;
      } else {
         NOT_IMPLEMENTED();
      }
   }

   /*
    * Success, NUL-terminate. (The API updated targStart for us).
    */
   ASSERT(targStart <= targEnd);
   targSize = targStart - targ;
   memset(targ + targSize, 0, 4);

  exit:
   return targ;
}


int
bsd_vsnprintf_core(char **outbuf,
                   char *groupingIn,
                   char thousands_sepIn,
                   char *decimal_point,
                   size_t bufSize,
                   const char *fmt0,
                   va_list ap)
{
   char *fmt;      /* format string */
   int ch;         /* character from fmt */
   int n, n2;      /* handy integer (short term usage) */
   char *cp;      /* handy char pointer (short term usage) */
   BSDFmt_IOV *iovp;   /* for PRINT macro */
   int flags;      /* flags as above */
   int ret;      /* return value accumulator */
   int width;      /* width from format (%8d), or 0 */
   int prec;      /* precision from format; <0 for N/A */
   char sign;      /* sign prefix (' ', '+', '-', or \0) */
   char thousands_sep;   /* locale specific thousands separator */
   char *grouping;       /* locale specific numeric grouping rules */

#if !defined(NO_FLOATING_POINT)
   /*
    * We can decompose the printed representation of floating
    * point numbers into several parts, some of which may be empty:
    *
    * [+|-| ] [0x|0X] MMM . NNN [e|E|p|P] [+|-] ZZ
    *    A       B     ---C---      D       E   F
    *
    * A:   'sign' holds this value if present; '\0' otherwise
    * B:   ox[1] holds the 'x' or 'X'; '\0' if not hexadecimal
    * C:   cp points to the string MMMNNN.  Leading and trailing
    *   zeros are not in the string and must be added.
    * D:   expchar holds this character; '\0' if no exponent, e.g. %f
    * F:   at least two digits for decimal, at least one digit for hex
    */
#if defined __ANDROID__
   static char dp = '.';
#endif
   int signflag;      /* true if float is negative */
   union {         /* floating point arguments %[aAeEfFgG] */
      double dbl;
      long double ldbl;
   } fparg;
   int expt = 0;      /* integer value of exponent */
   char expchar;      /* exponent character: [eEpP\0] */
   char *dtoaend;      /* pointer to end of converted digits */
   int expsize;      /* character count for expstr */
   int lead;      /* sig figs before decimal or group sep */
   int ndig;      /* actual number of digits returned by dtoa */
   char expstr[MAXEXPDIG + 2];   /* buffer for exponent string: e+ZZZ */
   char *dtoaresult;   /* buffer allocated by dtoa */
   int nseps;      /* number of group separators with ' */
   int nrepeats;      /* number of repeats of the last group */
#endif
   u_long   ulval;      /* integer arguments %[diouxX] */
   uintmax_t ujval;   /* %j, %ll, %q, %t, %z integers */
   int base;      /* base for [diouxX] conversion */
   int dprec;      /* a copy of prec if [diouxX], 0 otherwise */
   int realsz;      /* field size expanded by dprec, sign, etc */
   int size;      /* size of converted field or string */
   int prsize;             /* max size of printed field */
   const char *xdigs;        /* digits for %[xX] conversion */
   BSDFmt_UIO uio;   /* output information: summary */
   BSDFmt_IOV iov[BSDFMT_NIOV]; /* ... and individual io vectors */
   char buf[INT_CONV_BUF]; /* buffer with space for digits of uintmax_t */
   char ox[2];      /* space for 0x; ox[1] is either x, X, or \0 */
   union arg *argtable;    /* args, built due to positional arg */
   union arg statargtable [STATIC_ARG_TBL_SIZE];
   int nextarg;            /* 1-based argument index */
#ifndef _WIN32
   va_list orgap;          /* original argument pointer */
#endif
   char *convbuf = NULL;      /* wide to multibyte conversion result */
   BSDFmt_StrBuf sbuf;

   /*
    * BEWARE, these `goto error' on error, and PAD uses `n'.
    */
#define   PRINT(ptr, len) {                   \
      iovp->iov_base = (ptr);                 \
      iovp->iov_len = (len) * sizeof (char);  \
      uio.uio_resid += (len) * sizeof (char); \
      iovp++;                                 \
      if (++uio.uio_iovcnt >= BSDFMT_NIOV) {  \
         if (BSDFmt_SPrint(&sbuf, &uio))      \
            goto error;                       \
         iovp = iov;                          \
      }                                       \
   }

#define   PAD(howmany, with) {     \
      if ((n = (howmany)) > 0) {   \
         while (n > PADSIZE) {     \
            PRINT(with, PADSIZE);  \
            n -= PADSIZE;          \
         }                         \
         PRINT(with, n);           \
      }                            \
   }

#define   PRINTANDPAD(p, ep, len, with) do {   \
      n2 = (ep) - (p);                         \
      if (n2 > (len))                          \
         n2 = (len);                           \
      if (n2 > 0)                              \
         PRINT((p), n2);                       \
      PAD((len) - (n2 > 0 ? n2 : 0), (with));  \
   } while(0)

#define   FLUSH() {                                     \
      if (uio.uio_resid && BSDFmt_SPrint(&sbuf, &uio))  \
         goto error;                                    \
      uio.uio_iovcnt = 0;                               \
      iovp = iov;                                       \
   }

   /*
    * Get the argument indexed by nextarg.   If the argument table is
    * built, use it to get the argument.  If its not, get the next
    * argument (and arguments must be gotten sequentially).
    */

#define GETARG(type)                                         \
   ((argtable != NULL) ? *((type*)(&argtable[nextarg++])) :  \
    (nextarg++, va_arg(ap, type)))

   /*
    * To extend shorts properly, we need both signed and unsigned
    * argument extraction methods.
    */

#define   SARG()                                        \
   (flags&LONGINT ? GETARG(long) :                      \
    flags&SHORTINT ? (long)(short)GETARG(int) :         \
    flags&CHARINT ? (long)(signed char)GETARG(int) :    \
    (long)GETARG(int))

#define   UARG()                                        \
   (flags&LONGINT ? GETARG(u_long) :                    \
    flags&SHORTINT ? (u_long)(u_short)GETARG(int) :     \
    flags&CHARINT ? (u_long)(u_char)GETARG(int) :       \
    (u_long)GETARG(u_int))

#define SJARG()                                         \
   (flags&INTMAXT ? GETARG(intmax_t) :                  \
    flags&SIZET ? (intmax_t)GETARG(size_t) :            \
    flags&PTRDIFFT ? (intmax_t)GETARG(ptrdiff_t) :      \
    (intmax_t)GETARG(long long))

#define   UJARG()                                       \
   (flags&INTMAXT ? GETARG(uintmax_t) :                 \
    flags&SIZET ? (uintmax_t)GETARG(size_t) :           \
    flags&PTRDIFFT ? (uintmax_t)GETARG(ptrdiff_t) :     \
    (uintmax_t)GETARG(unsigned long long))

   /*
    * Get * arguments, including the form *nn$.  Preserve the nextarg
    * that the argument can be gotten once the type is determined.
    */

#define GETASTER(val)                \
   n2 = 0;                           \
   cp = fmt;                         \
   while (is_digit(*cp)) {           \
      n2 = 10 * n2 + to_digit(*cp);  \
      cp++;                          \
   }                                 \
   if (*cp == '$') {                 \
      int hold = nextarg;            \
      FIND_ARGUMENTS();              \
      nextarg = n2;                  \
      val = GETARG (int);            \
      nextarg = hold;                \
      fmt = ++cp;                    \
   } else {                          \
      val = GETARG (int);            \
   }

   /*
    * Windows can't scan the args twice, so always build argtable.
    * Otherwise, do it when we see an n$ argument.
    */

#ifndef _WIN32
   #define FIND_ARGUMENTS()                            \
      (argtable == NULL ?                              \
	 (argtable = statargtable,                     \
	  __find_arguments(fmt0, orgap, &argtable)) :  \
	 (void) 0)
#else
   #define FIND_ARGUMENTS()                            \
      ASSERT(argtable != NULL)
#endif

   xdigs = xdigs_lower;
   thousands_sep = '\0';
   grouping = NULL;
   convbuf = NULL;
#if !defined(NO_FLOATING_POINT)
   dtoaresult = NULL;
#ifdef __ANDROID__
   /*
    * Struct lconv is not working! For decimal_point,
    * using '.' instead is a workaround.
    */
   decimal_point = &dp;
#endif
#endif

   fmt = (char *)fmt0;
   nextarg = 1;
#ifndef _WIN32
   argtable = NULL;
   va_copy(orgap, ap);
#else
   argtable = statargtable;
   __find_arguments(fmt0, ap, &argtable);
#endif
   uio.uio_iov = iovp = iov;
   uio.uio_resid = 0;
   uio.uio_iovcnt = 0;
   ret = 0;

   /*
    * Set up output string buffer structure.
    */

   sbuf.alloc = (*outbuf == NULL);
   sbuf.error = FALSE;
   sbuf.buf = *outbuf;
   sbuf.size = bufSize;
   sbuf.index = 0;

   /*
    * If asprintf(), allocate initial buffer based on format length.
    * Empty format only needs one byte. Otherwise, round up to multiple of 64.
    */

   if (sbuf.alloc) {
      size_t n = strlen(fmt0) + 1;	// +1 for \0

      if (n > 1) {
	 n = ROUNDUP(n, 64);
      }
      if ((sbuf.buf = malloc(n * sizeof (char))) == NULL) {
	 sbuf.error = TRUE;
	 goto error;
      }
      sbuf.size = n;
   }

   // shut compile up
#if !defined(NO_FLOATING_POINT)
   expchar = 0;
   expsize = 0;
   lead = 0;
   ndig = 0;
   nseps = 0;
   nrepeats = 0;
#endif
   ulval = 0;
   ujval = 0;

   /*
    * Scan the format for conversions (`%' character).
    */
   for (;;) {
      for (cp = fmt; (ch = *fmt) != '\0' && ch != '%'; fmt++)
         /* void */;
      if ((n = fmt - cp) != 0) {
         if ((unsigned)ret + n > INT_MAX) {
            ret = EOF;
            goto error;
         }
         PRINT(cp, n);
         ret += n;
      }
      if (ch == '\0') {
         goto done;
      }
      fmt++;      /* skip over '%' */

      flags = 0;
      dprec = 0;
      width = 0;
      prec = -1;
      sign = '\0';
      ox[1] = '\0';

     rflag:      ch = *fmt++;
     reswitch:   switch (ch) {
      case ' ':
         /*-
          * ``If the space and + flags both appear, the space flag will be
          *   ignored.'' -- ANSI X3J11
          */

         if (!sign) {
            sign = ' ';
         }
         goto rflag;
      case '#':
         flags |= ALT;
         goto rflag;
      case '*':
         /*-
          * ``A negative field width argument is taken as a flag followed by
          *   a positive field width.''-- ANSI X3J11
          *
          * They don't exclude field widths read from args.
          */
         GETASTER (width);
         if (width >= 0) {
            goto rflag;
         }
         width = -width;
         /* FALLTHROUGH */
      case '-':
         flags |= LADJUST;
         goto rflag;
      case '+':
         sign = '+';
         goto rflag;
      case '\'':
         flags |= GROUPING;
#if !defined __ANDROID__
         thousands_sep = thousands_sepIn;
         grouping = groupingIn;
#else
         /*
          * Struct lconv is not working! The code below is a workaround.
          */
         thousands_sep = ',';
#endif
	 /*
	  * Grouping should not begin with 0, but it nevertheless does (see
          * bug 281072) and makes the formatting code behave badly, so we
          * fix it up.
	  */

	 if (grouping != NULL && *grouping == '\0') {
	    static char g[] = { CHAR_MAX, '\0' };

	    grouping = g;
	 }
         goto rflag;
      case '.':
         if ((ch = *fmt++) == '*') {
            GETASTER (prec);
            goto rflag;
         }
         prec = 0;
         while (is_digit(ch)) {
            prec = 10 * prec + to_digit(ch);
            ch = *fmt++;
         }
         goto reswitch;
      case '0':
         /*-
          * ``Note that 0 is taken as a flag, not as the beginning of a
          *   field width.''  -- ANSI X3J11
          */

         flags |= ZEROPAD;
         goto rflag;
      case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
         n = 0;
         do {
            n = 10 * n + to_digit(ch);
            ch = *fmt++;
         } while (is_digit(ch));
         if (ch == '$') {
            nextarg = n;
	    FIND_ARGUMENTS();
            goto rflag;
         }
         width = n;
         goto reswitch;
      case 'h':
         if (flags & SHORTINT) {
            flags &= ~SHORTINT;
            flags |= CHARINT;
         } else {
            flags |= SHORTINT;
         }
         goto rflag;
      case 'j':
         flags |= INTMAXT;
         goto rflag;
      case 'I':
         /* could be I64 - long long int is 64bit */
         if (fmt[0] == '6' && fmt[1] == '4') {
            fmt += 2;
            flags |= LLONGINT;
            goto rflag;
         }
         /* could be I32 - normal int is 32bit */
         if (fmt[0] == '3' && fmt[1] == '2') {
            fmt += 2;
            /* flags |= normal integer - it is 32bit for all our targets */
            goto rflag;
         }
         /*
          * I alone - use Microsoft's semantic as size_t modifier.  We do
          * not support glibc's semantic to use alternative digits.
          */
         flags |= SIZET;
         goto rflag;
      case 'l':
         if (flags & LONGINT) {
            flags &= ~LONGINT;
            flags |= LLONGINT;
         } else
            flags |= LONGINT;
         goto rflag;
      case 'L':
      case 'q':
         flags |= LLONGINT;   /* not necessarily */
         goto rflag;
      case 't':
         flags |= PTRDIFFT;
         goto rflag;
      case 'Z':
      case 'z':
         flags |= SIZET;
         goto rflag;
      case 'C':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'c':
         if (flags & LONGINT) {
            static const mbstate_t initial;
            mbstate_t mbs;
            size_t mbseqlen;

	    mbs = initial;
            mbseqlen = wcrtomb(cp = buf, (wchar_t) GETARG(wint_t), &mbs);
            if (mbseqlen == (size_t) -1) {
               sbuf.error = TRUE;
               goto error;
            }
            size = (int) mbseqlen;
         } else {
            *(cp = buf) = GETARG(int);
            size = 1;
         }
         sign = '\0';
         break;
      case 'D':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'd':
      case 'i':
         if (flags & INTMAX_SIZE) {
            ujval = SJARG();
            if ((intmax_t) ujval < 0) {
               ujval = -ujval;
               sign = '-';
            }
         } else {
            ulval = SARG();
            if ((long) ulval < 0) {
               ulval = -ulval;
               sign = '-';
            }
         }
         base = 10;
         goto number;

#if !defined(NO_FLOATING_POINT)
      case 'e':
      case 'E':
         expchar = ch;
         if (prec < 0) {  /* account for digit before decpt */
            prec = DEFPREC + 1;
         } else {
            prec++;
         }
         goto fp_begin;
      case 'f':
      case 'F':
         expchar = '\0';
         goto fp_begin;
      case 'g':
      case 'G':
         expchar = ch - ('g' - 'e');
         if (prec == 0) {
            prec = 1;
         }
      fp_begin:
         if (prec < 0) {
            prec = DEFPREC;
         }
         if (dtoaresult != NULL) {
            freedtoa(dtoaresult);
         }
         if (flags & LLONGINT) {
            fparg.ldbl = GETARG(long double);
#if defined NO_LDTOA
            dtoaresult = NULL;
            /*
             * Below is to keep compiler happy
             */
            signflag = -1;
            expt = 0;
            dtoaend = NULL;
#else
            dtoaresult = cp = ldtoa(&fparg.ldbl, expchar ? 2 : 3, prec,
                                     &expt, &signflag, &dtoaend);
#endif
         } else {
            fparg.dbl = GETARG(double);
#if defined NO_DTOA
            dtoaresult = NULL;
            /*
             * Below is to keep compiler happy
             */
            signflag = -1;
            expt = 0;
            dtoaend = NULL;
#else
            dtoaresult = cp = dtoa(fparg.dbl, expchar ? 2 : 3, prec,
                                   &expt, &signflag, &dtoaend);
#endif
         }

         /* Our dtoa / ldtoa call strdup(), which can fail. PR319844 */
         if (dtoaresult == NULL) {
            sbuf.error = TRUE;
            goto error;
         }

         flags |= FPT;

         if ((expt == 9999) ||
             ((Str_Strcasecmp(cp, "-inf") == 0) ||
              (Str_Strcasecmp(cp, "inf") == 0) ||
              (Str_Strcasecmp(cp, "nan") == 0))) {
            if (*cp == '-') {
               sign = '-';
               cp++;
            }

            cp = islower(ch) ? Str_ToLower(cp) : Str_ToUpper(cp);

            expt = INT_MAX;
            size = strlen(cp);
            break;
         }

         if (signflag) {
            sign = '-';
         }

         ndig = dtoaend - cp;
         if (ch == 'g' || ch == 'G') {
            if (expt > -4 && expt <= prec) {
               /* Make %[gG] smell like %[fF] */
               expchar = '\0';
               if (flags & ALT) {
                  prec -= expt;
               } else {
                  prec = ndig - expt;
               }

               if (prec < 0) {
                  prec = 0;
               }
            } else {
               /*
                * Make %[gG] smell like %[eE], but trim trailing zeroes
                * if no # flag.
                */

               if (!(flags & ALT)) {
                  prec = ndig;
               }
            }
         }
         if (expchar) {
            expsize = BSDFmt_Exponent(expstr, expt - 1, expchar);
            size = expsize + prec;
            if (prec > 1 || flags & ALT) {
               ++size;
            }
         } else {
            /* space for digits before decimal point */
            if (expt > 0) {
               size = expt;
            } else {  /* "0" */
               size = 1;
            }
            /* space for decimal pt and following digits */
            if (prec || flags & ALT) {
               size += prec + 1;
            }
            if (grouping && expt > 0) {
               /* space for thousands' grouping */
               nseps = nrepeats = 0;
               lead = expt;
               while (*grouping != CHAR_MAX) {
                  if (lead <= *grouping) {
                     break;
                  }
                  lead -= *grouping;
                  if (*(grouping + 1)) {
                     nseps++;
                     grouping++;
                  } else {
                     nrepeats++;
                  }
               }
               size += nseps + nrepeats;
            } else {
               lead = expt;
            }
         }
         break;
#endif /* !NO_FLOATING_POINT */

      case 'n':
         /*
          * Assignment-like behavior is specified if the value overflows or
          * is otherwise unrepresentable. C99 says to use `signed char'
          * for %hhn conversions.
          */

         if (flags & LLONGINT) {
            *GETARG(long long *) = ret;
         }
         else if (flags & SIZET) {
            *GETARG(size_t *) = (size_t)ret;
         }
         else if (flags & PTRDIFFT) {
            *GETARG(ptrdiff_t *) = ret;
         }
         else if (flags & INTMAXT) {
            *GETARG(intmax_t *) = ret;
         }
         else if (flags & LONGINT) {
            *GETARG(long *) = ret;
         }
         else if (flags & SHORTINT) {
            *GETARG(short *) = ret;
         }
         else if (flags & CHARINT) {
            *GETARG(signed char *) = ret;
         } else {
            *GETARG(int *) = ret;
         }
         continue;   /* no output */
      case 'O':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'o':
         if (flags & INTMAX_SIZE) {
            ujval = UJARG();
         } else {
            ulval = UARG();
         }
         base = 8;
         goto nosign;
      case 'p':
         /*-
          * ``The argument shall be a pointer to void. The value of the
          *   pointer is converted to a sequence of printable characters, in
          *   an implementation- defined manner.''   -- ANSI X3J11
          */

         ujval = (uintmax_t)(uintptr_t) GETARG(void *);
         base = 16;
         xdigs = xdigs_upper;
         flags = flags | INTMAXT;
         /*
          * PR 103201
          * VisualC sscanf doesn't grok '0x', so prefix zeroes.
          */
//       ox[1] = 'x';
         goto nosign;
      case 'S':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 's':
         if (flags & LONGINT) {
            wchar_t *wcp;

            /* Argument is wchar_t * */
            if (convbuf != NULL) {
               free(convbuf);
               convbuf = NULL;
            }
            if ((wcp = GETARG(wchar_t *)) == NULL) {
               cp = "(null)";
            } else {
               convbuf = BSDFmt_WCharToUTF8(wcp, prec);
               if (convbuf == NULL) {
                  sbuf.error = TRUE;
                  goto error;
               }
               cp = convbuf;
            }
         } else if ((cp = GETARG(char *)) == NULL) {
            /* Argument is char * */
            cp = "(null)";
         }

         if (prec >= 0) {
            /*
             * can't use strlen; can only look for the NUL in the first
             * `prec' characters, and strlen() will go further.
             */

            char *p = memchr(cp, 0, (size_t)prec);

            if (p == NULL) {
               size = prec;
            } else {
               size = p - cp;

               if (size > prec) {
                  size = prec;
               }
            }
            size = CodeSet_Utf8FindCodePointBoundary(cp, size);
         } else {
            size = strlen(cp);
         }
         sign = '\0';
         break;
      case 'U':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'u':
         if (flags & INTMAX_SIZE)
            ujval = UJARG();
         else
            ulval = UARG();
         base = 10;
         goto nosign;
      case 'X':
         xdigs = xdigs_upper;
         goto hex;
      case 'x':
         xdigs = xdigs_lower;
      hex:
         if (flags & INTMAX_SIZE) {
            ujval = UJARG();
         } else {
            ulval = UARG();
         }
         base = 16;
         /* leading 0x/X only if non-zero */
         if (flags & ALT && (flags & INTMAX_SIZE ? ujval != 0 : ulval != 0)) {
            ox[1] = ch;
         }

         flags &= ~GROUPING;
         /* unsigned conversions */
      nosign:         sign = '\0';
         /*-
          * ``... diouXx conversions ... if a precision is specified, the
          *       0 flag will be ignored.'' -- ANSI X3J11
          */
      number:         if ((dprec = prec) >= 0) {
                         flags &= ~ZEROPAD;
                      }

         /*-
          * ``The result of converting a zero value with an explicit
          *   precision of zero is no characters.'' -- ANSI X3J11
          *
          * ``The C Standard is clear enough as is. The call printf("%#.0o", 0)
          *    should print 0.'' -- Defect Report #151
          */

         cp = buf + INT_CONV_BUF;
         if (flags & INTMAX_SIZE) {
            if (ujval != 0 || prec != 0 ||
                (flags & ALT && base == 8)) {
               cp = BSDFmt_UJToA(ujval, cp, base,
                                 flags & ALT, xdigs,
                                 flags & GROUPING, thousands_sep,
                                 grouping);
            }
         } else {
            if (ulval != 0 || prec != 0 ||
                (flags & ALT && base == 8)) {
               cp = __ultoa(ulval, cp, base,
                            flags & ALT, xdigs,
                            flags & GROUPING, thousands_sep,
                            grouping);
            }
         }
         size = buf + INT_CONV_BUF - cp;
         if (size > INT_CONV_BUF) {   /* should never happen */
            abort();
         }
         break;
      default:   /* "%?" prints ?, unless ? is NUL */
         if (ch == '\0') {
            goto done;
         }
         /* pretend it was %c with argument ch */
         cp = buf;
         *cp = ch;
         size = 1;
         sign = '\0';
         break;
      }

      /*
       * All reasonable formats wind up here.  At this point, `cp' points to
       * a string which (if not flags&LADJUST) should be padded out to `width'
       * places.  If flags&ZEROPAD, it should first be prefixed by any sign
       * or other prefix; otherwise, it should be blank padded before the
       * prefix is emitted. After any left-hand padding and prefixing, emit
       * zeroes required by a decimal [diouxX] precision, then print the
       * string proper, then emit zeroes required by any leftover floating
       * precision; finally, if LADJUST, pad with blanks.
       *
       * Compute actual size, so we know how much to pad. size excludes
       * decimal prec; realsz includes it.
       */

      realsz = dprec > size ? dprec : size;
      if (sign) {
         realsz++;
      }
      if (ox[1]) {
         realsz += 2;
      }

      prsize = width > realsz ? width : realsz;
      if ((unsigned)ret + prsize > INT_MAX) {
         ret = EOF;
         goto error;
      }

      /* right-adjusting blank padding */
      if ((flags & (LADJUST | ZEROPAD)) == 0) {
         PAD(width - realsz, blanks);
      }

      /* prefix */
      if (sign) {
         PRINT(&sign, 1);
      }

#if !defined(NO_FLOATING_POINT)
      /* NAN, INF and -INF */
      if ((flags & FPT) && (expt == INT_MAX)) {
         PRINT(cp, size);
         goto skip;
      }
#endif

      if (ox[1]) {   /* ox[1] is either x, X, or \0 */
         ox[0] = '0';
         PRINT(ox, 2);
      }

      /* right-adjusting zero padding */
      if ((flags & (LADJUST|ZEROPAD)) == ZEROPAD) {
         PAD(width - realsz, zeroes);
      }

      /* leading zeroes from decimal precision */
      PAD(dprec - size, zeroes);

      /* the string or number proper */
      if (flags & FPT) { /* glue together f_p fragments */
#if defined(NO_FLOATING_POINT)
         NOT_IMPLEMENTED();
#else
         if (expchar) {   /* %[fF] or sufficiently short %[gG] */
            if (prec > 1 || flags & ALT) {
               buf[0] = *cp++;
               buf[1] = *decimal_point;
               PRINT(buf, 2);
               if (ndig > 0) {
                  PRINT(cp, ndig - 1);
                  PAD(prec - ndig, zeroes);
               } else {
                  PAD(prec - ndig - 1, zeroes);
               }
            } else {  /* XeYYY */
               PRINT(cp, 1);
            }

            PRINT(expstr, expsize);
         } else {   /* %[eE] or sufficiently long %[gG] */
            if (expt <= 0) {
               PRINT(zeroes, 1);
               if (prec || flags & ALT) {
                  PRINT(decimal_point, 1);
               }

               PAD(-expt, zeroes);
               /* already handled initial 0's */
               prec += expt;
            } else {
               PRINTANDPAD(cp, dtoaend, lead, zeroes);
               cp += lead;
               if (grouping) {
                  while (nseps > 0 || nrepeats > 0) {
                     if (nrepeats > 0) {
                        nrepeats--;
                     } else {
                        grouping--;
                        nseps--;
                     }
                     PRINT(&thousands_sep, 1);
                     PRINTANDPAD(cp, dtoaend, *grouping, zeroes);
                     cp += *grouping;
                  }
                  if (cp > dtoaend) {
                     cp = dtoaend;
                  }
               }
               if (prec || flags & ALT) {
                  PRINT(decimal_point, 1);
               }
            }
            PRINTANDPAD(cp, dtoaend, prec, zeroes);
         }
#endif
      } else {
         PRINT(cp, size);
      }

skip:
      /* left-adjusting padding (always blank) */
      if (flags & LADJUST) {
         PAD(width - realsz, blanks);
      }

      /* finally, adjust ret */
      ret += prsize;

      FLUSH();   /* copy out the I/O vectors */
   }

done:
   FLUSH();

   /*
    * Always null terminate, unless buffer is size 0.
    */

   ASSERT(!sbuf.error && ret >= 0);
   if (sbuf.size <= 0) {
      ASSERT(!sbuf.alloc);
   } else {
      ASSERT(sbuf.index < sbuf.size);
      sbuf.buf[sbuf.index] = '\0';
   }

error:
#ifndef _WIN32
   va_end(orgap);
#endif
#if !defined(NO_FLOATING_POINT)
   if (dtoaresult != NULL) {
      freedtoa(dtoaresult);
   }
#endif
   if (convbuf != NULL) {
      free(convbuf);
      convbuf = NULL;
   }
   if (sbuf.error) {
      ret = EOF;
   }
   if ((argtable != NULL) && (argtable != statargtable)) {
      free (argtable);
   }

   // return allocated buffer on success, free it on failure
   if (sbuf.alloc) {
      if (ret < 0) {
	 free(sbuf.buf);
      } else {
	 *outbuf = sbuf.buf;
      }
   }

   return (ret);
   /* NOTREACHED */

#undef PRINT
#undef PAD
#undef PRINTANDPAD
#undef FLUSH
#undef GETARG
#undef SARG
#undef SJARG
#undef UARG
#undef UJARG
#undef GETASTER
#undef FIND_ARGUMENTS
}

int
bsd_vsnprintf_c_locale(char **outbuf,
                       size_t bufSize,
                       const char *fmt0,
                       va_list ap)
{
   char thousands_sep;
   char *decimal_point;
   static char dp = '.';

   /*
    * Perform a "%f" conversion always using the locale associated
    * with the C locale - "," for thousands, '.' for decimal point.
    */

   thousands_sep = ',';
   decimal_point = &dp;

   return bsd_vsnprintf_core(outbuf, NULL, thousands_sep, decimal_point,
                             bufSize, fmt0, ap);
}

int
bsd_vsnprintf(char **outbuf,
              size_t bufSize,
              const char *fmt0,
              va_list ap)
{
   char *grouping;
   char thousands_sep;
   char *decimal_point;

#if defined(__ANDROID__)
   static char dp = '.';

   /*
    * Struct lconv is not working! The code below is a workaround.
    */
   grouping = NULL;
   thousands_sep = ',';
   decimal_point = &dp;
#else
   grouping =        localeconv()->grouping;
   thousands_sep = *(localeconv()->thousands_sep);
   decimal_point =   localeconv()->decimal_point;
#endif

   return bsd_vsnprintf_core(outbuf, grouping, thousands_sep, decimal_point,
                             bufSize, fmt0, ap);
}

/*
 * Find all arguments when a positional parameter is encountered.  Returns a
 * table, indexed by argument number, of pointers to each arguments.  The
 * initial argument table should be an array of STATIC_ARG_TBL_SIZE entries.
 * It will be replaces with a malloc-ed one if it overflows.
 */

static void
__find_arguments (const char *fmt0, va_list ap, union arg **argtable)
{
   char *fmt;      /* format string */
   int ch;         /* character from fmt */
   int n, n2;      /* handy integer (short term usage) */
   char *cp;      /* handy char pointer (short term usage) */
   int flags;      /* flags as above */
   enum typeid *typetable; /* table of types */
   enum typeid stattypetable [STATIC_ARG_TBL_SIZE];
   int tablesize;      /* current size of type table */
   int tablemax;      /* largest used index in table */
   int nextarg;      /* 1-based argument index */

   /*
    * Add an argument type to the table, expanding if necessary.
    */
#define ADDTYPE(type)                                                   \
   ((nextarg >= tablesize) ?                                            \
    __grow_type_table(nextarg, &typetable, &tablesize) : (void)0,       \
    (nextarg > tablemax) ? tablemax = nextarg : 0,                      \
    typetable[nextarg++] = type)

#define   ADDSARG()                                             \
   ((flags&INTMAXT) ? ADDTYPE(T_INTMAXT) :                      \
    ((flags&SIZET) ? ADDTYPE(T_SIZET) :                         \
     ((flags&PTRDIFFT) ? ADDTYPE(T_PTRDIFFT) :                  \
      ((flags&LLONGINT) ? ADDTYPE(T_LLONG) :                    \
       ((flags&LONGINT) ? ADDTYPE(T_LONG) : ADDTYPE(T_INT))))))

#define   ADDUARG()                                                     \
   ((flags&INTMAXT) ? ADDTYPE(T_UINTMAXT) :                             \
    ((flags&SIZET) ? ADDTYPE(T_SIZET) :                                 \
     ((flags&PTRDIFFT) ? ADDTYPE(T_PTRDIFFT) :                          \
      ((flags&LLONGINT) ? ADDTYPE(T_U_LLONG) :                          \
       ((flags&LONGINT) ? ADDTYPE(T_U_LONG) : ADDTYPE(T_U_INT))))))

   /*
    * Add * arguments to the type array.
    */
#define ADDASTER()                              \
   n2 = 0;                                      \
   cp = fmt;                                    \
   while (is_digit(*cp)) {                      \
      n2 = 10 * n2 + to_digit(*cp);             \
      cp++;                                     \
   }                                            \
   if (*cp == '$') {                            \
      int hold = nextarg;                       \
      nextarg = n2;                             \
      ADDTYPE (T_INT);                          \
      nextarg = hold;                           \
      fmt = ++cp;                               \
   } else {                                     \
      ADDTYPE (T_INT);                          \
   }
   fmt = (char *)fmt0;
   typetable = stattypetable;
   tablesize = STATIC_ARG_TBL_SIZE;
   tablemax = 0;
   nextarg = 1;
   for (n = 0; n < STATIC_ARG_TBL_SIZE; n++)
      typetable[n] = T_UNUSED;

   /*
    * Scan the format for conversions (`%' character).
    */
   for (;;) {
      for (cp = fmt; (ch = *fmt) != '\0' && ch != '%'; fmt++)
         /* void */;
      if (ch == '\0')
         goto done;
      fmt++;      /* skip over '%' */

      flags = 0;

     rflag:      ch = *fmt++;
     reswitch:   switch (ch) {
      case ' ':
      case '#':
         goto rflag;
      case '*':
         ADDASTER ();
         goto rflag;
      case '-':
      case '+':
      case '\'':
         goto rflag;
      case '.':
         if ((ch = *fmt++) == '*') {
            ADDASTER ();
            goto rflag;
         }
         while (is_digit(ch)) {
            ch = *fmt++;
         }
         goto reswitch;
      case '0':
         goto rflag;
      case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
         n = 0;
         do {
            n = 10 * n + to_digit(ch);
            ch = *fmt++;
         } while (is_digit(ch));
         if (ch == '$') {
            nextarg = n;
            goto rflag;
         }
         goto reswitch;
      case 'h':
         if (flags & SHORTINT) {
            flags &= ~SHORTINT;
            flags |= CHARINT;
         } else
            flags |= SHORTINT;
         goto rflag;
      case 'j':
         flags |= INTMAXT;
         goto rflag;
      case 'I':
         /* could be I64 - long long int is 64bit */
         if (fmt[0] == '6' && fmt[1] == '4') {
            fmt += 2;
            flags |= LLONGINT;
            goto rflag;
         }
         /* could be I32 - normal int is 32bit */
         if (fmt[0] == '3' && fmt[1] == '2') {
            fmt += 2;
            /* flags |= normal integer - it is 32bit for all our targets */
            goto rflag;
         }
         /*
          * I alone - use Microsoft's semantic as size_t modifier.  We do
          * not support glibc's semantic to use alternative digits.
          */
         flags |= SIZET;
         goto rflag;
      case 'l':
         if (flags & LONGINT) {
            flags &= ~LONGINT;
            flags |= LLONGINT;
         } else
            flags |= LONGINT;
         goto rflag;
      case 'L':
      case 'q':
         flags |= LLONGINT;   /* not necessarily */
         goto rflag;
      case 't':
         flags |= PTRDIFFT;
         goto rflag;
      case 'Z':
      case 'z':
         flags |= SIZET;
         goto rflag;
      case 'C':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'c':
         if (flags & LONGINT)
            ADDTYPE(T_WINT);
         else
            ADDTYPE(T_INT);
         break;
      case 'D':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'd':
      case 'i':
         ADDSARG();
         break;
#if !defined(NO_FLOATING_POINT)
      case 'a':
      case 'A':
      case 'e':
      case 'E':
      case 'f':
      case 'g':
      case 'G':
         if (flags & LLONGINT)
            ADDTYPE(T_LONG_DOUBLE);
         else
            ADDTYPE(T_DOUBLE);
         break;
#endif /* !NO_FLOATING_POINT */
      case 'n':
         if (flags & INTMAXT)
            ADDTYPE(TP_INTMAXT);
         else if (flags & PTRDIFFT)
            ADDTYPE(TP_PTRDIFFT);
         else if (flags & SIZET)
            ADDTYPE(TP_SIZET);
         else if (flags & LLONGINT)
            ADDTYPE(TP_LLONG);
         else if (flags & LONGINT)
            ADDTYPE(TP_LONG);
         else if (flags & SHORTINT)
            ADDTYPE(TP_SHORT);
         else if (flags & CHARINT)
            ADDTYPE(TP_SCHAR);
         else
            ADDTYPE(TP_INT);
         continue;   /* no output */
      case 'O':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'o':
         ADDUARG();
         break;
      case 'p':
         ADDTYPE(TP_VOID);
         break;
      case 'S':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 's':
         if (flags & LONGINT)
            ADDTYPE(TP_WCHAR);
         else
            ADDTYPE(TP_CHAR);
         break;
      case 'U':
         flags |= LONGINT;
         /*FALLTHROUGH*/
      case 'u':
      case 'X':
      case 'x':
         ADDUARG();
         break;
      default:   /* "%?" prints ?, unless ? is NUL */
         if (ch == '\0')
            goto done;
         break;
      }
   }
  done:
   /*
    * Build the argument table.
    */
   if (tablemax >= STATIC_ARG_TBL_SIZE) {
      *argtable = (union arg *)
         malloc (sizeof (union arg) * (tablemax + 1));
   }

   (*argtable) [0].intarg = 0;
   for (n = 1; n <= tablemax; n++) {
      switch (typetable [n]) {
      case T_UNUSED: /* whoops! */
         (*argtable) [n].intarg = va_arg (ap, int);
         break;
      case TP_SCHAR:
         (*argtable) [n].pschararg = va_arg (ap, signed char *);
         break;
      case TP_SHORT:
         (*argtable) [n].pshortarg = va_arg (ap, short *);
         break;
      case T_INT:
         (*argtable) [n].intarg = va_arg (ap, int);
         break;
      case T_U_INT:
         (*argtable) [n].uintarg = va_arg (ap, unsigned int);
         break;
      case TP_INT:
         (*argtable) [n].pintarg = va_arg (ap, int *);
         break;
      case T_LONG:
         (*argtable) [n].longarg = va_arg (ap, long);
         break;
      case T_U_LONG:
         (*argtable) [n].ulongarg = va_arg (ap, unsigned long);
         break;
      case TP_LONG:
         (*argtable) [n].plongarg = va_arg (ap, long *);
         break;
      case T_LLONG:
         (*argtable) [n].longlongarg = va_arg (ap, long long);
         break;
      case T_U_LLONG:
         (*argtable) [n].ulonglongarg = va_arg (ap, unsigned long long);
         break;
      case TP_LLONG:
         (*argtable) [n].plonglongarg = va_arg (ap, long long *);
         break;
      case T_PTRDIFFT:
         (*argtable) [n].ptrdiffarg = va_arg (ap, ptrdiff_t);
         break;
      case TP_PTRDIFFT:
         (*argtable) [n].pptrdiffarg = va_arg (ap, ptrdiff_t *);
         break;
      case T_SIZET:
         (*argtable) [n].sizearg = va_arg (ap, size_t);
         break;
      case TP_SIZET:
         (*argtable) [n].psizearg = va_arg (ap, size_t *);
         break;
      case T_INTMAXT:
         (*argtable) [n].intmaxarg = va_arg (ap, intmax_t);
         break;
      case T_UINTMAXT:
         (*argtable) [n].uintmaxarg = va_arg (ap, uintmax_t);
         break;
      case TP_INTMAXT:
         (*argtable) [n].pintmaxarg = va_arg (ap, intmax_t *);
         break;
#if !defined(NO_FLOATING_POINT)
      case T_DOUBLE:
         (*argtable) [n].doublearg = va_arg (ap, double);
         break;
      case T_LONG_DOUBLE:
         (*argtable) [n].longdoublearg = va_arg (ap, long double);
         break;
#endif
      case TP_CHAR:
         (*argtable) [n].pchararg = va_arg (ap, char *);
         break;
      case TP_VOID:
         (*argtable) [n].pvoidarg = va_arg (ap, void *);
         break;
      case T_WINT:
         (*argtable) [n].wintarg = va_arg (ap, wint_t);
         break;
      case TP_WCHAR:
         (*argtable) [n].pwchararg = va_arg (ap, wchar_t *);
         break;
      }
   }

   if ((typetable != NULL) && (typetable != stattypetable))
      free (typetable);

#undef ADDTYPE
#undef ADDSARG
#undef ADDUARG
#undef ADDASTER
}

/*
 * Increase the size of the type table.
 */
static void
__grow_type_table (int nextarg, enum typeid **typetable, int *tablesize)
{
   enum typeid *const oldtable = *typetable;
   const int oldsize = *tablesize;
   enum typeid *newtable;
   int n, newsize = oldsize * 2;

   if (newsize < nextarg + 1) {
      newsize = nextarg + 1;
   }

   if (oldsize == STATIC_ARG_TBL_SIZE) {
      if ((newtable = malloc(newsize * sizeof(enum typeid))) == NULL) {
         abort();         /* XXX handle better */
      }

      memmove(newtable, oldtable, oldsize * sizeof(enum typeid));
   } else {
      newtable = realloc(oldtable, newsize * sizeof(enum typeid));

      if (newtable == NULL) {
         abort();         /* XXX handle better */
      }
   }
   for (n = oldsize; n < newsize; n++) {
      newtable[n] = T_UNUSED;
   }

   *typetable = newtable;
   *tablesize = newsize;
}


#if !defined(NO_FLOATING_POINT)
int
BSDFmt_Exponent(char *p0, int exp, int fmtch)
{
   char *p, *t;
   char expbuf[MAXEXPDIG];

   p = p0;
   *p++ = fmtch;
   if (exp < 0) {
      exp = -exp;
      *p++ = '-';
   } else {
      *p++ = '+';
   }

   t = expbuf + MAXEXPDIG;

   if (exp < 10) {
      *p++ = '0';
   }

// See PR 704706: POSIX specifies that exponents < 100 only have 2 digits
//   if (exp < 100) {
//      *p++ = '0';
//   }

   if (exp > 9) {
      do {
         *--t = to_char(exp % 10);
      } while ((exp /= 10) > 9);

      *--t = to_char(exp);

      for (; t < expbuf + MAXEXPDIG; *p++ = *t++);
   } else {
      *p++ = to_char(exp);
   }

   return (p - p0);
}
#endif /* !NO_FLOATING_POINT */

#endif /* !STR_NO_WIN32_LIBS|*BSD */
