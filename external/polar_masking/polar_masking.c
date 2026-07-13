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
#include "utils/fmgroids.h"
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
static Node *make_func_node(int funcid, Oid rettype, Node *arg);
static Node *make_func_node_regex(int funcid, Oid rettype, Node *arg, MaskingInfo * maskinfo);
static Node *create_masking_func_node(Var *var, int masking_op);
static Node *create_masking_func_node_regex(Var *var, MaskingInfo * maskinfo);
Node	   *create_masking_var_node(MaskingInfo * maskinfo, Var *var);

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
static Node *
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
static Node *
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

/*
 * function for transforming Var node to masking Var node
 */
static Node *
create_masking_func_node(Var *var, int masking_op)
{
	Node	   *masking_func_node = NULL;
	Oid			funcid = get_masking_funcid(masking_op);

	if (funcid == InvalidOid)
		return masking_func_node;

	/*
	 * create masking function node with var according to var type, masking
	 * func nodes have to be casted to the original type of Var nodes
	 */
	switch (var->vartype)
	{
		case TEXTOID:
			{
				masking_func_node = make_func_node(funcid, TEXTOID, (Node *) var);
			}
			break;
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
			{
				masking_func_node = make_func_node(funcid, TEXTOID, (Node *) var);
				if (masking_func_node != NULL)
				{
					Node	   *cast_node = create_relabeltype_node(masking_func_node, var->vartype);

					if (cast_node != NULL)
					{
						masking_func_node = cast_node;
					}
				}
			}
			break;
		case NAMEOID:
			{
				Node	   *text_node = make_func_node(F_NAME_TEXT, TEXTOID, (Node *) var);

				if (text_node != NULL)
					masking_func_node = make_func_node(funcid, TEXTOID, text_node);
				if (masking_func_node != NULL)
				{
					Node	   *cast_func = make_func_node(F_TEXT_NAME, NAMEOID, masking_func_node);

					if (cast_func != NULL)
					{
						masking_func_node = cast_func;
					}
				}
			}
			break;
		default:
			break;
	}
	return masking_func_node;
}

/*
 * function for transforming Var node to regex masking Var node
 */
static Node *
create_masking_func_node_regex(Var *var, MaskingInfo * maskinfo)
{
	Node	   *masking_func_node = NULL;
	Oid			funcid = get_masking_funcid(maskinfo->masking_op);

	if (funcid == InvalidOid)
		return masking_func_node;

	if (var == NULL)
	{
		return NULL;
	}

	/*
	 * create masking function node with var according to var type, masking
	 * func nodes have to be casted to the original type of Var nodes
	 */
	switch (var->vartype)
	{
		case TEXTOID:
			{
				masking_func_node = make_func_node_regex(funcid, TEXTOID, (Node *) var, maskinfo);
			}
			break;
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
			{
				masking_func_node = make_func_node_regex(funcid, TEXTOID, (Node *) var, maskinfo);
				if (masking_func_node != NULL)
				{
					Node	   *cast_node = create_relabeltype_node(masking_func_node, var->vartype);

					if (cast_node != NULL)
					{
						masking_func_node = cast_node;
					}
				}
			}
			break;
		case NAMEOID:
			{
				Node	   *text_node = make_func_node(F_NAME_TEXT, TEXTOID, (Node *) var);

				masking_func_node = make_func_node_regex(funcid, TEXTOID, text_node, maskinfo);
				if (masking_func_node != NULL)
				{
					Node	   *cast_func = make_func_node(F_TEXT_NAME, NAMEOID, masking_func_node);

					if (cast_func != NULL)
					{
						masking_func_node = cast_func;
					}
				}
			}
			break;
		default:
			break;
	}
	return masking_func_node;
}

/*
 * function for create masking Var node according to masking operator
 */
Node *
create_masking_var_node(MaskingInfo * maskinfo, Var *var)
{
	Node	   *masked_node = NULL;

	switch (maskinfo->masking_op)
	{
		case MASKING_CREDITCARD:
		case MASKING_BASICEMAIL:
		case MASKING_FULLEMAIL:
		case MASKING_SHUFFLE:
		case MASKING_ALLDIGITS:
		case MASKING_RANDOM:
		case MASKING_ALL:
			{
				masked_node = create_masking_func_node(var, maskinfo->masking_op);
			}
			break;
		case MASKING_REGEXP:
			{
				masked_node = create_masking_func_node_regex(var, maskinfo);
			}
			break;
		default:
			{
				/*
				 * should not happen
				 */
				elog(ERROR, "unknown masking operator: %d", maskinfo->masking_op);
			}
			break;
	}

	/*
	 * Var node is not masked, return the original Var
	 */
	if (masked_node == NULL)
	{
		masked_node = (Node *) var;
	}
	return masked_node;
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
