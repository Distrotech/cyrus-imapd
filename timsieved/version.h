/* version.h: the version number
 *
 * $Id: version.h,v 1.1.2.1 1999/10/13 19:29:56 leg Exp $
 */

#define _SIEVED_VERSION "v1.0.0"

#ifdef EXTRA_IDENT
#define SIEVED_VERSION _SIEVED_VERSION "-" EXTRA_IDENT
#else
#define SIEVED_VERSION _SIEVED_VERSION
#endif

#define SIEVED_IDENT "Cyrus timsieved"
