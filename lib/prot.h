/* prot.h -- stdio-like module that handles IMAP protection mechanisms
 $Id: prot.h,v 1.17.4.1 1999/10/24 22:08:04 leg Exp $
 
 #        Copyright 1998 by Carnegie Mellon University
 #
 #                      All Rights Reserved
 #
 # Permission to use, copy, modify, and distribute this software and its
 # documentation for any purpose and without fee is hereby granted,
 # provided that the above copyright notice appear in all copies and that
 # both that copyright notice and this permission notice appear in
 # supporting documentation, and that the name of CMU not be
 # used in advertising or publicity pertaining to distribution of the
 # software without specific, written prior permission.
 #
 # CMU DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 # ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 # CMU BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 # ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 # WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 # ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 # SOFTWARE.
 *
 */

#ifndef INCLUDED_PROT_H
#define INCLUDED_PROT_H

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <sasl.h>

#ifndef P
#ifdef __STDC__
#define P(x) x
#else
#define P(x) ()
#endif
#endif

#define PROT_BUFSIZE 4096

struct protstream;

typedef void prot_readcallback_t P((struct protstream *s, void *rock));

struct protstream {
    char *ptr;
    int cnt;
    int fd;
    int write;
    int logfd;
    sasl_conn_t *conn;
    int saslssf;
    time_t *log_timeptr;
    const char *(*func)();
    void *state;
    int maxplain;
    const char *error;
    int eof;
    int read_timeout;
    struct protstream *flushonread;
    prot_readcallback_t *readcallback_proc;
    void *readcallback_rock;
    int buf_size;
    char *buf;
};

#define prot_getc(s) ((s)->cnt-- > 0 ? (int)*(s)->ptr++ : prot_fill(s))
#define prot_ungetc(c, s) ((s)->cnt++, (*--(s)->ptr = (c)))
#define prot_putc(c, s) ((*(s)->ptr++ = (c)), --(s)->cnt == 0 ? prot_flush(s) : 0)

extern struct protstream *prot_new P((int fd, int write));
extern int prot_free P((struct protstream *s));
extern int prot_setlog P((struct protstream *s, int fd));
extern int prot_setlogtime P((struct protstream *s, time_t *ptr));
extern int prot_setsasl P((struct protstream *s, sasl_conn_t *conn));
extern int prot_settimeout P((struct protstream *s, int timeout));
extern int prot_setflushonread P((struct protstream *s,
				  struct protstream *flushs));
extern int prot_setreadcallback P((struct protstream *s,
				   prot_readcallback_t *proc, void *rock));
extern const char *prot_error P((struct protstream *s));
extern int prot_rewind P((struct protstream *s));
extern int prot_fill P((struct protstream *s));
extern int prot_flush P((struct protstream *s));
extern int prot_write P((struct protstream *s, const char *buf, unsigned len));
extern int prot_printf(struct protstream *, const char *, ...);
extern int prot_read P((struct protstream *s, char *buf, unsigned size));
extern char *prot_fgets P((char *buf, unsigned size, struct protstream *s));

#endif /* INCLUDED_PROT_H */
