/* prot.c -- stdio-like module that handles IMAP protection mechanisms
 *
 *        Copyright 1998 by Carnegie Mellon University
 *
 *                      All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and that
 * both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of CMU not be
 * used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * CMU DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 * ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 * CMU BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 * ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 *
 */
/*
 * $Id: prot.c,v 1.42.4.2 2000/01/13 01:29:20 leg Exp $
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#ifdef __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "prot.h"
#include "xmalloc.h"

/*
 * Create a new protection stream for file descriptor 'fd'.  Stream
 * will be used for writing iff 'write' is nonzero.
 */
struct protstream *prot_new(fd, write)
int fd;
int write;
{
    struct protstream *newstream;

    newstream = (struct protstream *) xmalloc(sizeof(struct protstream));
    newstream->buf = (char *) xmalloc(sizeof(char) * (PROT_BUFSIZE + 4));
    newstream->buf_size = PROT_BUFSIZE;
    newstream->ptr = newstream->buf;
    newstream->cnt = write ? PROT_BUFSIZE : 0;
    newstream->maxplain = PROT_BUFSIZE;
    newstream->fd = fd;
    newstream->write = write;
    newstream->logfd = -1;
    newstream->log_timeptr = 0;
    newstream->func = 0;
    newstream->state = 0;
    newstream->error = 0;
    newstream->eof = 0;
    newstream->read_timeout = 0;
    newstream->dontblock = 0;
    newstream->flushonread = 0;
    newstream->readcallback_proc = 0;
    newstream->readcallback_rock = 0;
    newstream->conn = NULL;
    newstream->saslssf=0;

#ifdef HAVE_SSL
    newstream->tls_conn=NULL;
#endif /* HAVE_SSL */

    return newstream;
}

/*
 * Free a protection stream
 */
int prot_free(s)
struct protstream *s;
{
    free(s->buf);
    free((char*)s);
    return 0;
}

/*
 * Set the logging file descriptor for stream 's' to be 'fd'.
 */
int prot_setlog(s, fd)
struct protstream *s;
int fd;
{
    s->logfd = fd;
    return 0;
}

/*
 * Start logging timing information for stream 's'.
 */
int prot_setlogtime(s, ptr)
struct protstream *s;
time_t *ptr;
{
    s->log_timeptr = ptr;
    time(s->log_timeptr);
    return 0;
}

#ifdef HAVE_SSL

/*
 * Turn on TLS for this connection
 */

int prot_settls(struct protstream *s, SSL *tlsconn)
{
  s->tls_conn = tlsconn;

  return 0;
}

#endif /* HAVE_SSL */

/*
 * Turn on SASL for this connection
 */

int prot_setsasl(s, conn)
struct protstream *s;
sasl_conn_t *conn;
{
  int *ssfp;
  int result;

  /* flush first if need be */
  if (s->write && s->ptr != s->buf) prot_flush(s);

  s->conn=conn;

  result = sasl_getprop(conn, SASL_SSF, (void **) &ssfp);
  if (result != SASL_OK)
    return 1;

  s->saslssf = *ssfp;

  if (s->write) {
    int result;
    int *maxp, max;

    /* ask SASL for layer max */
    result = sasl_getprop(conn, SASL_MAXOUTBUF, (void **) &maxp);
    max = *maxp;
    if (result != SASL_OK)
      return 1;

    if (max == 0 || max > PROT_BUFSIZE) {
	/* max = 0 means unlimited, and we can't go bigger */
	max = PROT_BUFSIZE;
    }
    
    max-=50; /* account for extra foo incurred from layers */

    s->maxplain=max;
    s->cnt=max;
  }
  else if (s->cnt) {  /* XXX what does this do? */
    s->cnt = 0;
  }

  return 0;
}

/*
 * Set the read timeout for the stream 's' to 'timeout' seconds.
 * 's' must have been created for reading.
 */
int prot_settimeout(s, timeout)
struct protstream *s;
int timeout;
{
    s->read_timeout = timeout;
    return 0;
}

/*
 * Set the stream 's' to flush the stream 'flushs' before
 * blocking for reading. 's' must have been created for reading,
 * 'flushs' for writing.
 */
int prot_setflushonread(s, flushs)
struct protstream *s;
struct protstream *flushs;
{
    s->flushonread = flushs;
    return 0;
}

/*
 * Set on stream 's' the callback 'proc' and 'rock'
 * to make the next time we have to wait for input.
 */
int prot_setreadcallback(s, proc, rock)
struct protstream *s;
prot_readcallback_t *proc;
void *rock;
{
    s->readcallback_proc = proc;
    s->readcallback_rock = rock;
    return 0;
}

/*
 * Return a pointer to a statically-allocated string describing the
 * error encountered on 's'.  If there is no error condition, return a
 * null pointer.
 */
const char *prot_error(s)
struct protstream *s;
{
    return s->error;
}

/*
 * Rewind the stream 's'.  's' must have been created for reading.
 */
int 
prot_rewind(s)
struct protstream *s;
{
    if (lseek(s->fd, 0L, 0) == -1) {
	s->error = strerror(errno);
	return EOF;
    }
    s->cnt = 0;
    s->error = 0;
    s->eof = 0;
    return 0;
}

/*
 * Read data into the empty buffer for the stream 's' and return the
 * first character.  Returns EOF on EOF or error.
 */
int 
prot_fill(s)
struct protstream *s;
{
    int n;
    char *ptr;
    int left;
    int r;
    struct timeval timeout;
    fd_set rfds;
    int haveinput; 
   
    if (s->eof || s->error) return EOF;

    do {

	/* wait until get input */
	haveinput = 0;
	if (s->readcallback_proc ||
	    (s->flushonread && s->flushonread->ptr != s->flushonread->buf)) {
	    timeout.tv_sec = timeout.tv_usec = 0;
	    FD_ZERO(&rfds);
	    FD_SET(s->fd, &rfds);
	    if (select(s->fd + 1, &rfds, (fd_set *)0, (fd_set *)0,
		       &timeout) <= 0) {
		if (s->readcallback_proc) {
		    (*s->readcallback_proc)(s, s->readcallback_rock);
		    s->readcallback_proc = 0;
		    s->readcallback_rock = 0;
		}
		if (s->flushonread) prot_flush(s->flushonread);
	    }
	    else {
		haveinput = 1;
	    }
	}

	if (!haveinput && (s->read_timeout || s->dontblock)) {
	    timeout.tv_sec = s->read_timeout;
	    timeout.tv_usec = 0;
	    FD_ZERO(&rfds);
	    FD_SET(s->fd, &rfds);
	    do {
		r = select(s->fd + 1, &rfds, (fd_set *)0, (fd_set *)0,
			   &timeout);
	    } while (r == -1 && errno == EINTR);
	    if (r == 0) {
		if (!s->dontblock) {
		    s->error = "idle for too long";
		    return EOF;
		} else {
		    errno = EAGAIN;
		    return EOF;
		}
	    }
	}
	
	do {
#ifdef HAVE_SSL	  

	  /* just do a SSL read instead if we're under a tls layer */
	  if (s->tls_conn!=NULL)
	  {
	    n = SSL_read(s->tls_conn, s->buf, PROT_BUFSIZE);
	  } else {
	    n = read(s->fd, s->buf, PROT_BUFSIZE);
	  }

#else  /* HAVE_SSL */
	  n = read(s->fd, s->buf, PROT_BUFSIZE);
#endif /* HAVE_SSL */


	    
	} while (n == -1 && errno == EINTR);
	
	
	if (n <= 0) {
	    if (n) s->error = strerror(errno);
	    else s->eof = 1;
	    return EOF;
	}
	
	if (s->saslssf) { /* decode it */
	    int result;
	    char *out;
	    unsigned outlen;
	    
	    /* Decode the input token */
	    result = sasl_decode(s->conn, (const char *) s->buf, n, 
				 &out, &outlen);
	    
	    if (result != SASL_OK) {
		snprintf(s->buf, 200, "Decoding error: %s (%i)",
			 sasl_errstring(result, NULL, NULL), result);
		s->error = s->buf;
		return EOF;
	    }
	    
	    if (outlen > 0) {
		if (outlen > s->buf_size) {
		    s->buf = (char *) xrealloc(s->buf, 
					       sizeof(char) * (outlen + 4));
		    s->buf_size = outlen;
		}
		memcpy(s->buf, out, outlen);
		s->ptr = s->buf + 1;
		s->cnt = outlen;
		free(out);
	    } else {		/* didn't decode anything */
		s->cnt = 0;
	    }
	    
	} else {
	    /* No protection function, just use the raw data */
	    s->ptr = s->buf+1;
	    s->cnt = n;
	}
	
	  

	if (s->cnt > 0) {
	    if (s->logfd != -1) {
		time_t newtime;
		char timebuf[20];

		if (s->log_timeptr) {
		    time(&newtime);
		    sprintf(timebuf, "<%ld<", newtime - *s->log_timeptr);
		    write(s->logfd, timebuf, strlen(timebuf));
		    *s->log_timeptr = newtime;
		}

		left = s->cnt+1;
		ptr = s->buf;
		do {
		    n = write(s->logfd, ptr, left);
		    if (n == -1 && errno != EINTR) {
			break;
		    }
		    if (n > 0) {
			ptr += n;
			left -= n;
		    }
		} while (left);
	    }
	    s->cnt--;		/* we return the first char */
	    return *s->buf;
	}

    } while (1);

}

/*
 * Write out any buffered data in the stream 's'
 */
int prot_flush(s)
struct protstream *s;
{
    char *ptr = s->buf;
    int left = s->ptr - s->buf;
    int n;
    char *foo;

    if (s->eof || s->error) {
	s->ptr = s->buf;
	s->cnt = 1;
	return EOF;
    }
    if (!left) return 0;

    if (s->logfd != -1) {
	time_t newtime;
	char timebuf[20];

	if (s->log_timeptr) {
	    time(&newtime);
	    sprintf(timebuf, ">%ld>", newtime - *s->log_timeptr);
	    write(s->logfd, timebuf, strlen(timebuf));
	}

	do {
	    n = write(s->logfd, ptr, left);
	    if (n == -1 && errno != EINTR) {
		break;
	    }
	    if (n > 0) {
		ptr += n;
		left -= n;
	    }
	} while (left);
	left = s->ptr - s->buf;
	ptr = s->buf;
    }



    if (s->saslssf!=0) {
      /* Encode the data */  /* xxx handle left */
      unsigned int outlen;
      int result;

      result=sasl_encode(s->conn, ptr, left, &foo, &outlen);
      if (result!=SASL_OK)
      {
	s->error = "Encoding error";
	if (s->log_timeptr) time(s->log_timeptr);
	return EOF;
      }

      ptr=foo;
      left=outlen;
    }

    /* Write out the data */
    do {
#ifdef HAVE_SSL
      if (s->tls_conn!=NULL)
      {
        n = SSL_write(s->tls_conn, ptr, left);
      } else {
	n = write(s->fd, ptr, left);
      }
#else  /* HAVE_SSL */
	n = write(s->fd, ptr, left);
#endif /* HAVE_SSL */
	if (n == -1 && errno != EINTR) {
	    s->error = strerror(errno);
	    if (s->log_timeptr) time(s->log_timeptr);
	    return EOF;
	}

	if (n > 0) {
	    ptr += n;
	    left -= n;
	}
    } while (left);

    /* sasl_encode did a malloc so we need to free for it */
    if (s->saslssf!=0) { 
      free(foo);
    }

    /* Reset the output buffer */
    s->ptr = s->buf;
    s->cnt = s->maxplain;

    if (s->log_timeptr) time(s->log_timeptr);
    return 0;
}

/*
 * Write to the output stream 's' the 'len' bytes of data at 'buf'
 */
int prot_write(s, buf, len)
struct protstream *s;
const char *buf;
unsigned len;
{
    while (len >= s->cnt) {
	memcpy(s->ptr, buf, s->cnt);
	s->ptr += s->cnt;
	buf += s->cnt;
	len -= s->cnt;
	s->cnt = 0;
	if (prot_flush(s) == EOF) return EOF;
    }
    memcpy(s->ptr, buf, len);
    s->ptr += len;
    s->cnt -= len;
    if (s->error || s->eof) return EOF;
    return 0;
}

/*
 * Stripped-down version of printf() that works on protection streams
 * Only understands '%d', '%s', '%c', and '%%' in the format string.
 */
#ifdef __STDC__
int prot_printf(struct protstream *s, const char *fmt, ...)
#else
int prot_printf(va_alist)
va_dcl
#endif
{
    va_list pvar;
    char *percent, *p;
    int i;
    unsigned u;
    char buf[30];
#ifdef __STDC__
    va_start(pvar, fmt);
#else
    struct protstream *s;
    char *fmt;

    va_start(pvar);
    s = va_arg(pvar, struct protstream *);
    fmt = va_arg(pvar, char *);
#endif

    while ((percent = strchr(fmt, '%')) != 0) {
	prot_write(s, fmt, percent-fmt);
	switch (*++percent) {
	case '%':
	    prot_putc('%', s);
	    break;

	case 'd':
	    i = va_arg(pvar, int);
	    sprintf(buf, "%d", i);
	    prot_write(s, buf, strlen(buf));
	    break;

	case 'u':
	    u = va_arg(pvar, int);
	    sprintf(buf, "%u", u);
	    prot_write(s, buf, strlen(buf));
	    break;

	case 's':
	    p = va_arg(pvar, char *);
	    prot_write(s, p, strlen(p));
	    break;

	case 'c':
	    i = va_arg(pvar, int);
	    prot_putc(i, s);
	    break;

	default:
	    abort();
	}
	fmt = percent+1;
    }
    prot_write(s, fmt, strlen(fmt));
    va_end(pvar);
    if (s->error || s->eof) return EOF;
    return 0;
}

/*
 * Read from the protections stream 's' up to 'size' bytes into the buffer
 * 'buf'.  Returns the number of bytes read, or 0 for some error.
 */
int
prot_read(s, buf, size)
struct protstream *s;
char *buf;
unsigned size;
{
    int c;

    if (!size) return 0;

    if (s->cnt) {
	/* Some data in the input buffer, return that */
	if (size > s->cnt) size = s->cnt;
	memcpy(buf, s->ptr, size);
	s->ptr += size;
	s->cnt -= size;
	return size;
    }

    c = prot_fill(s);
    if (c == EOF) return 0;
    buf[0] = c;
    if (--size > s->cnt) size = s->cnt;
    memcpy(buf+1, s->ptr, size);
    s->ptr += size;
    s->cnt -= size;
    return size+1;
}

/*
 * Version of fgets() that works with protection streams.
 */
char *
prot_fgets(buf, size, s)
char *buf;
unsigned size;
struct protstream *s;
{
    char *p = buf;
    int c;

    if (size < 2) return 0;
    size -= 2;

    while (size && (c = prot_getc(s)) != EOF) {
	size--;
	*p++ = c;
	if (c == '\n') break;
    }
    if (p == buf) return 0;
    *p++ = '\0';
    return buf;
}
