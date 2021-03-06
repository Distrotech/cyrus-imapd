/* cyr_expire.c -- Program to expire deliver.db entries and messages
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: cyr_expire.c,v 1.25 2010/01/06 17:01:31 murch Exp $
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>

#include <sasl/sasl.h>

#include "annotate.h"
#include "cyrusdb.h"
#include "duplicate.h"
#include "exitcodes.h"
#include "global.h"
#include "hash.h"
#include "libcyr_cfg.h"
#include "mboxlist.h"
#include "util.h"
#include "xmalloc.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"

/* global state */
const int config_need_data = 0;
static int sigquit = 0;

void usage(void)
{
    fprintf(stderr,
	    "cyr_expire [-C <altconfig>] -E <days> [-X <expunge-days>] [-p prefix] [-a] [-v]\n");
    exit(-1);
}

struct expire_rock {
    struct hash_table *table;
    time_t expire_mark;
    time_t expunge_mark;
    unsigned long mailboxes;
    unsigned long messages;
    unsigned long deleted;
    int verbose;
    int skip_annotate;
};

struct delete_node {
    struct delete_node *next;
    char *name;
};

struct delete_rock {
    char prefix[100];
    int prefixlen;
    time_t delete_mark;
    struct delete_node *head;
    struct delete_node *tail;
    int verbose;
};

/*
 * Parse a non-negative duration string as seconds.
 *
 * Convert "23.5m" to fractional days.  Accepts the suffixes "d" (day),
 * (day), "h" (hour), "m" (minute) and "s" (second).  If no suffix, assume
 * days.
 * Returns 1 if successful and *secondsp is filled in, or 0 if the suffix
 * is unknown or on error.
 */
static int parse_duration(const char *s, int *secondsp)
{
    char *end = NULL;
    double val;
    int multiplier = 86400; /* default is days */

    /* no negative or empty numbers please */
    if (!*s || *s == '-')
        return 0;

    val = strtod(s, &end);
    /* Allow 'd', 'h', 'm' and 's' as end, else return error. */
    if (*end) {
	if (end[1]) return 0; /* trailing extra junk */

	switch (*end) {
	case 'd':
	    /* already the default */
	    break;
	case 'h':
	    multiplier = 3600;
	    break;
	case 'm':
	    multiplier = 60;
	    break;
	case 's':
	    multiplier = 1;
	    break;
	default:
	    return 0;
	}
    }

    *secondsp = multiplier * val;

    return 1;
}

/*
 * mailbox_expunge() callback to expunge expired articles.
 */
static unsigned expire_cb(struct mailbox *mailbox __attribute__((unused)), 
			  struct index_record *record, 
			  void *rock)
{
    struct expire_rock *erock = (struct expire_rock *) rock;

    erock->messages++;

    /* otherwise, we're expiring messages by sent date */
    if (record->gmtime < erock->expire_mark) {
	erock->deleted++;
	return 1;
    }

    return 0;
}


/*
 * mboxlist_findall() callback function to:
 * - expire messages from mailboxes,
 * - build a hash table of mailboxes in which we expired messages,
 * - and perform a cleanup of expunged messages
 */
int expire(char *name, int matchlen, int maycreate __attribute__((unused)),
	   void *rock)
{
    struct mboxlist_entry mbentry;
    struct expire_rock *erock = (struct expire_rock *) rock;
    char buf[MAX_MAILBOX_BUFFER] = "", *p;
    struct annotation_data attrib;
    int r, domainlen = 0;
    struct mailbox *mailbox = NULL;
    unsigned numdeleted = 0;

    if (sigquit) {
	return 1;
    }
    /* Skip remote mailboxes */
    r = mboxlist_lookup(name, &mbentry, NULL);
    if (r) {
	if (erock->verbose) {
	    printf("error looking up %s: %s\n", name, error_message(r));
	}
	return 1;
    }
    if (mbentry.mbtype & MBTYPE_REMOTE) return 0;

    if (config_virtdomains && (p = strchr(name, '!')))
	domainlen = p - name + 1;

    strncpy(buf, name, matchlen);
    buf[matchlen] = '\0';

    /* see if we need to expire messages.
     * since mailboxes inherit /vendor/cmu/cyrus-imapd/expire,
     * we need to iterate all the way up to "" (server entry)
     */
    if (erock->skip_annotate) {
      /* we don't want to check for annotations, so we didn't find any */
      attrib.value = 0;
    }
    else {
        while (1) {
	    r = annotatemore_lookup(buf, "/vendor/cmu/cyrus-imapd/expire", "",
				    &attrib);
    
	    if (r ||				/* error */
	        attrib.value ||			/* found an entry */
	        !buf[0] ||				/* done recursing */
	        !strcmp(buf+domainlen, "user")) {	/* server entry doesn't apply
						       to personal mailboxes */
	        break;
	    }
    
	    p = strrchr(buf, '.');			/* find parent mailbox */
    
	    if (p && (p - buf > domainlen))		/* don't split subdomain */
	        *p = '\0';
	    else if (!buf[domainlen])		/* server entry */
	        buf[0] = '\0';
	    else					/* domain entry */
	        buf[domainlen] = '\0';
        }
    }

    r = mailbox_open_iwl(name, &mailbox);
    if (r) {
	/* mailbox corrupt/nonexistent -- skip it */
	syslog(LOG_WARNING, "unable to open mailbox %s", name);
	return 0;
    }

    if (attrib.value) {
	/* XXX - stats are bound to be bogus... */
	erock->mailboxes++;
	erock->expire_mark = 0;

	if (attrib.value) {
	    /* add mailbox to table */
	    unsigned long expire_days = strtoul(attrib.value, NULL, 10);
	    time_t *expire_mark = (time_t *) xmalloc(sizeof(time_t));

	    *expire_mark = expire_days ?
		time(0) - (expire_days * 60 * 60 * 24) : 0 /* never */ ;
	    hash_insert(name, (void *) expire_mark, erock->table);

	    if (erock->verbose) {
		fprintf(stderr,
			"expiring messages in %s older than %ld days\n",
			name, expire_days);
	    }

	    erock->expire_mark = *expire_mark;
	}

	r = mailbox_expunge(mailbox, expire_cb, erock, NULL);
	if (r) {
	    syslog(LOG_ERR, "failed to expire old messages: %s", mailbox->name);
	}
    }

    r = mailbox_expunge_cleanup(mailbox, erock->expunge_mark, &numdeleted);

    erock->deleted += numdeleted;
    erock->mailboxes++;
    erock->messages += mailbox->i.num_records;

    mailbox_close(&mailbox);

    if (r) {
	syslog(LOG_WARNING, "failure expiring %s: %s", name, error_message(r));
    }

    /* Even if we had a problem with one mailbox, continue with the others */
    return 0;
}

int delete(char *name,
	   int matchlen __attribute__((unused)),
	   int maycreate __attribute__((unused)),
	   void *rock)
{
    struct mboxlist_entry mbentry;
    struct delete_rock *drock = (struct delete_rock *) rock;
    char *p;
    int i, r, domainlen = 0;
    struct delete_node *node;
    time_t timestamp;

    if (sigquit) {
	return 1;
    }
    if (config_virtdomains && (p = strchr(name, '!')))
	domainlen = p - name + 1;

    /* check if this is a mailbox we want to examine */
    if (strncmp(name+domainlen, drock->prefix, drock->prefixlen))
	return 0;

    /* Skip remote mailboxes */
    r = mboxlist_lookup(name, &mbentry, NULL);
    if (r) {
	if (drock->verbose) {
	    printf("error looking up %s: %s\n", name, error_message(r));
	}
	return 1;
    }
    if (mbentry.mbtype & MBTYPE_REMOTE) return 0;

    /* Sanity check for 8 hex digits only at the end */
    p = strrchr(name, '.');
    if (!p) return(0);
    p++;

    for (i = 0 ; i < 7; i++) {
        if (!Uisxdigit(p[i])) return(0);
    }
    if (p[8] != '\0') return(0);

    timestamp = strtoul(p, NULL, 16);
    if ((timestamp == 0) || (timestamp > drock->delete_mark))
        return(0);

    /* Add this mailbox to list of mailboxes to delete */
    node = xmalloc(sizeof(struct delete_node));
    node->next = NULL;
    node->name = xstrdup(name);

    if (drock->tail) {
	drock->tail->next = node;
	drock->tail = node;
    } else {
	drock->head = drock->tail = node;
    }
    return(0);
}
static void sighandler (int sig __attribute((unused)))
{
    sigquit = 1;
    return;
}

int main(int argc, char *argv[])
{
    extern char *optarg;
    int opt, r = 0, do_expunge = 1;
    int expunge_seconds = -1;
    int delete_seconds = -1;
    int expire_seconds = 0;
    char *alt_config = NULL;
    char *find_prefix = NULL;
    char buf[100];
    struct hash_table expire_table;
    struct expire_rock erock;
    struct delete_rock drock;
    const char *deletedprefix;
    struct sigaction action;

    if ((geteuid()) == 0 && (become_cyrus() != 0)) {
	fatal("must run as the Cyrus user", EC_USAGE);
    }

    /* zero the expire_rock & delete_rock */
    memset(&erock, 0, sizeof(erock));
    memset(&drock, 0, sizeof(drock));

    while ((opt = getopt(argc, argv, "C:D:E:X:p:vax")) != EOF) {
	switch (opt) {
	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	case 'D':
	    if (delete_seconds >= 0) usage();
	    if (!parse_duration(optarg, &delete_seconds)) usage();
	    break;

	case 'E':
	    if (expire_seconds > 0) usage();
	    if (!parse_duration(optarg, &expire_seconds)) usage();
	    break;

	case 'X':
	    if (expunge_seconds >= 0) usage();
	    if (!parse_duration(optarg, &expunge_seconds)) usage();
	    break;

	case 'x':
	    if (!do_expunge) usage();
	    do_expunge = 0;
	    break;

	case 'p':
	    find_prefix = optarg;
	    break;

	case 'v':
	    erock.verbose++;
	    drock.verbose++;
	    break;

	case 'a':
	    erock.skip_annotate = 1;
	    break;

	default:
	    usage();
	    break;
	}
    }

    if (!expire_seconds) usage();

    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = sighandler;
    if (sigaction(SIGQUIT, &action, NULL) < 0) {
        fatal("unable to install signal handler for %d: %m", SIGQUIT);
    }

    cyrus_init(alt_config, "cyr_expire", 0);
    global_sasl_init(1, 0, NULL);

    annotatemore_init(0, NULL, NULL);
    annotatemore_open(NULL);

    mboxlist_init(0);
    mboxlist_open(NULL);

    /* open the quota db, we'll need it for expunge */
    quotadb_init(0);
    quotadb_open(NULL);

    if (duplicate_init(NULL, 0) != 0) {
	fprintf(stderr, 
		"cyr_expire: unable to init duplicate delivery database\n");
	exit(1);
    }

    /* xxx better way to determine a size for this table? */
    construct_hash_table(&expire_table, 10000, 1);

    if (do_expunge) {

	/* expire messages from mailboxes,
	 * build a hash table of mailboxes in which we expired messages,
	 * so we can prune those immediately from the duplicate db.
	 */
	erock.table = &expire_table;
	if (expunge_seconds < 0) {
	    erock.expunge_mark = 0;
	} else {
	    erock.expunge_mark = time(0) - expunge_seconds;

	    if (erock.verbose) {
		fprintf(stderr,
			"Expunging deleted messages in mailboxes older than %0.2f days\n",
			((double)expunge_seconds/86400));
	    }
	}

	if (find_prefix) {
	    strlcpy(buf, find_prefix, sizeof(buf));
	} else {
	    strlcpy(buf, "*", sizeof(buf));
	}

	mboxlist_findall(NULL, buf, 1, 0, 0, &expire, &erock);

	syslog(LOG_NOTICE, "Expunged %lu out of %lu messages from %lu mailboxes",
	       erock.deleted, erock.messages, erock.mailboxes);
	if (erock.verbose) {
	    fprintf(stderr, "\nExpunged %lu out of %lu messages from %lu mailboxes\n",
		    erock.deleted, erock.messages, erock.mailboxes);
	}
    }
    if (sigquit) {
	goto finish;
    }

    if ((delete_seconds >= 0) && mboxlist_delayed_delete_isenabled() &&
	(deletedprefix = config_getstring(IMAPOPT_DELETEDPREFIX))) {
        struct delete_node *node;
        int count = 0;

        if (drock.verbose) {
            fprintf(stderr,
		    "Removing deleted mailboxes older than %0.2f days\n",
		    ((double)delete_seconds/86400));
        }

        strlcpy(drock.prefix, deletedprefix, sizeof(drock.prefix));
        strlcat(drock.prefix, ".", sizeof(drock.prefix));
        drock.prefixlen = strlen(drock.prefix);
        drock.delete_mark = time(0) - delete_seconds;
        drock.head = NULL;
        drock.tail = NULL;

        mboxlist_findall(NULL, buf, 1, 0, 0, &delete, &drock);

        for (node = drock.head ; node ; node = node->next) {
	    if (sigquit) {
		goto finish;
	    }
            if (drock.verbose) {
                fprintf(stderr, "Removing: %s\n", node->name);
            }
            r = mboxlist_deletemailbox(node->name, 1, NULL, NULL, 0, 0, 0);
            count++;
        }

        if (drock.verbose) {
            if (count != 1) {
                fprintf(stderr, "Removed %d deleted mailboxes\n", count);
            } else {
                fprintf(stderr, "Removed 1 deleted mailbox\n");
            }
        }
        syslog(LOG_NOTICE, "Removed %d deleted mailboxes", count);
    }
    if (sigquit) {
	goto finish;
    }

    /* purge deliver.db entries of expired messages */
    r = duplicate_prune(expire_seconds, &expire_table);

finish:
    free_hash_table(&expire_table, free);

    quotadb_close();
    quotadb_done();
    mboxlist_close();
    mboxlist_done();
    annotatemore_close();
    annotatemore_done();
    duplicate_done();
    sasl_done();
    cyrus_done();

    exit(r);
}
