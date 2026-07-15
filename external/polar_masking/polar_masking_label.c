/*-------------------------------------------------------------------------
*
* polar_masking_label.c
*       Process definition of masking label and policy.
*
* IDENTIFICATION
*    external/polar_masking/polar_masking_label.c
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "polar_masking.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_FUNCTION_INFO_V1(polar_masking_create_label);
PG_FUNCTION_INFO_V1(polar_masking_drop_label);
PG_FUNCTION_INFO_V1(polar_masking_apply_label_to_table);
PG_FUNCTION_INFO_V1(polar_masking_remove_table_from_label);
PG_FUNCTION_INFO_V1(polar_masking_apply_label_to_column);
PG_FUNCTION_INFO_V1(polar_masking_remove_column_from_label);
PG_FUNCTION_INFO_V1(polar_masking_alter_label_maskingop);
PG_FUNCTION_INFO_V1(polar_masking_alter_label_maskingop_set_regexpmasking);

static int	get_labelid_applied_on_table_or_column(Relation label_tab_rel, Relation label_col_rel, Oid relid, AttrNumber colid);
static Oid	check_schema_table_exist(char *schemaname, char *tabname);
static int	get_masking_labelid_ifexist(Relation policy_rel, char *labelname);
static int16 get_masking_operator_value_by_name(char *masking_op);
static void remove_regexpmasking_parameters(int labelid);
static void set_regexpmasking_parameters(int labelid, int startpos, int endpos, char *regex, char *replacetext);

static const Oid masking_func_arg1_array[] = {TEXTOID};
static const Oid masking_func_arg2_array[] = {TEXTOID, BPCHAROID};
static const Oid masking_regex_func_arg_array[] = {TEXTOID, TEXTOID, TEXTOID, INT4OID, INT4OID};

/*
 * get the labelid if table or the column of table is set a masking label
 */
static int
get_labelid_applied_on_table_or_column(Relation label_tab_rel, Relation label_col_rel, Oid relid, AttrNumber colid)
{
	HeapTuple	tuple;
	ScanKeyData scankey[2];
	SysScanDesc sscan;
	bool		isNull;
	int			labelid = InvalidMaskingLabelId;
	Oid			label_tab_relid_idx_oid;

	ScanKeyInit(&scankey[0],
				Anum_polar_masking_label_tab_relid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (OidIsValid(label_tab_relid_idx_oid = GetPolarMaskingLabelTabRelidIdxid()))
	{
		sscan = systable_beginscan(label_tab_rel, label_tab_relid_idx_oid, true,
								   NULL, 1, scankey);
	}
	else
	{
		sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
								   NULL, 1, scankey);
	}

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_label_tab_labelid,
											 RelationGetDescr(label_tab_rel), &isNull));
		systable_endscan(sscan);
		return labelid;
	}
	systable_endscan(sscan);

	if (colid == InvalidAttrNumber)
	{
		ScanKeyInit(&scankey[0],
					Anum_polar_masking_label_col_relid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));

		sscan = systable_beginscan(label_col_rel, InvalidOid, false,
								   NULL, 1, scankey);
	}
	else
	{
		Oid			label_col_relcolid_idxid;

		ScanKeyInit(&scankey[0],
					Anum_polar_masking_label_col_relid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		ScanKeyInit(&scankey[1],
					Anum_polar_masking_label_col_colid,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(colid));
		if (OidIsValid(label_col_relcolid_idxid = GetPolarMaskingLabelColRelidColidIdxid()))
		{
			sscan = systable_beginscan(label_col_rel, label_col_relcolid_idxid, true,
									   NULL, 2, scankey);
		}
		else
		{
			sscan = systable_beginscan(label_col_rel, InvalidOid, false,
									   NULL, 2, scankey);
		}

	}

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
		labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_label_col_labelid,
											 RelationGetDescr(label_col_rel), &isNull));

	systable_endscan(sscan);

	return labelid;
}

/*
 * check existence of schema and table. If exsits return Oid of the relation,
 * else return invalidOid
 */
static Oid
check_schema_table_exist(char *schemaname, char *tabname)
{
	Oid			schemaid = get_namespace_oid(schemaname, true);
	Oid			relid = InvalidOid;

	if (schemaid == InvalidOid)
	{
		return InvalidOid;
	}
	relid = get_relname_relid(tabname, schemaid);
	return relid;
}

/*
 * Get the masking labelid if labelname exsits in polar_masking_policy
 */
static int
get_masking_labelid_ifexist(Relation policy_rel, char *labelname)
{
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	bool		isNull;
	int			labelid = InvalidMaskingLabelId;

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));

	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
											 RelationGetDescr(policy_rel), &isNull));
	}

	systable_endscan(sscan);
	return labelid;
}

/*
 * Get masking operate value by operator name.
 */
static int16
get_masking_operator_value_by_name(char *opname)
{
	if (!strcasecmp(opname, "creditcardmasking"))
	{
		return MASKING_CREDITCARD;
	}
	else if (!strcasecmp(opname, "basicemailmasking"))
	{
		return MASKING_BASICEMAIL;
	}
	else if (!strcasecmp(opname, "fullemailmasking"))
	{
		return MASKING_FULLEMAIL;
	}
	else if (!strcasecmp(opname, "randommasking"))
	{
		return MASKING_RANDOM;
	}
	else if (!strcasecmp(opname, "alldigitsmasking"))
	{
		return MASKING_ALLDIGITS;
	}
	else if (!strcasecmp(opname, "shufflemasking"))
	{
		return MASKING_SHUFFLE;
	}
	else if (!strcasecmp(opname, "regexpmasking"))
	{
		return MASKING_REGEXP;
	}
	else if (!strcasecmp(opname, "maskall"))
	{
		return MASKING_ALL;
	}
	else if (!strcasecmp(opname, "none"))
	{
		return MASKING_UNKNOWN;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown masking operator name %s", opname)));
}

/*
 * remove regexpmasking parameters in polar_masking_policy_regex
 */
static void
remove_regexpmasking_parameters(int labelid)
{
	Relation	regexp_rel;
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	Oid			regex_rel_oid = GetPolarMaskingPolicyRegexRelid();

	if (!OidIsValid(regex_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	regexp_rel = table_open(regex_rel_oid, ExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_regex_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));

	sscan = systable_beginscan(regexp_rel, GetPolarMaskingPolicyRegexLabelidIdxid(), true,
							   NULL, 1, &scankey);

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		CatalogTupleDelete(regexp_rel, &tuple->t_self);
	}

	systable_endscan(sscan);
	table_close(regexp_rel, ExclusiveLock);
}

/*
 * alter regexpmasking parameters int polar_masking_policy_regex
 */
static void
set_regexpmasking_parameters(int labelid, int startpos, int endpos, char *regex, char *replacetext)
{
	Relation	regexp_rel;
	HeapTuple	tuple;
	HeapTuple	new_tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	Datum		new_record[Natts_polar_masking_policy_regex];
	bool		new_record_nulls[Natts_polar_masking_policy_regex];
	bool		new_record_repls[Natts_polar_masking_policy_regex];
	Oid			regex_rel_oid = GetPolarMaskingPolicyRegexRelid();

	if (!OidIsValid(regex_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));

	regexp_rel = table_open(regex_rel_oid, ExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_regex_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));

	sscan = systable_beginscan(regexp_rel, GetPolarMaskingPolicyRegexLabelidIdxid(), true,
							   NULL, 1, &scankey);

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		new_record[Anum_polar_masking_policy_regex_start_pos - 1] = Int32GetDatum(startpos);
		new_record[Anum_polar_masking_policy_regex_end_pos - 1] = Int32GetDatum(endpos);
		new_record[Anum_polar_masking_policy_regex_regex - 1] = CStringGetTextDatum(regex);
		new_record[Anum_polar_masking_policy_regex_replace_text - 1] = CStringGetTextDatum(replacetext);
		MemSet(new_record_repls, true, sizeof(new_record_repls));
		new_record_repls[Anum_polar_masking_policy_regex_labelid - 1] = false;
		new_tuple = heap_modify_tuple(tuple, RelationGetDescr(regexp_rel), new_record, new_record_nulls, new_record_repls);
		CatalogTupleUpdate(regexp_rel, &tuple->t_self, new_tuple);
	}
	else
	{
		new_record[Anum_polar_masking_policy_regex_labelid - 1] = Int32GetDatum(labelid);
		new_record[Anum_polar_masking_policy_regex_start_pos - 1] = Int32GetDatum(startpos);
		new_record[Anum_polar_masking_policy_regex_end_pos - 1] = Int32GetDatum(endpos);
		new_record[Anum_polar_masking_policy_regex_regex - 1] = CStringGetTextDatum(regex);
		new_record[Anum_polar_masking_policy_regex_replace_text - 1] = CStringGetTextDatum(replacetext);
		tuple = heap_form_tuple(RelationGetDescr(regexp_rel), new_record, new_record_nulls);
		CatalogTupleInsert(regexp_rel, tuple);
	}

	systable_endscan(sscan);
	table_close(regexp_rel, ExclusiveLock);
}

/*
 * create masking label
 */
Datum
polar_masking_create_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	HeapTuple	tuple;
	Datum		new_record[Natts_polar_masking_policy];
	bool		new_record_nulls[Natts_polar_masking_policy];
	char	   *labelname;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();

	if (!OidIsValid(policy_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	if (InvalidMaskingLabelId != get_masking_labelid_ifexist(policy_rel, labelname))
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label name: \"%s\" already exists",
						labelname)));
	}
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	new_record[Anum_polar_masking_policy_labelid - 1] = Int32GetDatum(GetNextMaskingLabelid());
	new_record[Anum_polar_masking_policy_name - 1] = CStringGetTextDatum(labelname);

	tuple = heap_form_tuple(RelationGetDescr(policy_rel), new_record, new_record_nulls);
	CatalogTupleInsert(policy_rel, tuple);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * drop masking label
 */
Datum
polar_masking_drop_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	Relation	label_col_rel;
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	int			labelid;
	bool		isNull;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_col_rel_oid = GetPolarMaskingLabelColRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid) ||
		!OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);
	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_col_rel = table_open(label_col_rel_oid, ExclusiveLock);

	/*
	 * remove label in polar_masking_policy, only one
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));

	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);

	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(label_col_rel, ExclusiveLock);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}
	labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
										 RelationGetDescr(policy_rel), &isNull));
	Assert(!isNull);
	CatalogTupleDelete(policy_rel, &tuple->t_self);
	systable_endscan(sscan);

	/*
	 * remove labels in polar_masking_label_tab
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_label_tab_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));
	sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		CatalogTupleDelete(label_tab_rel, &tuple->t_self);
	}
	systable_endscan(sscan);

	/*
	 * remove labels in polar_masking_label_col
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_label_col_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));
	sscan = systable_beginscan(label_col_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		CatalogTupleDelete(label_col_rel, &tuple->t_self);
	}
	systable_endscan(sscan);

	/*
	 * if exists, remove labels in polar_masking_policy_regex
	 */
	remove_regexpmasking_parameters(labelid);

	table_close(label_col_rel, ExclusiveLock);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * bind one table to a masking label
 */
Datum
polar_masking_apply_label_to_table(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	Relation	label_col_rel;
	HeapTuple	tuple;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	Oid			relid;
	int			labelid;
	Datum		new_record[Natts_polar_masking_label_tab];
	bool		new_record_nulls[Natts_polar_masking_label_tab];
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_col_rel_oid = GetPolarMaskingLabelColRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid) ||
		!OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}

	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_col_rel = table_open(label_col_rel_oid, ExclusiveLock);

	/*
	 * The masking label of table takes effect on all the columns of table. If
	 * a masking label is binded to the table or column of the table, the
	 * table cannot apply a table label on it. To prevent that masking label
	 * of table is different from label on column.
	 */
	if (get_labelid_applied_on_table_or_column(label_tab_rel, label_col_rel, relid, InvalidAttrNumber) != InvalidMaskingLabelId)
	{
		table_close(label_col_rel, ExclusiveLock);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label already be applied on the table or the columns of this table, cannot apply the label now"),
				 errhint("you have to remove the label applied on the table or table's column firstly")));
	}

	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	new_record[Anum_polar_masking_label_tab_labelid - 1] = Int32GetDatum(labelid);
	new_record[Anum_polar_masking_label_tab_relid - 1] = ObjectIdGetDatum(relid);

	tuple = heap_form_tuple(RelationGetDescr(label_tab_rel), new_record, new_record_nulls);
	CatalogTupleInsert(label_tab_rel, tuple);

	table_close(label_col_rel, ExclusiveLock);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * unbind one table from a masking label
 */
Datum
polar_masking_remove_table_from_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	Oid			relid;
	int			labelid;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();
	Oid			label_tab_relid_idxid;

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}
	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_tab_relid_idxid = GetPolarMaskingLabelTabRelidIdxid();

	ScanKeyInit(&scankey,
				Anum_polar_masking_label_tab_relid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (OidIsValid(label_tab_relid_idxid))
	{
		sscan = systable_beginscan(label_tab_rel, label_tab_relid_idxid, true,
								   NULL, 1, &scankey);
	}
	else
	{
		sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
								   NULL, 1, &scankey);
	}

	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label is not be applied on the table")));
	}

	CatalogTupleDelete(label_tab_rel, &tuple->t_self);

	systable_endscan(sscan);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * bind one column to masking label
 */
Datum
polar_masking_apply_label_to_column(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	Relation	label_col_rel;
	HeapTuple	tuple;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	char	   *colname;
	Oid			relid;
	AttrNumber	colid;
	int			labelid;
	Datum		new_record[Natts_polar_masking_label_col];
	bool		new_record_nulls[Natts_polar_masking_label_col];
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_col_rel_oid = GetPolarMaskingLabelColRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid) ||
		!OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	if (PG_ARGISNULL(3))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the column name can not be null")));
	}
	else
	{
		colname = TextDatumGetCString(PG_GETARG_DATUM(3));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	colid = get_attnum(relid, colname);
	if (InvalidAttrNumber == colid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the column \"%s\" does not exist in the relation \"%s.%s\"", colname, schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}
	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_col_rel = table_open(label_col_rel_oid, ExclusiveLock);

	/*
	 * if a masking label is binded to the table, the column of table cannot
	 * be applied a label on it.
	 */
	if (get_labelid_applied_on_table_or_column(label_tab_rel, label_col_rel, relid, colid) != InvalidMaskingLabelId)
	{
		table_close(label_col_rel, ExclusiveLock);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label already be applied on the table or the column, cannot apply the label now"),
				 errhint("you have to remove the label applied on the table or table's column firstly")));
	}

	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	new_record[Anum_polar_masking_label_col_labelid - 1] = Int32GetDatum(labelid);
	new_record[Anum_polar_masking_label_col_relid - 1] = ObjectIdGetDatum(relid);
	new_record[Anum_polar_masking_label_col_colid - 1] = Int16GetDatum(colid);

	tuple = heap_form_tuple(RelationGetDescr(label_col_rel), new_record, new_record_nulls);
	CatalogTupleInsert(label_col_rel, tuple);

	table_close(label_col_rel, ExclusiveLock);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * unbind one column from masking label
 */
Datum
polar_masking_remove_column_from_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_col_rel;
	HeapTuple	tuple;
	ScanKeyData scankey[2];
	SysScanDesc sscan;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	char	   *colname;
	Oid			relid;
	AttrNumber	colid;
	int			labelid;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_col_rel_oid = GetPolarMaskingLabelColRelid();
	Oid			label_col_relcolid_idxid;

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	if (PG_ARGISNULL(3))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the column name can not be null")));
	}
	else
	{
		colname = TextDatumGetCString(PG_GETARG_DATUM(3));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	colid = get_attnum(relid, colname);
	if (InvalidAttrNumber == colid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the column \"%s\" does not exist in the relation \"%s.%s\"", colname, schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);
	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}

	label_col_rel = table_open(label_col_rel_oid, ExclusiveLock);

	ScanKeyInit(&scankey[0],
				Anum_polar_masking_label_col_relid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&scankey[1],
				Anum_polar_masking_label_col_colid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(colid));

	if (OidIsValid(label_col_relcolid_idxid = GetPolarMaskingLabelColRelidColidIdxid()))
	{
		sscan = systable_beginscan(label_col_rel, label_col_relcolid_idxid, true,
								   NULL, 2, scankey);
	}
	else
	{
		sscan = systable_beginscan(label_col_rel, InvalidOid, false,
								   NULL, 2, scankey);
	}

	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(label_col_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label is not be applied on the column")));
	}

	CatalogTupleDelete(label_col_rel, &tuple->t_self);

	systable_endscan(sscan);
	table_close(label_col_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * alter the masking operator of the masking label
 */
Datum
polar_masking_alter_label_maskingop(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	HeapTuple	tuple;
	HeapTuple	new_tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	char	   *opname;
	int16		opval;
	int16		oldopval;
	bool		isNull;
	Datum		new_record[Natts_polar_masking_policy];
	bool		new_record_nulls[Natts_polar_masking_policy];
	bool		new_record_repls[Natts_polar_masking_policy];
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	int			labelid;

	if (!OidIsValid(policy_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the masking operator name can not be null")));
	}
	else
	{
		opname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	opval = get_masking_operator_value_by_name(opname);

	if (opval == MASKING_REGEXP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the function cannnot set regexpmasking"),
				 errhint("please use function \"polar_masking_alter_label_maskingop_set_regexpmasking\" to set regexpmasking")));

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));
	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}

	oldopval = DatumGetInt16(heap_getattr(tuple, Anum_polar_masking_policy_operator,
										  RelationGetDescr(policy_rel), &isNull));
	if (oldopval == opval)
	{
		systable_endscan(sscan);
		table_close(policy_rel, ExclusiveLock);
		elog(WARNING, "the masking operator of masking label \"%s\" is already \"%s\", no need to update", labelname, opname);
		PG_RETURN_NULL();
	}

	labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
										 RelationGetDescr(policy_rel), &isNull));

	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	MemSet(new_record_repls, false, sizeof(new_record_repls));

	new_record[Anum_polar_masking_policy_operator - 1] = Int16GetDatum(opval);
	new_record_repls[Anum_polar_masking_policy_operator - 1] = true;

	new_tuple = heap_modify_tuple(tuple, RelationGetDescr(policy_rel), new_record, new_record_nulls, new_record_repls);
	CatalogTupleUpdate(policy_rel, &tuple->t_self, new_tuple);

	systable_endscan(sscan);

	if (oldopval == MASKING_REGEXP)
	{
		remove_regexpmasking_parameters(labelid);
	}

	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * alter the masking operator to regexpmasking
 */
Datum
polar_masking_alter_label_maskingop_set_regexpmasking(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	HeapTuple	tuple;
	HeapTuple	new_tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	int			labelid;
	int16		oldopval;
	bool		isNull;
	int			startpos;
	int			endpos;
	char	   *regex;
	char	   *replacetext;
	Datum		new_record[Natts_polar_masking_policy];
	bool		new_record_nulls[Natts_polar_masking_policy];
	bool		new_record_repls[Natts_polar_masking_policy];
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();

	if (!OidIsValid(policy_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the regexpmasking start postion can not be null")));
	}
	else
	{
		startpos = Int32GetDatum(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the regexpmasking end postion can not be null")));
	}
	else
	{
		endpos = Int32GetDatum(PG_GETARG_DATUM(2));
	}

	if (startpos < 0 || endpos < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the start or end position should >= 0")));
	}

	if (startpos > endpos && endpos != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the start position should not greater than end position(%d > %d) in regexpmasking", startpos, endpos)));
	}


	if (PG_ARGISNULL(3))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the masking operator name can not be null")));
	}
	else
	{
		regex = TextDatumGetCString(PG_GETARG_DATUM(3));
	}

	if (PG_ARGISNULL(4))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		replacetext = TextDatumGetCString(PG_GETARG_DATUM(4));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));
	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}

	oldopval = DatumGetInt16(heap_getattr(tuple, Anum_polar_masking_policy_operator,
										  RelationGetDescr(policy_rel), &isNull));
	labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
										 RelationGetDescr(policy_rel), &isNull));

	if (oldopval != MASKING_REGEXP)
	{
		MemSet(new_record, 0, sizeof(new_record));
		MemSet(new_record_nulls, false, sizeof(new_record_nulls));
		MemSet(new_record_repls, false, sizeof(new_record_repls));

		new_record[Anum_polar_masking_policy_operator - 1] = Int16GetDatum(MASKING_REGEXP);
		new_record_repls[Anum_polar_masking_policy_operator - 1] = true;

		new_tuple = heap_modify_tuple(tuple, RelationGetDescr(policy_rel), new_record, new_record_nulls, new_record_repls);
		CatalogTupleUpdate(policy_rel, &tuple->t_self, new_tuple);

	}

	systable_endscan(sscan);

	set_regexpmasking_parameters(labelid, startpos, endpos, regex, replacetext);

	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * check the masking operator for column
 */
bool
check_masking_for_column(MaskingInfo * maskinfo)
{
	HeapTuple	tuple;
	bool		IsNull;
	int			labelid;
	Relation	label_tab_rel;
	Relation	label_col_rel;
	Relation	policy_rel;
	SysScanDesc scandesc;
	ScanKeyData scankey;
	Oid			policy_rel_oid;
	Oid			label_col_rel_oid;
	Oid			label_tab_rel_oid;
	Oid			regex_rel_oid;
	Oid			policy_labelid_idxid;

	if (maskinfo->relid == InvalidOid)
	{
		return false;
	}

	policy_rel_oid = GetPolarMaskingPolicyRelid();
	label_col_rel_oid = GetPolarMaskingLabelColRelid();
	label_tab_rel_oid = GetPolarMaskingLabelTabRelid();
	regex_rel_oid = GetPolarMaskingPolicyRegexRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid) ||
		!OidIsValid(label_tab_rel_oid) || !OidIsValid(regex_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	label_tab_rel = table_open(label_tab_rel_oid, RowExclusiveLock);
	label_col_rel = table_open(label_col_rel_oid, RowExclusiveLock);
	policy_rel = table_open(policy_rel_oid, RowExclusiveLock);

	labelid = get_labelid_applied_on_table_or_column(label_tab_rel, label_col_rel, maskinfo->relid, maskinfo->attnum);

	if (labelid == InvalidMaskingLabelId)
	{
		table_close(policy_rel, RowExclusiveLock);
		table_close(label_col_rel, RowExclusiveLock);
		table_close(label_tab_rel, RowExclusiveLock);
		return false;
	}

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));

	if (OidIsValid(policy_labelid_idxid = GetPolarMaskingPolicyLabelidIdxid()))
	{
		scandesc = systable_beginscan(policy_rel, policy_labelid_idxid, true,
									  NULL, 1, &scankey);
	}
	else
	{
		scandesc = systable_beginscan(policy_rel, InvalidOid, false,
									  NULL, 1, &scankey);
	}

	tuple = systable_getnext(scandesc);

	Assert(HeapTupleIsValid(tuple));

	maskinfo->masking_op = DatumGetInt16(heap_getattr(tuple, Anum_polar_masking_policy_operator, RelationGetDescr(policy_rel), &IsNull));

	if (maskinfo->masking_op == MASKING_UNKNOWN)
	{
		systable_endscan(scandesc);
		table_close(policy_rel, RowExclusiveLock);
		table_close(label_col_rel, RowExclusiveLock);
		table_close(label_tab_rel, RowExclusiveLock);
		return false;
	}

	systable_endscan(scandesc);

	if (maskinfo->masking_op == MASKING_REGEXP)
	{
		Relation	regex_rel = table_open(regex_rel_oid, RowExclusiveLock);
		Oid			regex_idxid;

		ScanKeyInit(&scankey,
					Anum_polar_masking_policy_regex_labelid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(labelid));

		if (OidIsValid(regex_idxid = GetPolarMaskingPolicyRegexLabelidIdxid()))
		{
			scandesc = systable_beginscan(regex_rel, regex_idxid, true,
										  NULL, 1, &scankey);
		}
		else
		{
			scandesc = systable_beginscan(regex_rel, InvalidOid, false,
										  NULL, 1, &scankey);
		}

		tuple = systable_getnext(scandesc);
		Assert(HeapTupleIsValid(tuple));

		maskinfo->regex = heap_getattr(tuple, Anum_polar_masking_policy_regex_regex, RelationGetDescr(regex_rel), &IsNull);
		maskinfo->start = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_regex_start_pos, RelationGetDescr(regex_rel), &IsNull));
		maskinfo->end = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_regex_end_pos, RelationGetDescr(regex_rel), &IsNull));
		maskinfo->replace_text = heap_getattr(tuple, Anum_polar_masking_policy_regex_replace_text, RelationGetDescr(regex_rel), &IsNull);
		systable_endscan(scandesc);
		table_close(regex_rel, RowExclusiveLock);
	}

	table_close(policy_rel, RowExclusiveLock);
	table_close(label_col_rel, RowExclusiveLock);
	table_close(label_tab_rel, RowExclusiveLock);
	return true;
}

/*
 * get masking function Oid by function name and args
 */
static Oid
get_function_id(int arglen, const Oid argarr[], const char *funcname)
{
	CatCList   *catlist = NULL;
	Oid			funcid = InvalidOid;

	catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

	if (catlist != NULL)
	{
		for (int i = 0; i < catlist->n_members; ++i)
		{
			HeapTuple	proctup = &catlist->members[i]->tuple;
			Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
			bool		samearg = true;;
			if (procform)
			{
				/*
				 * check function namespace
				 */
				if (procform->pronamespace != POLAR_MASKING_NAMESPACE)
					continue;

				/*
				 * check function arg length and type
				 */
				if (arglen != procform->pronargs)
					continue;

				for (int i = 0; i < arglen; i++)
				{
					if (argarr[i] != procform->proargtypes.values[i])
					{
						samearg = false;
						break;
					}
				}
				if (samearg)
				{
					funcid = procform->oid;
					break;
				}
			}
		}
	}

	ReleaseSysCacheList(catlist);
	return funcid;
}

/*
 * get the masking function Oid by masking operator
 */
Oid
get_masking_funcid(int masking_op)
{
	Oid			funcid = InvalidOid;

	Assert(masking_op != MASKING_UNKNOWN);

	switch (masking_op)
	{
		case MASKING_CREDITCARD:
			funcid = get_function_id(2, masking_func_arg2_array, "creditcardmasking");
			break;
		case MASKING_BASICEMAIL:
			funcid = get_function_id(2, masking_func_arg2_array, "basicemailmasking");
			break;
		case MASKING_FULLEMAIL:
			funcid = get_function_id(2, masking_func_arg2_array, "fullemailmasking");
			break;
		case MASKING_ALLDIGITS:
			funcid = get_function_id(2, masking_func_arg2_array, "alldigitsmasking");
			break;
		case MASKING_SHUFFLE:
			funcid = get_function_id(1, masking_func_arg1_array, "shufflemasking");
			break;
		case MASKING_RANDOM:
			funcid = get_function_id(1, masking_func_arg1_array, "randommasking");
			break;
		case MASKING_REGEXP:
			funcid = get_function_id(5, masking_regex_func_arg_array, "regexpmasking");
			break;
		case MASKING_ALL:
			funcid = get_function_id(2, masking_func_arg2_array, "maskall");
			break;
		default:
			elog(ERROR, "unknown masking operator: %d", masking_op);
			break;
	}
	return funcid;
}
