/**
 * Prefix opclass allows to efficiently index a prefix table with
 * GiST.
 *
 * More common use case is telephony prefix searching for cost or
 * routing.
 *
 * Many thanks to AndrewSN, who provided great amount of help in the
 * writing of this opclass, on the PostgreSQL internals, GiST inner
 * working and prefix search analyses.
 */

#include <stdio.h>
#include "postgres.h"

#include "access/gist.h"
#include "access/skey.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/builtins.h"
#include "libpq/pqformat.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
#include <math.h>

/**
 * We use those DEBUG defines in the code, uncomment them to get very
 * verbose output.
 *
#define  DEBUG
#define  DEBUG_UNION
#define  DEBUG_INTER
#define  DEBUG_PENALTY
#define  DEBUG_PICKSPLIT
#define  DEBUG_CONSISTENT
#define  DEBUG_PRESORT_GP
#define  DEBUG_PRESORT_MAX
#define  DEBUG_PRESORT_UNIONS
#define  DEBUG_PRESORT_RESULT

#define  DEBUG_PR_IN
#define  DEBUG_PR_NORMALIZE
#define  DEBUG_MAKE_VARLENA
*/

PG_MODULE_MAGIC;

/**
 * prefix_range datatype, varlena structure
 */
typedef struct {
  char first;
  char last;
  char prefix[1]; /* this is a varlena structure, data follows */
} prefix_range;

enum pr_delimiters_t {
  PR_OPEN   = '[',
  PR_CLOSE  = ']',
  PR_SEP    = '-'
} pr_delimiters;

/**
 * prefix_range input/output functions and operators
 */
Datum prefix_range_init(PG_FUNCTION_ARGS);
Datum prefix_range_in(PG_FUNCTION_ARGS);
Datum prefix_range_out(PG_FUNCTION_ARGS);
Datum prefix_range_recv(PG_FUNCTION_ARGS);
Datum prefix_range_send(PG_FUNCTION_ARGS);
Datum prefix_range_cast_to_text(PG_FUNCTION_ARGS);
Datum prefix_range_cast_from_text(PG_FUNCTION_ARGS);

Datum prefix_range_length(PG_FUNCTION_ARGS);
Datum prefix_range_eq(PG_FUNCTION_ARGS);
Datum prefix_range_neq(PG_FUNCTION_ARGS);
Datum prefix_range_lt(PG_FUNCTION_ARGS);
Datum prefix_range_le(PG_FUNCTION_ARGS);
Datum prefix_range_gt(PG_FUNCTION_ARGS);
Datum prefix_range_ge(PG_FUNCTION_ARGS);
Datum prefix_range_cmp(PG_FUNCTION_ARGS);

Datum prefix_range_overlaps(PG_FUNCTION_ARGS);
Datum prefix_range_contains(PG_FUNCTION_ARGS);
Datum prefix_range_contains_strict(PG_FUNCTION_ARGS);
Datum prefix_range_contained_by(PG_FUNCTION_ARGS);
Datum prefix_range_contained_by_strict(PG_FUNCTION_ARGS);
Datum prefix_range_union(PG_FUNCTION_ARGS);
Datum prefix_range_inter(PG_FUNCTION_ARGS);

#define DatumGetPrefixRange(X)	          ((prefix_range *) VARDATA_ANY(X) )
#define PrefixRangeGetDatum(X)	          PointerGetDatum(make_varlena(X))
#define PG_GETARG_PREFIX_RANGE_P(n)	  DatumGetPrefixRange(PG_DETOAST_DATUM(PG_GETARG_DATUM(n)))
#define PG_RETURN_PREFIX_RANGE_P(x)	  return PrefixRangeGetDatum(x)

/**
 * Used by prefix_contains_internal and pr_contains_prefix.
 *
 * plen is the length of string p, qlen the length of string q, the
 * caller are dealing with either text * or char * and its their
 * responsabolity to use either strlen() or VARSIZE_ANY_EXHDR()
 */
static inline
bool __prefix_contains(char *p, char *q, int plen, int qlen) {
  if(qlen < plen )
    return false;

  return memcmp(p, q, plen) == 0;
}

static inline
char *__greater_prefix(char *a, char *b, int alen, int blen)
{
  int i = 0;
  char *result = NULL;

  for(i=0; i<alen && i<blen && a[i] == b[i]; i++);

  /* i is the last common char position in a, or 0 */
  if( i == 0 ) {
    /**
     * return ""
     */
    result = (char *)palloc(sizeof(char));
  }
  else {
    result = (char *)palloc((i+1) * sizeof(char));
    memcpy(result, a, i);
  }
  result[i] = 0;

  return result;
}

/**
 * Helper function which builds a prefix_range from a prefix, a first
 * and a last component, making a copy of the prefix string.
 */
static inline
prefix_range *build_pr(const char *prefix, char first, char last) {
  int s = strlen(prefix) + 1;
  prefix_range *pr = palloc(sizeof(prefix_range) + s);
  memcpy(pr->prefix, prefix, s);
  pr->first = first;
  pr->last  = last;

#ifdef DEBUG_PR_IN
  elog(NOTICE,
       "build_pr: pr->prefix = '%s', pr->first = %d, pr->last = %d",
       pr->prefix, pr->first, pr->last);
#endif

  return pr;
}

/**
 * Normalize a prefix_range. Two cases are handled:
 *
 *  abc[x-x] is rewritten abcx
 *  abc[x-y] is rewritten abc[y-x] when y < x
 */
static inline
prefix_range *pr_normalize(prefix_range *a) {
  char tmpswap;
  char *prefix;

  prefix_range *pr = build_pr(a->prefix, a->first, a->last);

  if( pr->first == pr->last ) {
    int s = strlen(pr->prefix)+2;
    prefix = (char *)palloc(s);
    memcpy(prefix, pr->prefix, s-2);
    prefix[s-2] = pr->first;
    prefix[s-1] = 0;

#ifdef DEBUG_PR_NORMALIZE
    elog(NOTICE, "prefix_range %s %s %s", a->prefix, pr->prefix, prefix);
#endif

    pfree(pr);
    pr = build_pr(prefix, 0, 0);
  }
  else if( pr->first > pr->last ) {
    tmpswap   = pr->first;
    pr->first = pr->last;
    pr->last  = tmpswap;
  }
  return pr;
}

/*
 * Init a prefix value from the prefix_range(text, text, text)
 * function
 */
static inline
prefix_range *make_prefix_range(char *str, char first, char last) {
  int len;
  prefix_range *pr = NULL;

  if( str != NULL )
    pr  = build_pr(str, first, last);

  else
    pr = build_pr("", first, last);
  if( str != NULL ){
  len = strlen(pr->prefix);
  memcpy(pr->prefix, str, len);
  pr->prefix[len] = 0;
  } else{
  pr->prefix[0] = 0;
  }
  return pr_normalize(pr);
}

/**
 * First, the input reader. A prefix range will have to respect the
 * following regular expression: .*([[].-.[]])?
 *
 * examples : 123[4-6], [1-3], 234, 01[] --- last one not covered by
 * regexp.
 */

static inline
prefix_range *pr_from_str(char *str) {
  prefix_range *pr = NULL;
  char *prefix = (char *)palloc(strlen(str)+1);
  char current = 0, previous = 0;
  bool opened = false;
  bool closed = false;
  bool sawsep = false;
  char *ptr, *prefix_ptr = prefix;

  bzero(prefix, strlen(str)+1);

  for(ptr=str; *ptr != 0; ptr++) {
    previous = current;
    current = *ptr;

    if( !opened && current != PR_OPEN )
      *prefix_ptr++ = current;

#ifdef DEBUG_PR_IN
    elog(NOTICE, "prefix_range previous='%c' current='%c' prefix='%s'",
	 (previous?previous:' '), current, prefix);
#endif

    switch( current ) {

    case PR_OPEN:
      if( opened ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains several %c", str, PR_OPEN);
#endif
	return NULL;
      }
      opened = true;

      pr = build_pr(prefix, 0, 0);
      break;

    case PR_SEP:
      if( opened ) {
	if( closed ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s contains trailing character", str);
#endif
	  return NULL;
	}
	sawsep = true;

	if( previous == PR_OPEN ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has separator following range opening, without data", str);
#endif
	  return NULL;
	}

	pr->first = previous;
      }
      break;

    case PR_CLOSE:
      if( !opened ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s closes a range which is not opened ", str);
#endif
	return NULL;
      }

      if( closed ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains several %c", str, PR_CLOSE);
#endif
	return NULL;
      }
      closed = true;

      if( sawsep ) {
	if( previous == PR_SEP ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has a closed range without last bound", str);
#endif
	  return NULL;
	}
	pr->last = previous;
      }
      else {
	if( previous != PR_OPEN ) {
#ifdef DEBUG_PR_IN
	  elog(ERROR,
	       "prefix_range %s has a closing range without separator", str);
#endif
	  return NULL;
	}
      }
      break;

    default:
      if( closed ) {
#ifdef DEBUG_PR_IN
	elog(ERROR,
	     "prefix_range %s contains trailing characters", str);
#endif
	return NULL;
      }
      break;
    }
  }

  if( ! opened ) {
    pr = build_pr(prefix, 0, 0);
  }

  if( opened && !closed ) {
#ifdef DEBUG_PR_IN
    elog(ERROR, "prefix_range %s opens a range but does not close it", str);
#endif
    return NULL;
  }

  pr = pr_normalize(pr);

#ifdef DEBUG_PR_IN
  if( pr != NULL ) {
    if( pr->first && pr->last )
      elog(NOTICE,
	   "prefix_range %s: prefix = '%s', first = '%c', last = '%c'",
	   str, pr->prefix, pr->first, pr->last);
    else
      elog(NOTICE,
	   "prefix_range %s: prefix = '%s', no first nor last",
	   str, pr->prefix);
  }
#endif

  return pr;
}

static inline
struct varlena *make_varlena(prefix_range *pr) {
  struct varlena *vdat;
  int size;

  if (pr != NULL) {
    size = sizeof(prefix_range) + ((strlen(pr->prefix)+1)*sizeof(char)) + VARHDRSZ;
    vdat = palloc(size);
    SET_VARSIZE(vdat, size);
    memcpy(VARDATA(vdat), pr, (size - VARHDRSZ));

#ifdef DEBUG_MAKE_VARLENA
    elog(NOTICE, "make varlena: size=%d varsize=%d compressed=%c external=%c %s[%c-%c] %s[%c-%c]",
	 size,
	 VARSIZE_ANY_EXHDR(vdat),
	 (VARATT_IS_COMPRESSED(vdat) ? 't' : 'f'),
	 (VARATT_IS_EXTERNAL(vdat)   ? 't' : 'f'),
	 pr->prefix,
	 (pr->first != 0 ? pr->first : ' '),
	 (pr->last  != 0 ? pr->last  : ' '),
	 ((prefix_range *)VARDATA(vdat))->prefix,
	 (((prefix_range *)VARDATA(vdat))->first ? ((prefix_range *)VARDATA(vdat))->first : ' '),
	 (((prefix_range *)VARDATA(vdat))->last  ? ((prefix_range *)VARDATA(vdat))->last  : ' '));
#endif

    return vdat;
  }
  return NULL;
}

/*
 * Allow users to use length(prefix) rather than length(prefix::text), and
 * while at it, provides an implementation which won't count the displaying
 * artifacts that are the [] and -.
 */
static inline
int pr_length(prefix_range *pr) {
  int len = strlen(pr->prefix);

  if( pr->first != 0 || pr->last != 0 )
    len += 1;

  return len;
}

static inline
bool pr_eq(prefix_range *a, prefix_range *b) {
  int sa = strlen(a->prefix);
  int sb = strlen(b->prefix);

  return sa == sb
    && memcmp(a->prefix, b->prefix, sa) == 0
    && a->first == b->first
    && a->last  == b->last;
}

/*
 * We invent a prefix_range ordering for convenience, but that's
 * dangerous. Use the BTree opclass at your own risk.
 *
 * On the other hand, when your routing table does contain pretty static
 * data and you test it carefully or know it will fit into the ordering
 * simplification, you're good to go.
 *
 * Baring bug, the constraint is to have non-overlapping data.
 */

/*static inline
bool pr_lt(prefix_range *a, prefix_range *b, bool eqval) {
*/

static inline
int pr_cmp(prefix_range *a, prefix_range *b) {
  int cmp = 0;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  int mlen = alen; /* minimum length */
  char *p  = a->prefix;
  char *q  = b->prefix;

  /*
   * First case, common prefix length
   */
  if( alen == blen ) {
    cmp = memcmp(p, q, alen);

    /* Uncommon prefix, easy to compare */
    if( cmp != 0 )
      return cmp;

    /* Common prefix, check for (sub)ranges */
    else
      return (a->first == b->first) ? (a->last - b->last) : (a->first - b->first);
  }

  /* For memcmp() safety, we need the minimum length */
  if( mlen > blen )
    mlen = blen;

  /*
   * Don't forget we may have [x-y] prefix style, that's empty prefix, only range.
   */
  if( alen == 0 && a->first != 0 ) {
    /* return (eqval ? (a->first <= q[0]) : (a->first < q[0])); */
    return a->first - q[0];
  }
  else if( blen == 0 && b->first != 0 ) {
    /* return (eqval ? (p[0] <= b->first) : (p[0] < b->first)); */
    return p[0] - b->first;
  }

  /*
   * General case
   *
   * When memcmp() on the shorter of p and q returns 0, that means they
   * share a common prefix: avoid to say that '93' < '9377' and '9377' <
   * '93'.
   */
  cmp = memcmp(p, q, mlen);

  if( cmp == 0 )
    /*
     * we are comparing e.g. '1' and '12' (the shorter contains the
     * smaller), so let's pretend '12' < '1' as it contains less elements.
     */
    return (alen == mlen) ? 1 : -1;

  return cmp;
}

static inline
bool pr_lt(prefix_range *a, prefix_range *b, bool eqval) {
  int cmp = pr_cmp(a, b);
  return eqval ? cmp <= 0 : cmp < 0;
}

static inline
bool pr_gt(prefix_range *a, prefix_range *b, bool eqval) {
  int cmp = pr_cmp(a, b);
  return eqval ? cmp >= 0 : cmp > 0;
}

static inline
bool pr_contains(prefix_range *left, prefix_range *right, bool eqval) {
  int sl;
  int sr;
  bool left_prefixes_right;

  if( pr_eq(left, right) )
    return eqval;

  sl = strlen(left->prefix);
  sr = strlen(right->prefix);

  if( sr < sl )
    return false;

  left_prefixes_right = memcmp(left->prefix, right->prefix, sl) == 0;

  if( left_prefixes_right ) {
    if( sl == sr )
      return left->first == 0 ||
	(left->first <= right->first && left->last >= right->last);

    return left->first == 0 ||
      (left->first <= right->prefix[sl] && right->prefix[sl] <= left->last);
  }
  return false;
}

/**
 * does a given prefix_range includes a given prefix?
 */
static inline
bool pr_contains_prefix(prefix_range *pr, text *query, bool eqval) {
  int plen = strlen(pr->prefix);
  int qlen = VARSIZE_ANY_EXHDR(query);
  char *p  = pr->prefix;
  char *q  = (char *)VARDATA_ANY(query);

  if( __prefix_contains(p, q, plen, qlen) ) {
    if( pr->first == 0 || qlen == plen ) {
      return eqval;
    }

    /**
     * __prefix_contains() is true means qlen >= plen, and previous
     * test ensures qlen != plen, we hence assume qlen > plen.
     */
    Assert(qlen > plen);
    return pr-> first <= q[plen] && q[plen] <= pr->last;
  }
  return false;
}

static
prefix_range *pr_union(prefix_range *a, prefix_range *b) {
  prefix_range *res = NULL;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  char *gp = NULL;
  int gplen;
  char min, max;

  if( 0 == alen && 0 == blen ) {
    res = build_pr("",
		   a->first <= b->first ? a->first : b->first,
		   a->last  >= b->last  ? a->last : b->last);
    return pr_normalize(res);
  }

  gp = __greater_prefix(a->prefix, b->prefix, alen, blen);
  gplen = strlen(gp);

  if( gplen == 0 ) {
    res = build_pr("", 0, 0);
    if( alen > 0 && blen > 0 ) {
      res->first = a->prefix[0];
      res->last  = b->prefix[0];
    }
    else if( alen == 0 ) {
      res->first = a->first <= b->prefix[0] ? a->first : b->prefix[0];
      res->last  = a->last  >= b->prefix[0] ? a->last  : b->prefix[0];
    }
    else if( blen == 0 ) {
      res->first = b->first <= a->prefix[0] ? b->first : a->prefix[0];
      res->last  = b->last  >= a->prefix[0] ? b->last  : a->prefix[0];
    }
  }
  else {
    res = build_pr(gp, 0, 0);

    if( gplen == alen && alen == blen ) {
      res->first = a->first <= b->first ? a->first : b->first;
      res->last  = a->last  >= b->last  ? a->last : b->last;
    }
    else if( gplen == alen ) {
      Assert(alen < blen);
      res->first = a->first <= b->prefix[alen] ? a->first : b->prefix[alen];
      res->last  = a->last  >= b->prefix[alen] ? a->last  : b->prefix[alen];
    }
    else if( gplen == blen ) {
      Assert(blen < alen);
      res->first = b->first <= a->prefix[blen] ? b->first : a->prefix[blen];
      res->last  = b->last  >= a->prefix[blen] ? b->last  : a->prefix[blen];
    }
    else {
      Assert(gplen < alen && gplen < blen);
      min = a->prefix[gplen];
      max = b->prefix[gplen];

      if( min > max ) {
	min = b->prefix[gplen];
	max = a->prefix[gplen];
      }
      res->first = min;
      res->last  = max;
#ifdef DEBUG_UNION
    elog(NOTICE, "union a: %s %d %d", a->prefix, a->first, a->last);
    elog(NOTICE, "union b: %s %d %d", b->prefix, b->first, b->last);
    elog(NOTICE, "union r: %s %d %d", res->prefix, res->first, res->last);
#endif
    }
  }
  return pr_normalize(res);
}

static inline
prefix_range *pr_inter(prefix_range *a, prefix_range *b) {
  prefix_range *res = NULL;
  int alen = strlen(a->prefix);
  int blen = strlen(b->prefix);
  char *gp = NULL;
  int gplen;

  if( 0 == alen && 0 == blen ) {
    res = build_pr("",
		   a->first > b->first ? a->first : b->first,
		   a->last  < b->last  ? a->last  : b->last);
    return pr_normalize(res);
  }

  gp = __greater_prefix(a->prefix, b->prefix, alen, blen);
  gplen = strlen(gp);

  if( gplen != alen && gplen != blen ) {
    return build_pr("", 0, 0);
  }

  if( gplen == alen && 0 == alen ) {
    if( a->first <= b->prefix[0] && b->prefix[0] <= a->last ) {
      res = build_pr(b->prefix, b->first, b->last);
    }
    else
      res = build_pr("", 0, 0);
  }
  else if( gplen == blen && 0 == blen ) {
    if( b->first <= a->prefix[0] && a->prefix[0] <= b->last ) {
      res = build_pr(a->prefix, a->first, a->last);
    }
    else
      res = build_pr("", 0, 0);
  }
  else if( gplen == alen && alen == blen ) {
	  char first, last;

	  if( a->first == 0 )
		  first = b->first;
	  else
		  first = a->first > b->first ? a->first : b->first;

	  if( a->last == 0 )
		  last = b->last;
	  else
		  last = a->last  > b->last  ? b->last  : a->last;

	  res = build_pr(gp, first, last);

#ifdef DEBUG_INTER
    elog(NOTICE, "inter a: %s %d %d", a->prefix, a->first, a->last);
    elog(NOTICE, "inter b: %s %d %d", b->prefix, b->first, b->last);
    elog(NOTICE, "inter r: %s %d %d", res->prefix, res->first, res->last);
#endif
  }
  else if( gplen == alen ) {
    Assert(gplen < blen);
    res = build_pr(b->prefix, b->first, b->last);
  }
  else if( gplen == blen ) {
    Assert(gplen < alen);
    res = build_pr(a->prefix, a->first, a->last);
  }

  return pr_normalize(res);
}

/**
 * true if ranges have at least one common element
 */
static inline
bool pr_overlaps(prefix_range *a, prefix_range *b) {
  prefix_range *inter = pr_inter(a, b);

  return strlen(inter->prefix) > 0 || (inter->first != 0 && inter->last != 0);
}


PG_FUNCTION_INFO_V1(prefix_range_init);
Datum
prefix_range_init(PG_FUNCTION_ARGS)
{
  text *txt  = PG_GETARG_TEXT_P(0);
  char *str  =
    DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(txt)));

  text *t_first = PG_GETARG_TEXT_P(1);
  char *c_first =
    DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(t_first)));

  text *t_last  = PG_GETARG_TEXT_P(2);
  char *c_last =
    DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(t_last)));

  int flen = t_first == NULL ? 0 : strlen(c_first);
  int llen = t_last  == NULL ? 0 : strlen(c_last);

  char first, last;

  if( flen > 1 || llen > 1 )
    ereport(ERROR,
	    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	     errmsg("prefix_range first and last must be at most 1 char long.")
	     ));

  first = flen == 0 ? 0 : c_first[0];
  last  = llen == 0 ? 0 : c_last[0];

  PG_RETURN_PREFIX_RANGE_P(make_prefix_range(str, first, last));
}

PG_FUNCTION_INFO_V1(prefix_range_in);
Datum
prefix_range_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    prefix_range *pr = pr_from_str(str);

    if (pr != NULL) {
      PG_RETURN_PREFIX_RANGE_P(pr);
    }

    ereport(ERROR,
	    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	     errmsg("invalid prefix_range value: \"%s\"", str)));
    PG_RETURN_NULL();
}


PG_FUNCTION_INFO_V1(prefix_range_out);
Datum
prefix_range_out(PG_FUNCTION_ARGS)
{
  prefix_range *pr = PG_GETARG_PREFIX_RANGE_P(0);
  char *out = NULL;

  if( pr->first ) {
    out = (char *)palloc((strlen(pr->prefix)+6) * sizeof(char));
    sprintf(out, "%s[%c-%c]", pr->prefix, pr->first, pr->last);
  }
  else {
    out = (char *)palloc((strlen(pr->prefix)+1) * sizeof(char));
    sprintf(out, "%s", pr->prefix);
  }
  PG_RETURN_CSTRING(out);
}

PG_FUNCTION_INFO_V1(prefix_range_recv);
Datum
prefix_range_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);
    const char *first = pq_getmsgbytes(buf, 1);
    const char *last  = pq_getmsgbytes(buf, 1);
    const char *prefix = pq_getmsgstring(buf);
    prefix_range *pr = build_pr(prefix, *first, *last);

    pq_getmsgend(buf);
    PG_RETURN_PREFIX_RANGE_P(pr);
}

PG_FUNCTION_INFO_V1(prefix_range_send);
Datum
prefix_range_send(PG_FUNCTION_ARGS)
{
    prefix_range *pr = PG_GETARG_PREFIX_RANGE_P(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendbyte(&buf, pr->first);
    pq_sendbyte(&buf, pr->last);
    pq_sendstring(&buf, pr->prefix);

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(prefix_range_cast_from_text);
Datum
prefix_range_cast_from_text(PG_FUNCTION_ARGS)
{
  text *txt = PG_GETARG_TEXT_P(0);
  Datum cstring = DirectFunctionCall1(textout, PointerGetDatum(txt));
  return DirectFunctionCall1(prefix_range_in, cstring);
}

PG_FUNCTION_INFO_V1(prefix_range_cast_to_text);
Datum
prefix_range_cast_to_text(PG_FUNCTION_ARGS)
{
  prefix_range *pr = PG_GETARG_PREFIX_RANGE_P(0);
  Datum cstring;
  text *out;

  if (pr != NULL) {
    cstring = DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(pr));
    out = (text *)DirectFunctionCall1(textin, cstring);

    PG_RETURN_TEXT_P(out);
  }
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(prefix_range_length);
Datum
prefix_range_length(PG_FUNCTION_ARGS)
{
  PG_RETURN_INT32( pr_length(PG_GETARG_PREFIX_RANGE_P(0)) );
}

PG_FUNCTION_INFO_V1(prefix_range_eq);
Datum
prefix_range_eq(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_eq(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_neq);
Datum
prefix_range_neq(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( ! pr_eq(PG_GETARG_PREFIX_RANGE_P(0),
			  PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_lt);
Datum
prefix_range_lt(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_lt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			false) );
}

PG_FUNCTION_INFO_V1(prefix_range_le);
Datum
prefix_range_le(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_lt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			true) );
}

PG_FUNCTION_INFO_V1(prefix_range_gt);
Datum
prefix_range_gt(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_gt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			false) );
}

PG_FUNCTION_INFO_V1(prefix_range_ge);
Datum
prefix_range_ge(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_gt(PG_GETARG_PREFIX_RANGE_P(0),
			PG_GETARG_PREFIX_RANGE_P(1),
			true) );
}

PG_FUNCTION_INFO_V1(prefix_range_cmp);
Datum
prefix_range_cmp(PG_FUNCTION_ARGS)
{
  prefix_range *a = PG_GETARG_PREFIX_RANGE_P(0);
  prefix_range *b = PG_GETARG_PREFIX_RANGE_P(1);

  PG_RETURN_INT32(pr_cmp(a, b));
}

PG_FUNCTION_INFO_V1(prefix_range_overlaps);
Datum
prefix_range_overlaps(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_overlaps(PG_GETARG_PREFIX_RANGE_P(0),
			      PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_contains);
Datum
prefix_range_contains(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(0),
			      PG_GETARG_PREFIX_RANGE_P(1),
			      true ));
}

PG_FUNCTION_INFO_V1(prefix_range_contains_strict);
Datum
prefix_range_contains_strict(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(0),
			      PG_GETARG_PREFIX_RANGE_P(1),
			      false ));
}

PG_FUNCTION_INFO_V1(prefix_range_contained_by);
Datum
prefix_range_contained_by(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(1),
			      PG_GETARG_PREFIX_RANGE_P(0),
			      true ));
}

PG_FUNCTION_INFO_V1(prefix_range_contained_by_strict);
Datum
prefix_range_contained_by_strict(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL( pr_contains(PG_GETARG_PREFIX_RANGE_P(1),
			      PG_GETARG_PREFIX_RANGE_P(0),
			      false ));
}

PG_FUNCTION_INFO_V1(prefix_range_union);
Datum
prefix_range_union(PG_FUNCTION_ARGS)
{
  PG_RETURN_PREFIX_RANGE_P( pr_union(PG_GETARG_PREFIX_RANGE_P(0),
				     PG_GETARG_PREFIX_RANGE_P(1)) );
}

PG_FUNCTION_INFO_V1(prefix_range_inter);
Datum
prefix_range_inter(PG_FUNCTION_ARGS)
{
  PG_RETURN_PREFIX_RANGE_P( pr_inter(PG_GETARG_PREFIX_RANGE_P(0),
				     PG_GETARG_PREFIX_RANGE_P(1)) );
}

/**
 * GiST support methods
 *
 * pr_penalty allows SQL level penalty code testing.
 */
Datum gpr_consistent(PG_FUNCTION_ARGS);
Datum gpr_compress(PG_FUNCTION_ARGS);
Datum gpr_decompress(PG_FUNCTION_ARGS);
Datum gpr_penalty(PG_FUNCTION_ARGS);
Datum gpr_picksplit(PG_FUNCTION_ARGS);
Datum gpr_picksplit_presort(PG_FUNCTION_ARGS);
Datum gpr_picksplit_jordan(PG_FUNCTION_ARGS);
Datum gpr_union(PG_FUNCTION_ARGS);
Datum gpr_same(PG_FUNCTION_ARGS);
Datum pr_penalty(PG_FUNCTION_ARGS);

/*
 * Internal implementation of consistent
 *
  OPERATOR	1	@>,
  OPERATOR	2	<@,
  OPERATOR	3	=,
  OPERATOR	4	&&,
*/
static inline
bool pr_consistent(StrategyNumber strategy,
		   prefix_range *key, prefix_range *query, bool is_leaf) {

    switch (strategy) {
    case 1:
      return pr_contains(key, query, true);

    case 2:
      if ( is_leaf ) {
        return pr_contains(query, key, true);
      } else {
	return pr_overlaps(query, key);
      }

    case 3:
      if( is_leaf ) {

#ifdef DEBUG_CONSISTENT
	elog(NOTICE, "gpr_consistent: %s %c= %s",

	     DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(key))),
	     pr_eq(key, query) ? '=' : '!',
	     DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(query))));

#endif
	return pr_eq(key, query);
      }
      else {
	return pr_contains(key, query, true);
      }

    case 4:
      return pr_overlaps(key, query);

    default:
      return false;
    }
}

/*
 * The consistent function signature has changed in 8.4 to include RECHECK
 * handling, but the signature declared in the OPERATOR CLASS is not
 * considered at all.
 *
 * Still the function is called by mean of the fmgr, so we know whether
 * we're called with pre-8.4 conventions or not by checking PG_NARGS().
 *
 * In all cases, we currently ignore the oid parameter (subtype).
 */
PG_FUNCTION_INFO_V1(gpr_consistent);
Datum
gpr_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    prefix_range *query = PG_GETARG_PREFIX_RANGE_P(1);
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    prefix_range *key = DatumGetPrefixRange(entry->key);
    bool *recheck;

    Assert( PG_NARGS() == 4 || PG_NARGS() == 5);

    if( PG_NARGS() == 5 ) {
      /*
       * New API in 8.4:
       *  consistent(internal, data_type, smallint, oid, internal)
       *
       * We don't ever want to recheck: index ain't lossy, we store
       * prefix_range keys on the leaves.
       */
      recheck  = (bool *) PG_GETARG_POINTER(4);
      *recheck = false;
    }
    PG_RETURN_BOOL( pr_consistent(strategy, key, query, GIST_LEAF(entry)) );
}

/*
 * GiST Compress and Decompress methods for prefix_range
 * do not do anything.
 */
PG_FUNCTION_INFO_V1(gpr_compress);
Datum
gpr_compress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(gpr_decompress);
Datum
gpr_decompress(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

static
float __pr_penalty(prefix_range *orig, prefix_range *new)
{
  float penalty;
  char *gp;
  int  nlen, olen, gplen, dist = 0;
  char tmp;

#ifdef DEBUG_PENALTY
  if( orig->prefix[0] != 0 ) {
    /**
     * The prefix main test case deals with phone number data, hence
     * containing only numbers...
     */
    if( orig->prefix[0] < '0' || orig->prefix[0] > '9' )
      elog(NOTICE, "__pr_penalty(%s, %s) orig->first=%d orig->last=%d ",
	   DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(orig))),
	   DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(new))),
	   orig->first, orig->last);
    Assert(orig->prefix[0] >= '0' && orig->prefix[0] <= '9');
  }
#endif

  olen  = strlen(orig->prefix);
  nlen  = strlen(new->prefix);
  gp    = __greater_prefix(orig->prefix, new->prefix, olen, nlen);
  gplen = strlen(gp);

  dist  = 1;

  if( 0 == olen && 0 == nlen ) {
    if( orig->last >= new->first )
      dist = 0;
    else
      dist = new->first - orig->last;
  }
  else if( 0 == olen ) {
    /**
     * penalty('[a-b]', 'xyz');
     */
    if( orig->first != 0 ) {
      tmp = new->prefix[0];

      if( orig->first <= tmp && tmp <= orig->last ) {
	gplen = 1;

	dist = 1 + (int)tmp - (int)orig->first;
	if( (1 + (int)orig->last - (int)tmp) < dist )
	  dist = 1 + (int)orig->last - (int)tmp;
      }
      else
	dist = (orig->first > tmp ? orig->first - tmp  : tmp - orig->last );
    }
  }
  else if( 0 == nlen ) {
    /**
     * penalty('abc', '[x-y]');
     */
    if( new->first != 0 ) {
      tmp = orig->prefix[0];

      if( new->first <= tmp && tmp <= new->last ) {
	gplen = 1;

	dist = 1 + (int)tmp - (int)new->first;
	if( (1 + (int)new->last - (int)tmp) < dist )
	  dist = 1 + (int)new->last - (int)tmp;
      }
      else
	dist = (new->first > tmp ? new->first - tmp  : tmp - new->last );
    }
  }
  else {
    /**
     * General case
     */

    if( gplen > 0 ) {
      if( olen > gplen && nlen == gplen && new->first != 0 ) {
	/**
	 * gpr_penalty('abc[f-l]', 'ab[x-y]')
	 */
	if( new->first <= orig->prefix[gplen]
	    && orig->prefix[gplen] <= new->last ) {

	  dist   = 1 + (int)orig->prefix[gplen] - (int)new->first;
	  if( (1 + (int)new->last - (int)orig->prefix[gplen]) < dist )
	    dist = 1 + (int)new->last - (int)orig->prefix[gplen];

	  gplen += 1;
	}
	else {
	  dist += 1;
	}
      }
      else if( nlen > gplen && olen == gplen && orig->first != 0 ) {
	/**
	 * gpr_penalty('ab[f-l]', 'abc[x-y]')
	 */
	if( orig->first <= new->prefix[gplen]
	    && new->prefix[gplen] <= orig->last ) {

	  dist   = 1 + (int)new->prefix[gplen] - (int)orig->first;
	  if( (1 + (int)orig->last - (int)new->prefix[gplen]) < dist )
	    dist = 1 + (int)orig->last - (int)new->prefix[gplen];

	  gplen += 1;
	}
	else {
	  dist += 1;
	}
      }
    }
    /**
     * penalty('abc[f-l]', 'xyz[g-m]'), nothing common
     * dist = 1, gplen = 0, penalty = 1
     */
  }
  penalty = (((float)dist) / powf(256, gplen));

#ifdef DEBUG_PENALTY
  elog(NOTICE, "__pr_penalty(%s, %s) == %d/(256^%d) == %g",
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(orig))),
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(new))),
       dist, gplen, penalty);
#endif

  return penalty;
}

PG_FUNCTION_INFO_V1(gpr_penalty);
Datum
gpr_penalty(PG_FUNCTION_ARGS)
{
  GISTENTRY *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
  GISTENTRY *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
  float *penalty = (float *) PG_GETARG_POINTER(2);

  prefix_range *orig = DatumGetPrefixRange(origentry->key);
  prefix_range *new  = DatumGetPrefixRange(newentry->key);

  *penalty = __pr_penalty(orig, new);
  PG_RETURN_POINTER(penalty);
}

PG_FUNCTION_INFO_V1(pr_penalty);
Datum
pr_penalty(PG_FUNCTION_ARGS)
{
  float penalty = __pr_penalty(PG_GETARG_PREFIX_RANGE_P(0),
			       PG_GETARG_PREFIX_RANGE_P(1));
  PG_RETURN_FLOAT4(penalty);
}

/*
 * That's an experimental feature, only used in the
 * gist_prefix_range_jordan_ops opclass, which is not talked about in the
 * user documentation of the module.
 */
static int gpr_cmp(const void *a, const void *b) {
  GISTENTRY **e1 = (GISTENTRY **)a;
  GISTENTRY **e2 = (GISTENTRY **)b;
  prefix_range *k1 = DatumGetPrefixRange((*e1)->key);
  prefix_range *k2 = DatumGetPrefixRange((*e2)->key);

  return pr_cmp(k1, k2);
}

/**
 * Median idea from Jordan:
 *
 * sort the entries and choose a cut point near the median, being
 * careful not to cut a group sharing a common prefix when sensible.
 */
PG_FUNCTION_INFO_V1(gpr_picksplit_jordan);
Datum
gpr_picksplit_jordan(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    OffsetNumber maxoff = entryvec->n - 1;
    GISTENTRY *ent      = entryvec->vector;
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    int	i, nbytes;
    OffsetNumber *left, *right;
    prefix_range *tmp_union;
    prefix_range *unionL;
    prefix_range *unionR;

    GISTENTRY **raw_entryvec;
    int cut, cut_tolerance, lower_dist, upper_dist;

    maxoff = entryvec->n - 1;
    nbytes = (maxoff + 1) * sizeof(OffsetNumber);

    v->spl_left  = (OffsetNumber *) palloc(nbytes);
    left         = v->spl_left;
    v->spl_nleft = 0;

    v->spl_right  = (OffsetNumber *) palloc(nbytes);
    right         = v->spl_right;
    v->spl_nright = 0;

    unionL = NULL;
    unionR = NULL;

    /* Initialize the raw entry vector. */
    raw_entryvec = (GISTENTRY **) malloc(entryvec->n * sizeof(void *));
    for (i=FirstOffsetNumber; i <= maxoff; i=OffsetNumberNext(i))
      raw_entryvec[i] = &(entryvec->vector[i]);

    /* Sort the raw entry vector.
     */
    pg_qsort(&raw_entryvec[1], maxoff, sizeof(void *), &gpr_cmp);

    /*
     * Find the distance between the middle of the raw entry vector and the
     * lower-index of the first group.
     */
    cut = maxoff / 2;
    cut_tolerance = cut / 2;
    for (i=cut - 1; i > FirstOffsetNumber; i=OffsetNumberPrev(i)) {
      tmp_union = pr_union(DatumGetPrefixRange(ent[i].key),
			   DatumGetPrefixRange(ent[i+1].key));

      if( strlen(tmp_union->prefix) == 0 )
	break;
    }
    lower_dist = cut - i;

    /*
     * Find the distance between the middle of the raw entry vector and the
     * upper-index of the first group.
     */
    for (i=1 + cut; i < maxoff; i=OffsetNumberNext(i)) {
      tmp_union = pr_union(DatumGetPrefixRange(ent[i].key),
			   DatumGetPrefixRange(ent[i-1].key));

      if( strlen(tmp_union->prefix) == 0 )
	break;
    }
    upper_dist = i - cut;

    /*
     * Choose the cut based on whichever falls within the cut tolerance and
     * is closer to the midpoint.  Choose one at random it there is a tie.
     *
     * If neither are within the tolerance, use the midpoint as the default.
     */
    if (lower_dist <= cut_tolerance || upper_dist <= cut_tolerance) {
      if (lower_dist < upper_dist)
	cut -= lower_dist;
      else if (upper_dist < lower_dist)
	cut += upper_dist;
      else
	cut = (random() % 2) ? (cut - lower_dist) : (cut + upper_dist);
    }

    for (i=FirstOffsetNumber; i <= maxoff; i=OffsetNumberNext(i)) {
      int real_index = raw_entryvec[i] - entryvec->vector;
      // datum_alpha = VECGETKEY(entryvec, real_index);
      tmp_union = DatumGetPrefixRange(entryvec->vector[real_index].key);

      Assert(tmp_union != NULL);

      /* Put everything below the cut in the left node. */
      if (i < cut) {
	if( unionL == NULL )
	  unionL = tmp_union;
	else
	  unionL = pr_union(unionL, tmp_union);

	*left = real_index;
	++left;
	++(v->spl_nleft);
      }
      /* And put everything else in the right node. */
      else {
	if( unionR == NULL )
	  unionR = tmp_union;
	else
	  unionR = pr_union(unionR, tmp_union);

	*right = real_index;
	++right;
	++(v->spl_nright);
      }
    }

    *left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */
    v->spl_ldatum = PrefixRangeGetDatum(unionL);
    v->spl_rdatum = PrefixRangeGetDatum(unionR);
    PG_RETURN_POINTER(v);
}

/**
 * prefix picksplit first pass step: presort the GistEntryVector
 * vector by positionning the elements sharing the non-empty prefix
 * which is the more frequent in the distribution at the beginning of
 * the vector.
 *
 * This will have the effect that the picksplit() implementation will
 * do a better job, per preliminary tests on not-so random data.
 */
struct gpr_unions
{
  prefix_range *prefix;     /* a shared prefix */
  int   n;                  /* how many entries begins with this prefix */
};


static
OffsetNumber *pr_presort(GistEntryVector *list)
{
  GISTENTRY *ent      = list->vector;
  OffsetNumber maxoff = list->n - 1;
  prefix_range *init  = DatumGetPrefixRange(ent[FirstOffsetNumber].key);
  prefix_range *cur, *gp;
  int  gplen;
  bool found;

  struct gpr_unions max;
  struct gpr_unions *unions = (struct gpr_unions *)
    palloc((maxoff+1) * sizeof(struct gpr_unions));

  OffsetNumber unions_it = FirstOffsetNumber; /* unions iterator */
  OffsetNumber i, u;

  int result_it, result_it_maxes = FirstOffsetNumber;
  OffsetNumber *result = (OffsetNumber *) malloc(list->n * sizeof(OffsetNumber));

#ifdef DEBUG_PRESORT_MAX
#define DEBUG_COUNT
  int debug_count;
#endif
#ifdef DEBUG_PRESORT_UNIONS
#ifndef DEBUG_COUNT
  int debug_count;
#endif
#endif

  unions[unions_it].prefix = init;
  unions[unions_it].n      = 1;
  unions_it = OffsetNumberNext(unions_it);

  max.prefix = init;
  max.n      = 1;

#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   init=%s max.prefix=%s max.n=%d",
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(init))),
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(max.prefix))),
       max.n);
#endif

  /**
   * Prepare a list of prefixes and how many time they are found.
   */
  for(i = OffsetNumberNext(FirstOffsetNumber); i <= maxoff; i = OffsetNumberNext(i)) {
    found = false;
    cur   = DatumGetPrefixRange(ent[i].key);

    for(u = FirstOffsetNumber; u < unions_it; u = OffsetNumberNext(u)) {
      if( unions[u].n < 1 )
	continue;

      gp = pr_union(cur, unions[u].prefix);
      gplen = strlen(gp->prefix);

#ifdef DEBUG_PRESORT_GP
      if( gplen > 0 ) {
	elog(NOTICE, " prefix_presort():   gplen=%2d, %s @> %s = %s",
	     gplen,
	     DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(gp))),
	     DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(cur))),
	     (pr_contains(gp, cur, true) ? "t" : "f"));
      }
#endif

      if( gplen > 0 ) {
	/**
	 * Current list entry share a common prefix with some previous
	 * analyzed list entry, update the prefix and number.
	 */
	found = true;
	unions[u].n     += 1;
	unions[u].prefix = gp;

	/**
	 * We just updated unions, we may have to update max too.
	 */
	if( unions[u].n > max.n ) {
	  max.prefix = unions[u].prefix;
	  max.n      = unions[u].n;
#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   max.prefix=%s max.n=%d",
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(max.prefix))),
       max.n);
#endif
	}

	/**
	 * break from the unions loop, we're done with it for this
	 * element.
	 */
	break;
      }
    }
    /**
     * We're done with the unions loop, if we didn't find a common
     * prefix we have to add the current list element to unions
     */
    if( !found ) {
      unions[unions_it].prefix = cur;
      unions[unions_it].n      = 1;
      unions_it = OffsetNumberNext(unions_it);
    }
  }
#ifdef DEBUG_PRESORT_UNIONS
  debug_count = 0;
  for(u = FirstOffsetNumber; u < unions_it; u = OffsetNumberNext(u)) {
    debug_count += unions[u].n;
    elog(NOTICE, " prefix_presort():   unions[%s] = %d",
	 DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(unions[u].prefix))),
	 unions[u].n);
  }
  elog(NOTICE, " prefix_presort():   total: %d", debug_count);
#endif

#ifdef DEBUG_PRESORT_MAX
  debug_count = 0;
  for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
    cur   = DatumGetPrefixRange(ent[i].key);

    if( pr_contains(max.prefix, cur, true) )
      debug_count++;
  }
  elog(NOTICE, " prefix_presort():   max.prefix %s @> %d entries",
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(max.prefix))),
       debug_count);
#endif

  /**
   * We now have a list of common non-empty prefixes found on the list
   * (unions) and kept the max entry while computing this weighted
   * unions list.
   *
   * Simple case : a common non-empty prefix is shared by all list
   * entries.
   */
  if( max.n >= list->n ) {
    /**
     * A common non-empty prefix is shared by all list entries
     */
    for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
      result[i] = i;
    }
    return result;
  }

  /**
   * If we arrive here, we now have to make up the result by copying
   * max matching elements first, then the others list entries in
   * their original order. To do this, we reserve the first result
   * max.n places to the max.prefix matching elements (see result_it
   * and result_it_maxes).
   *
   * result_it_maxes will go from FirstOffsetNumber to max.n included,
   * and result_it will iterate through the end of the list, that is
   * from max.n - FirstOffsetNumber + 1 to maxoff.
   *
   * [a, b] contains b - a + 1 elements, hence
   * [FirstOffsetNumber, max.n] contains max.n - FirstOffsetNumber + 1
   * elements, whatever FirstOffsetNumber value.
   */
  result_it_maxes = FirstOffsetNumber;
  result_it       = OffsetNumberNext(max.n - FirstOffsetNumber + 1);

#ifdef DEBUG_PRESORT_MAX
  elog(NOTICE, " prefix_presort():   max.prefix=%s max.n=%d result_it=%d",
       DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(max.prefix))),
       max.n, result_it);
#endif

  for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
    cur = DatumGetPrefixRange(ent[i].key);

#ifdef DEBUG_PRESORT_RESULT
    elog(NOTICE, " prefix_presort():   ent[%4d] = %s <@ %s = %s => result[%4d]",
	 i,
	 DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(cur))),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out,PrefixRangeGetDatum(max.prefix))),
	 (pr_contains(max.prefix, cur, true) ? "t" : "f"),
	 (pr_contains(max.prefix, cur, true) ? result_it_maxes : result_it));
#endif

    if( pr_contains(max.prefix, cur, true) ) {
      /**
       * cur has to go in first part of the list, as max.prefix is a
       * prefix of it.
       */
      Assert(result_it_maxes <= max.n);
      result[result_it_maxes] = i;
      result_it_maxes = OffsetNumberNext(result_it_maxes);
    }
    else {
      /**
       * cur has to go at next second part position.
       */
      Assert(result_it <= maxoff);
      result[result_it] = i;
      result_it = OffsetNumberNext(result_it);
    }
  }
#ifdef DEBUG_PRESORT_RESULT
  elog(NOTICE, " prefix_presort():   result_it_maxes=%4d result_it=%4d list->n=%d maxoff=%d",
       result_it_maxes, result_it, list->n, maxoff);
#endif
  return result;
}


/**
 * Internal picksplit function, with a presort option.
 *
 * This form allows for defining two different opclasses for
 * comparative testing purposes.
 */
static
Datum pr_picksplit(GistEntryVector *entryvec, GIST_SPLITVEC *v, bool presort) {
    OffsetNumber maxoff = entryvec->n - 1;
    GISTENTRY *ent      = entryvec->vector;

    OffsetNumber *sort  = NULL;

    int	nbytes;
    OffsetNumber offl, offr;
    OffsetNumber *listL;
    OffsetNumber *listR;
    prefix_range *curl, *curr, *tmp_union;
    prefix_range *unionL;
    prefix_range *unionR;

    /**
     * Keeping track of penalties to insert into ListL or ListR, for
     * both the leftmost and the rightmost element of the remaining
     * list.
     */
    float pll, plr, prl, prr;

#ifdef DEBUG_PICKSPLIT
    OffsetNumber i;
#endif

    if( presort ) {
      sort = pr_presort(entryvec);

#ifdef DEBUG_PICKSPLIT
      for(i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i) ) {
	elog(NOTICE, "pr_picksplit: sort[%3d] = %2d", i, sort[i]);
      }
#endif
    }

    nbytes = (maxoff + 1) * sizeof(OffsetNumber);
    listL = (OffsetNumber *) palloc(nbytes);
    listR = (OffsetNumber *) palloc(nbytes);
    v->spl_left  = listL;
    v->spl_right = listR;
    v->spl_nleft = v->spl_nright = 0;

    offl = FirstOffsetNumber;
    offr = maxoff;

    unionL = DatumGetPrefixRange(ent[offl].key);
    unionR = DatumGetPrefixRange(ent[offr].key);

    v->spl_left[v->spl_nleft++]   = offl;
    v->spl_right[v->spl_nright++] = offr;
    v->spl_left  = listL;
    v->spl_right = listR;

    offl = OffsetNumberNext(offl);
    offr = OffsetNumberPrev(offr);

    while( offl < offr ) {

      if( presort ) {
	curl = DatumGetPrefixRange(ent[sort[offl]].key);
	curr = DatumGetPrefixRange(ent[sort[offr]].key);
      }
      else {
	curl = DatumGetPrefixRange(ent[offl].key);
	curr = DatumGetPrefixRange(ent[offr].key);
      }

#ifdef DEBUG_PICKSPLIT
      elog(NOTICE, "gpr_picksplit: ent[%3d] = '%s' \tent[%3d] = '%s'",
	   offl,
	   DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(curl))),
	   offr,
	   DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(curr))));
#endif

      Assert(curl != NULL && curr != NULL);

      pll = __pr_penalty(unionL, curl);
      plr = __pr_penalty(unionR, curl);
      prl = __pr_penalty(unionL, curr);
      prr = __pr_penalty(unionR, curr);

      if( pll <= plr && prl >= prr ) {
	/**
	 * curl should go to left and curr to right, unless they share
	 * a non-empty common prefix, in which case we place both curr
	 * and curl on the same side. Arbitrarily the left one.
	 */
	if( pll == plr && prl == prr ) {
	  tmp_union = pr_union(curl, curr);

	  if( strlen(tmp_union->prefix) > 0 ) {
	    unionL = pr_union(unionL, tmp_union);
	    v->spl_left[v->spl_nleft++] = offl;
	    v->spl_left[v->spl_nleft++] = offr;

	    offl = OffsetNumberNext(offl);
	    offr = OffsetNumberPrev(offr);
	    continue;
	  }
	}
	/**
	 * here pll <= plr and prl >= prr and (pll != plr || prl != prr)
	 */
	unionL = pr_union(unionL, curl);
	unionR = pr_union(unionR, curr);

	v->spl_left[v->spl_nleft++]   = offl;
	v->spl_right[v->spl_nright++] = offr;

	offl = OffsetNumberNext(offl);
	offr = OffsetNumberPrev(offr);
      }
      else if( pll > plr && prl >= prr ) {
	/**
	 * Current rightmost entry is added to listL
	 */
	unionR = pr_union(unionR, curr);
	v->spl_right[v->spl_nright++] = offr;
	offr = OffsetNumberPrev(offr);
      }
      else if( pll <= plr && prl < prr ) {
	/**
	 * Current leftmost entry is added to listL
	 */
	unionL = pr_union(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
	offl = OffsetNumberNext(offl);
      }
      else if( (pll - plr) < (prr - prl) ) {
	/**
	 * All entries still in the list go into listL
	 */
	for(; offl <= offr; offl = OffsetNumberNext(offl)) {
	  curl   = DatumGetPrefixRange(ent[offl].key);
	  unionL = pr_union(unionL, curl);
	  v->spl_left[v->spl_nleft++] = offl;
	}
      }
      else {
	/**
	 * All entries still in the list go into listR
	 */
	for(; offr >= offl; offr = OffsetNumberPrev(offr)) {
	  curr   = DatumGetPrefixRange(ent[offr].key);
	  unionR = pr_union(unionR, curr);
	  v->spl_right[v->spl_nright++] = offr;
	}
      }
    }

    /**
     * The for loop continues while offl < offr. If maxoff is odd, it
     * could be that there's a last value to process. Here we choose
     * where to add it.
     */
    if( offl == offr ) {
      curl = DatumGetPrefixRange(ent[offl].key);

      pll  = __pr_penalty(unionL, curl);
      plr  = __pr_penalty(unionR, curl);

      if( pll < plr || (pll == plr && v->spl_nleft < v->spl_nright) ) {
	curl       = DatumGetPrefixRange(ent[offl].key);
	unionL     = pr_union(unionL, curl);
	v->spl_left[v->spl_nleft++] = offl;
      }
      else {
	curl       = DatumGetPrefixRange(ent[offl].key);
	unionR     = pr_union(unionR, curl);
	v->spl_right[v->spl_nright++] = offl;
      }
    }

    v->spl_ldatum = PrefixRangeGetDatum(unionL);
    v->spl_rdatum = PrefixRangeGetDatum(unionR);

    /**
     * All read entries (maxoff) should have make it to the
     * GIST_SPLITVEC return value.
     */
    Assert(maxoff = v->spl_nleft+v->spl_nright);

#ifdef DEBUG_PICKSPLIT
    elog(NOTICE, "gpr_picksplit(): entryvec->n=%4d maxoff=%4d l=%4d r=%4d l+r=%4d unionL='%s' unionR='%s'",
	 entryvec->n, maxoff, v->spl_nleft, v->spl_nright, v->spl_nleft+v->spl_nright,
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, v->spl_ldatum)),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, v->spl_rdatum)));
#endif

    PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(gpr_picksplit);
Datum
gpr_picksplit(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    PG_RETURN_DATUM(pr_picksplit(entryvec, v, false));
}

PG_FUNCTION_INFO_V1(gpr_picksplit_presort);
Datum
gpr_picksplit_presort(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

    PG_RETURN_DATUM(pr_picksplit(entryvec, v, true));
}

PG_FUNCTION_INFO_V1(gpr_union);
Datum
gpr_union(PG_FUNCTION_ARGS)
{
    GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    GISTENTRY *ent = entryvec->vector;

    prefix_range *out, *tmp;
    int	numranges, i = 0;

    numranges = entryvec->n;
    tmp = DatumGetPrefixRange(ent[0].key);
    out = tmp;

    if( numranges == 1 ) {
      out = build_pr(tmp->prefix, tmp->first, tmp->last);

      PG_RETURN_PREFIX_RANGE_P(out);
    }

    for (i = 1; i < numranges; i++) {
#ifdef DEBUG_UNION
      prefix_range *old = out;
#endif
      tmp = DatumGetPrefixRange(ent[i].key);
      out = pr_union(out, tmp);

#ifdef DEBUG_UNION
    elog(NOTICE, "gpr_union: %s | %s = %s",
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(old))),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(tmp))),
	 DatumGetCString(DirectFunctionCall1(prefix_range_out, PrefixRangeGetDatum(out))));
#endif
    }

    /*
#ifdef DEBUG_UNION
    elog(NOTICE, "gpr_union: %s",
	 DatumGetCString(DirectFunctionCall1(prefix_range_out,
					     PrefixRangeGetDatum(out))));
#endif
    */
    PG_RETURN_PREFIX_RANGE_P(out);
}

PG_FUNCTION_INFO_V1(gpr_same);
Datum
gpr_same(PG_FUNCTION_ARGS)
{
    prefix_range *v1 = PG_GETARG_PREFIX_RANGE_P(0);
    prefix_range *v2 = PG_GETARG_PREFIX_RANGE_P(1);
    bool *result = (bool *) PG_GETARG_POINTER(2);

    *result = pr_eq(v1, v2);
    PG_RETURN_POINTER( result );
}
