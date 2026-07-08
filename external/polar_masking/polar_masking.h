/*-------------------------------------------------------------------------
 *
 * polar_masking.h
 *	  Header for polar_masking.
 *
 * IDENTIFICATION
 *	  external/polar_masking/polar_masking.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POLAR_MASKING_H_
#define _POLAR_MASKING_H_

#include "catalog/namespace.h"
#include "commands/sequence.h"

#define MASKING_SCHEMA_NAME "polar_masking"

/*
 * Some definiton for polar_masking_label_tab.
 * The relation is used for saving masking table labels.
 */
#define MASKING_LABEL_TAB_RELNAME "polar_masking_label_tab"
#define Anum_polar_masking_label_tab_labelid 1
#define Anum_polar_masking_label_tab_relid 2
#define Natts_polar_masking_label_tab 2
#define MASKING_LABEL_TAB_RELID_IDXNAME "polar_masking_label_tab_relid_idx"

/*
 * Some definiton for polar_masking_label_col.
 * The relation is used for saving masking column labels.
 */
#define MASKING_LABEL_COL_RELNAME "polar_masking_label_col"
#define Anum_polar_masking_label_col_labelid 1
#define Anum_polar_masking_label_col_relid 2
#define Anum_polar_masking_label_col_colid 3
#define Natts_polar_masking_label_col 3
#define MASKING_LABEL_COL_RELID_COLID_IDXNAME "polar_masking_label_col_relid_colid_idx"

/*
 * Some definiton for polar_masking_policy.
 * The relation is used for saving masking labels and their binding relationships with masking operators.
 */
#define MASKING_POLICY_RELNAME "polar_masking_policy"
#define Anum_polar_masking_policy_labelid 1
#define Anum_polar_masking_policy_name 2
#define Anum_polar_masking_policy_operator 3
#define Natts_polar_masking_policy 3
#define MASKING_POLICY_LABELID_IDXNAME "polar_masking_policy_labelid_idx"

/*
 * Some definiton for polar_masking_policy_regex.
 * The relation is used for saving parameters of regexpmasking masking operators.
 */
#define MASKING_POLICY_REGEX_RELNAME "polar_masking_policy_regex"
#define Anum_polar_masking_policy_regex_labelid 1
#define Anum_polar_masking_policy_regex_start_pos 2
#define Anum_polar_masking_policy_regex_end_pos 3
#define Anum_polar_masking_policy_regex_regex 4
#define Anum_polar_masking_policy_regex_replace_text 5
#define Natts_polar_masking_policy_regex 5
#define MASKING_POLICY_REGEX_LABELID_IDXNAME "polar_masking_policy_regex_labelid_idx"

/*
 * Definiton of sequence and function to get labelid, labelid value: 1 - 2147483647
 */
#define MASKING_LABELID_SEQNAME "polar_masking_labelid_sequence"
#define GetNextMaskingLabelid() ((int32) nextval_internal(get_relname_relid(MASKING_LABELID_SEQNAME, POLAR_MASKING_NAMESPACE), false))

#define InvalidMaskingLabelId 0
#define POLAR_MASKING_NAMESPACE (get_namespace_oid(MASKING_SCHEMA_NAME, true))

/*
 * Definition of functions to get objectid of polar masking relations and
 * indexes.
 */
#define GetPolarMaskingLabelTabRelid() (get_relname_relid(MASKING_LABEL_TAB_RELNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingLabelColRelid() (get_relname_relid(MASKING_LABEL_COL_RELNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingPolicyRelid() (get_relname_relid(MASKING_POLICY_RELNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingPolicyRegexRelid() (get_relname_relid(MASKING_POLICY_REGEX_RELNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingLabelTabRelidIdxid() (get_relname_relid(MASKING_LABEL_TAB_RELID_IDXNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingLabelColRelidColidIdxid() (get_relname_relid(MASKING_LABEL_COL_RELID_COLID_IDXNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingPolicyLabelidIdxid() (get_relname_relid(MASKING_POLICY_LABELID_IDXNAME, POLAR_MASKING_NAMESPACE))
#define GetPolarMaskingPolicyRegexLabelidIdxid() (get_relname_relid(MASKING_POLICY_REGEX_LABELID_IDXNAME, POLAR_MASKING_NAMESPACE))

/*
 * define the enum value of masking operators
 */
enum
{
	MASKING_UNKNOWN, //masking opeator not set
	MASKING_CREDITCARD, //creditcardmasking
	MASKING_BASICEMAIL, //basicemailmasking
	MASKING_FULLEMAIL, //fullemailmasking
	MASKING_ALLDIGITS, //alldigitsmasking
	MASKING_SHUFFLE, //shufflemasking
	MASKING_RANDOM, //randommasking
	MASKING_REGEXP, //regexpmasking
	MASKING_ALL // maskall
};

typedef struct MaskingInfo
{
	Oid			relid;
	AttrNumber	attnum;
	/* InvalidAttrNumber is used for relation policy */
	int16		masking_op;
	/* masking opeartor */

	/*
	 * following are attributes for regexpmasking
	 */
	int			start;
	int			end;
	Datum		regex;
	Datum		replace_text;
}			MaskingInfo;

extern bool check_masking_for_column(MaskingInfo * maskinfo);
extern Oid	get_masking_funcid(int masking_op);

#endif
