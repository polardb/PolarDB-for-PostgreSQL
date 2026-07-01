/*-------------------------------------------------------------------------
*
* polar_masking.c
*		Process data masking for query after rewrite.
*
* IDENTIFICATION
*    external/polar_masking/polar_masking.c
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "polar_masking.h"

#include "nodes/makefuncs.h"
#include "rewrite/rewriteHandler.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
void		_PG_fini(void);

bool		polar_masking_enabled = false;

static polar_post_rewrite_query_hook_type next_polar_post_rewrite_query_hook = NULL;
static inline Node *create_string_node(Datum dat);
static inline Node *create_integer_node(int value);
static inline Node *create_relabeltype_node(Node *node, int resulttype);
static void polar_process_masking_after_rewrite(List *query_list);

Node	   *make_func_node(int funcid, Oid rettype, Node *arg);
Node	   *make_func_node_regex(int funcid, Oid rettype, Node *arg, MaskingInfo * maskinfo);

/*
 * function for creating string node for masking function nodes' arg
 */
static inline Node *
create_string_node(Datum dat)
{
	Const	   *const_node = makeConst(TEXTOID,
									   -1,	/* typmod -1 is OK for all cases */
									   InvalidOid,	/* all cases are
													 * uncollatable types */
									   -2,
									   dat,
									   false,
									   false);

	return (Node *) const_node;
}

/*
 * function for creating integer node for masking function nodes' arg
 */
static inline Node *
create_integer_node(int value)
{
	Const	   *const_node = makeConst(INT4OID,
									   -1,	/* typmod -1 is OK for all cases */
									   InvalidOid,	/* all cases are
													 * uncollatable types */
									   sizeof(int32),
									   Int32GetDatum(value),
									   false,
									   true);

	return (Node *) const_node;
}

/*
 * function for creating type cast
 */
static inline Node *
create_relabeltype_node(Node *node, int resulttype)
{
	CoercionForm relabelformat;
	RelabelType *typeexpr;

	if (node == NULL)
	{
		return NULL;
	}
	relabelformat = COERCE_EXPLICIT_CAST;
	typeexpr = makeNode(RelabelType);
	if (!typeexpr)
		return NULL;
	typeexpr->arg = (Expr *) node;
	typeexpr->resulttype = resulttype;
	typeexpr->resulttypmod = -1;
	typeexpr->resultcollid = get_typcollation(resulttype);	/* OID of collation */
	typeexpr->relabelformat = relabelformat;
	return (Node *) typeexpr;
}

/*
 * function for creating masking function node
 */
Node *
make_func_node(int funcid, Oid rettype, Node *arg)
{
	FuncExpr   *funcexpr = makeNode(FuncExpr);

	funcexpr->funcid = funcid;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcretset = false;
	funcexpr->funcvariadic = false;
	funcexpr->funcformat = COERCE_EXPLICIT_CALL;

	/*
	 * set masking function arg(the masked column var node)
	 */
	funcexpr->args = lappend(funcexpr->args, arg);
	funcexpr->funccollid = get_typcollation(rettype);
	funcexpr->inputcollid = 100;

	return (Node *) funcexpr;
}

/*
 * function for creating regexp masking function node
 */
Node *
make_func_node_regex(int funcid, Oid rettype, Node *arg, MaskingInfo * maskinfo)
{
	FuncExpr   *funcexpr = makeNode(FuncExpr);

	funcexpr->funcretset = false;
	funcexpr->funcvariadic = false;
	funcexpr->funcformat = COERCE_EXPLICIT_CALL;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcid = funcid;

	/*
	 * set args for regexpmasking
	 */
	funcexpr->args = lappend(funcexpr->args, arg);
	funcexpr->args = lappend(funcexpr->args, create_string_node(maskinfo->regex));
	funcexpr->args = lappend(funcexpr->args, create_string_node(maskinfo->replace_text));
	funcexpr->args = lappend(funcexpr->args, create_integer_node(maskinfo->start));
	funcexpr->args = lappend(funcexpr->args, create_integer_node(maskinfo->end));
	funcexpr->funccollid = get_typcollation(rettype);
	funcexpr->inputcollid = 100;

	return (Node *) funcexpr;
}

static void
polar_process_masking_after_rewrite(List *query_list)
{

	if (!polar_masking_enabled || query_list == NIL)
	{
		return;
	}

	if (next_polar_post_rewrite_query_hook)
	{
		next_polar_post_rewrite_query_hook(query_list);
	}
}

/*
 * Module load function
 */
void
_PG_init(void)
{
	next_polar_post_rewrite_query_hook = polar_post_rewrite_query_hook;
	polar_post_rewrite_query_hook = polar_process_masking_after_rewrite;

	DefineCustomBoolVariable(
							 "polar_masking.polar_masking_enabled",
							 "if polar_masking is enabled",
							 NULL,
							 &polar_masking_enabled,
							 false,
							 PGC_SIGHUP,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL, NULL, NULL);
}

/*
 * Module unload function
 */
void
_PG_fini(void)
{
	/* uninstall hook */
	polar_post_rewrite_query_hook = next_polar_post_rewrite_query_hook;

}
