/*-------------------------------------------------------------------------
 *
 * mongo_query.c
 *
 * Function definitions for sending queries to MongoDB. These functions assume
 * that queries are sent through the official MongoDB C driver, and apply query
 * optimizations to reduce the amount of data fetched from the driver.
 *
 * Copyright (c) 2012, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mongo_fdw.h"

#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/relation.h"
#include "optimizer/var.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"


/* Local functions forward declarations */
static Expr * FindArgumentOfType(List *argumentList, NodeTag argumentType);
static char * MongoOperatorName(const char *operatorName);
static List * EqualityOperatorList(List *operatorList);
static List * UniqueColumnList(List *operatorList);
static List * ColumnOperatorList(Var *column, List *operatorList);
static void AppendConstantValue(bson *queryDocument, const char *keyName,
								const bson_type toBsonType, Const *constant);


/*
 * ApplicableOpExpressionList walks over all filter clauses that relate to this
 * foreign table, and chooses applicable clauses that we know we can translate
 * into Mongo queries. Currently, these clauses include comparison expressions
 * that have a column and a constant as arguments. For example, "o_orderdate >=
 * date '1994-01-01' + interval '1' year" is an applicable expression.
 */
List *
ApplicableOpExpressionList(RelOptInfo *baserel)
{
	List *opExpressionList = NIL;
	List *restrictInfoList = baserel->baserestrictinfo;
	ListCell *restrictInfoCell = NULL;

	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Expr *expression = restrictInfo->clause;
		NodeTag expressionType = 0;

		OpExpr *opExpression = NULL;
		char *operatorName = NULL;
		char *mongoOperatorName = NULL;
		List *argumentList = NIL;
		Var *column = NULL;
		Const *constant = NULL;
		bool equalsOperator = false;
		bool constantIsArray = false;

		/* we only support operator expressions */
		expressionType = nodeTag(expression);
		if (expressionType != T_OpExpr)
		{
			continue;
		}

		opExpression = (OpExpr *) expression;
		operatorName = get_opname(opExpression->opno);

		/* we only support =, <, >, <=, >=, and <> operators */
		if (strncmp(operatorName, EQUALITY_OPERATOR_NAME, NAMEDATALEN) == 0)
		{
			equalsOperator = true;
		}

		mongoOperatorName = MongoOperatorName(operatorName);
		if (!equalsOperator && mongoOperatorName == NULL)
		{
			ereport(INFO, (errmsg_internal("Ignoring unsupported operator %s", 
							               operatorName)));
			continue;
		}

		/*
		 * We only support simple binary operators that compare a column against
		 * a constant. If the expression is a tree, we don't recurse into it.
		 */
		argumentList = opExpression->args;
		column = (Var *) FindArgumentOfType(argumentList, T_Var);
		constant = (Const *) FindArgumentOfType(argumentList, T_Const);

		/*
		 * We don't push down operators where the constant is an array, since
		 * conditional operators for arrays in MongoDB aren't properly defined.
		 * For example, {similar_products : [ "B0009S4IJW", "6301964144" ]}
		 * finds results that are equal to the array, but {similar_products:
		 * {$gte: [ "B0009S4IJW", "6301964144" ]}} returns an empty set.
		 */
		if (constant != NULL)
		{
			Oid constantArrayTypeId = get_element_type(constant->consttype);
			if (constantArrayTypeId != InvalidOid)
			{
				constantIsArray = true;
				ereport(INFO, (errmsg_internal("Ignoring %s expression with array",
											   operatorName)));
			}
		}
		else
		{
			ereport(INFO, (errmsg_internal("Ignoring %s expression without a constant",
										   operatorName)));
		}

		if (column != NULL && constant != NULL && !constantIsArray)
		{
			opExpressionList = lappend(opExpressionList, opExpression);
		}
	}

	return opExpressionList;
}


/*
 * FindArgumentOfType walks over the given argument list, looks for an argument
 * with the given type, and returns the argument if it is found.
 */
static Expr *
FindArgumentOfType(List *argumentList, NodeTag argumentType)
{
	Expr *foundArgument = NULL;
	ListCell *argumentCell = NULL;

	foreach(argumentCell, argumentList)
	{
		Expr *argument = (Expr *) lfirst(argumentCell);
		if (nodeTag(argument) == argumentType)
		{
			foundArgument = argument;
			break;
		}
	}

	return foundArgument;
}


/*
 * QueryDocument takes in the applicable operator expressions for a relation and
 * converts these expressions into equivalent queries in MongoDB. For now, this
 * function can only transform simple comparison expressions, and returns these
 * transformed expressions in a BSON document. For example, simple expressions
 * "l_shipdate >= date '1994-01-01' AND l_shipdate < date '1995-01-01'" become
 * "l_shipdate: { $gte: new Date(757382400000), $lt: new Date(788918400000) }".
 */
bson *
QueryDocument(Oid relationId, List *opExpressionList, MongoFdwOptions* mongoFdwOptions,
		      struct HTAB *columnMappingHash)
{
	List *equalityOperatorList = NIL;
	List *comparisonOperatorList = NIL;
	List *columnList = NIL;
	ListCell *equalityOperatorCell = NULL;
	ListCell *columnCell = NULL;
	bson *queryDocument = NULL;
	int documentStatus = BSON_OK;
	char *prefix = "parent.";
	char *oidGeneratedKeySuffix = ".generated";
	int prefix_len = strlen(prefix);
	StringInfo columnNameInfo = NULL;

	queryDocument = bson_create();
	bson_init(queryDocument);

	/*
	 * We distinguish between equality expressions and others since we need to
	 * insert the latter (<, >, <=, >=, <>) as separate sub-documents into the
	 * BSON query object.
	 */
	equalityOperatorList = EqualityOperatorList(opExpressionList);
	comparisonOperatorList = list_difference(opExpressionList, equalityOperatorList);

	/* append equality expressions to the query */
	foreach(equalityOperatorCell, equalityOperatorList)
	{
		OpExpr *equalityOperator = (OpExpr *) lfirst(equalityOperatorCell);
		Oid columnId = InvalidOid;
		char *columnName = NULL;
		int suffixLen = strlen(oidGeneratedKeySuffix);
		int columnNameLen = 0;
		char *dot = NULL;
		bson_type toBsonType = -1;
		bool handleFound = false;
		ColumnMapping *columnMapping = NULL;

		List *argumentList = equalityOperator->args;
		Var *column = (Var *) FindArgumentOfType(argumentList, T_Var);
		Const *constant = (Const *) FindArgumentOfType(argumentList, T_Const);

		columnId = column->varattno;
		columnName = get_relid_attribute_name(relationId, columnId);
		void *hashKey = (void *) columnName;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey, HASH_FIND,
													  &handleFound);
		if (mongoFdwOptions->fieldName && *mongoFdwOptions->fieldName != '\0') {
			if (strncmp(columnName, prefix, prefix_len) == 0)
			{
				columnName = columnName + prefix_len; 
			}
			else
			{
				columnNameInfo = makeStringInfo();
				appendStringInfo(columnNameInfo, "%s.%s", mongoFdwOptions->fieldName,
								 columnName);
				columnName = columnNameInfo->data;
			}
		}

		columnNameLen = strlen(columnName);
		if (columnNameLen > suffixLen &&
			strcmp(columnName + (columnNameLen - suffixLen), oidGeneratedKeySuffix) == 0)
		{
			dot = columnName + (columnNameLen - suffixLen);
			/* Chop .generated off of the keyname to get the oid name */
			*dot = '\0';
			toBsonType = BSON_OID;
		}
		else if(columnMapping)
		{
			toBsonType = columnMapping->columnBsonType;
		}

		AppendConstantValue(queryDocument, columnName, toBsonType, constant);

		if (dot) {
			*dot = '.';
		}
	}

	/*
	 * For comparison expressions, we need to group them by their columns and
	 * append all expressions that correspond to a column as one sub-document.
	 * Otherwise, even when we have two expressions to define the upper- and
	 * lower-bound of a range, Mongo uses only one of these expressions during
	 * an index search.
	 */
	columnList = UniqueColumnList(comparisonOperatorList);

	/* append comparison expressions, grouped by columns, to the query */
	foreach(columnCell, columnList)
	{
		Var *column = (Var *) lfirst(columnCell);
		Oid columnId = InvalidOid;
		char *columnName = NULL;
		List *columnOperatorList = NIL;
		ListCell *columnOperatorCell = NULL;
		int suffixLen = strlen(oidGeneratedKeySuffix);
		int columnNameLen = 0;
		char *dot = NULL;
		bson_type toBsonType = -1;
		ColumnMapping *columnMapping = NULL;
		bool handleFound = false;

		columnId = column->varattno;
		columnName = get_relid_attribute_name(relationId, columnId);
		void *hashKey = (void *) columnName;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey, HASH_FIND,
													  &handleFound);
		if (mongoFdwOptions->fieldName && *mongoFdwOptions->fieldName != '\0') {
			if (strncmp(columnName, prefix, prefix_len) == 0)
			{
				columnName = columnName + prefix_len; 
			}
			else
			{
				columnNameInfo = makeStringInfo();
				appendStringInfo(columnNameInfo, "%s.%s", mongoFdwOptions->fieldName,
								 columnName);
				columnName = columnNameInfo->data;
			}
		}

		columnNameLen = strlen(columnName);
		if (columnNameLen > suffixLen &&
			strcmp(columnName + (columnNameLen - suffixLen), oidGeneratedKeySuffix) == 0)
		{
			dot = columnName + (columnNameLen - suffixLen);
			/* Chop .generated off of the keyname to get the oid name */
			*dot = '\0';
			toBsonType = BSON_OID;
		}
		else if(columnMapping)
		{
			toBsonType = columnMapping->columnBsonType;
		}

		/* find all expressions that correspond to the column */
		columnOperatorList = ColumnOperatorList(column, comparisonOperatorList);

		/* for comparison expressions, start a sub-document */
		bson_append_start_object(queryDocument, columnName);

		if (dot) {
			*dot = '.';
		}

		foreach(columnOperatorCell, columnOperatorList)
		{
			OpExpr *columnOperator = (OpExpr *) lfirst(columnOperatorCell);
			char *operatorName = NULL;
			char *mongoOperatorName = NULL;

			List *argumentList = columnOperator->args;
			Const *constant = (Const *) FindArgumentOfType(argumentList, T_Const);

			operatorName = get_opname(columnOperator->opno);
			mongoOperatorName = MongoOperatorName(operatorName);

			AppendConstantValue(queryDocument, mongoOperatorName, toBsonType, constant);
		}

		bson_append_finish_object(queryDocument);
	}

	documentStatus = bson_finish(queryDocument);
	if (documentStatus != BSON_OK)
	{
		ereport(ERROR, (errmsg("could not create document for query"),
						errhint("BSON error: %s", queryDocument->errstr)));
	}

	return queryDocument;
}


/*
 * MongoOperatorName takes in the given PostgreSQL comparison operator name, and
 * returns its equivalent in MongoDB.
 */
static char *
MongoOperatorName(const char *operatorName)
{
	const char *mongoOperatorName = NULL;
	const int32 nameCount = 5;
	static const char *nameMappings[][2] = { { "<", "$lt" },
											 { ">", "$gt" },
											 { "<=", "$lte" },
											 { ">=", "$gte" },
											 { "<>", "$ne" } };

	int32 nameIndex = 0;
	for (nameIndex = 0; nameIndex < nameCount; nameIndex++)
	{
		const char *pgOperatorName = nameMappings[nameIndex][0];
		if (strncmp(pgOperatorName, operatorName, NAMEDATALEN) == 0)
		{
			mongoOperatorName = nameMappings[nameIndex][1];
			break;
		}
	}

	return (char *) mongoOperatorName;
}


/*
 * EqualityOperatorList finds the equality (=) operators in the given list, and
 * returns these operators in a new list.
 */
static List *
EqualityOperatorList(List *operatorList)
{
	List *equalityOperatorList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		char *operatorName = NULL;

		operatorName = get_opname(operator->opno);
		if (strncmp(operatorName, EQUALITY_OPERATOR_NAME, NAMEDATALEN) == 0)
		{
			equalityOperatorList = lappend(equalityOperatorList, operator);
		}
	}

	return equalityOperatorList;
}


/*
 * UniqueColumnList walks over the given operator list, and extracts the column
 * argument in each operator. The function then de-duplicates extracted columns,
 * and returns them in a new list.
 */
static List *
UniqueColumnList(List *operatorList)
{
	List *uniqueColumnList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		List *argumentList = operator->args;
		Var *column = (Var *) FindArgumentOfType(argumentList, T_Var);

		/* list membership is determined via column's equal() function */
		uniqueColumnList = list_append_unique(uniqueColumnList, column);
	}

	return uniqueColumnList;
}


/*
 * ColumnOperatorList finds all expressions that correspond to the given column,
 * and returns them in a new list.
 */
static List *
ColumnOperatorList(Var *column, List *operatorList)
{
	List *columnOperatorList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		List *argumentList = operator->args;

		Var *foundColumn = (Var *) FindArgumentOfType(argumentList, T_Var);
		if (equal(column, foundColumn))
		{
			columnOperatorList = lappend(columnOperatorList, operator);
		}
	}

	return columnOperatorList;
}

static bool
AppendBsonInt(bson *queryDocument, const char *keyName,
			  const bson_type toBsonType, int value)
{
	bool appended = false;
	switch(toBsonType)
	{
		case BSON_STRING:
		{
			char *result = palloc0(22 * sizeof(char));
			snprintf(result, 22, "%d", value);
			bson_append_string(queryDocument, keyName, result);
			appended = true;
			break;
		}
		case BSON_DATE:
		{
			bson_append_date(queryDocument, keyName, value);
			appended = true;
			break;
		}
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		default:
		{
			bson_append_int(queryDocument, keyName, value);
			appended = true;
			break;
		}
	}
	return appended;
}

static bool
AppendBsonLong(bson *queryDocument, const char *keyName,
			  const bson_type toBsonType, long value)
{
	bool appended = false;
	switch(toBsonType)
	{
		case BSON_STRING:
		{
			char *result = palloc0(22 * sizeof(char));
			snprintf(result, 22, "%ld", value);
			bson_append_string(queryDocument, keyName, result);
			appended = true;
			break;
		}
		case BSON_DATE:
		{
			bson_append_date(queryDocument, keyName, value);
			appended = true;
			break;
		}
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		default:
		{
			bson_append_long(queryDocument, keyName, value);
			appended = true;
			break;
		}
	}
	return appended;
}

static bool
AppendBsonDouble(bson *queryDocument, const char *keyName,
			  const bson_type toBsonType, double value)
{
	bool appended = false;
	switch(toBsonType)
	{
		case BSON_STRING:
		{
			char *result = palloc0(22 * sizeof(char));
			snprintf(result, 22, "%g", value);
			bson_append_string(queryDocument, keyName, result);
			appended = true;
			break;
		}
		case BSON_DATE:
		{
			bson_append_date(queryDocument, keyName, (long) value);
			appended = true;
			break;
		}
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		default:
		{
			bson_append_double(queryDocument, keyName, value);
			appended = true;
			break;
		}
	}
	return appended;
}

static bool
AppendBsonString(bson *queryDocument, const char *keyName,
			     const bson_type toBsonType, char *value)
{
	bool appended = false;
	switch(toBsonType)
	{
		case BSON_INT:
		case BSON_LONG:
		{
			long parsed = 0;
			if(ParseLong(value, &parsed))
			{
				bson_append_long(queryDocument, keyName, parsed);
				appended = true;
			}
			break;
		}
		case BSON_DOUBLE:
		{
			double parsed = 0.0;
			if(ParseDouble(value, &parsed))
			{
				bson_append_double(queryDocument, keyName, parsed);
				appended = true;
			}
			break;
		}
		case BSON_OID:
		{
			bson_oid_t bsonObjectId;
			memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));
			bson_oid_from_string(&bsonObjectId, value);
			bson_append_oid(queryDocument, keyName, &bsonObjectId);
			appended = true;
			break;
		}
		case BSON_BOOL:
		case BSON_STRING:
		default:
		{
			bson_append_string(queryDocument, keyName, value);
			appended = true;
			break;
		}
	}
	return appended;
}

static bool
AppendBsonDate(bson *queryDocument, const char *keyName,
			   const bson_type toBsonType, long valueMilliSecs)
{
	bool appended = false;
	switch(toBsonType)
	{
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		{
			long valueSecs = valueMilliSecs / 1000;
			ereport(INFO, (errmsg("Appending date as long %s: %ld", keyName, valueSecs)));
			bson_append_long(queryDocument, keyName, valueSecs);
			appended = true;
			break;
		}
		case BSON_OID:
		{
			/* Generate an oid with a generated time at the time we're querying */
			bson_oid_t oid;
			time_t t = valueMilliSecs / 1000;
			bson_big_endian32(&oid.ints[0], &t);
			oid.ints[1] = 0;
			oid.ints[2] = 0;
			bson_append_oid(queryDocument, keyName, &oid);
			appended = true;
			break;
		}
		case BSON_DATE:
		default:
		{
			ereport(INFO, (errmsg("Appending date as date %s: %ld", keyName, valueMilliSecs)));
			bson_append_date(queryDocument, keyName, valueMilliSecs);
			appended = true;
			break;
		}
	}
	return appended;
}

/*
 * AppendConstantValue appends to the query document the key name and constant
 * value. The function translates the constant value from its PostgreSQL type to
 * its MongoDB equivalent.
 */
static void
AppendConstantValue(bson *queryDocument, const char *keyName,
					const bson_type toBsonType, Const *constant)
{
	Datum constantValue = constant->constvalue;
	Oid constantTypeId = constant->consttype;

	bool constantNull = constant->constisnull;
	if (constantNull)
	{
		bson_append_null(queryDocument, keyName);
		return;
	}

	switch(constantTypeId)
	{
		case INT2OID:
		{
			int16 value = DatumGetInt16(constantValue);
			AppendBsonInt(queryDocument, keyName, toBsonType, (int) value);
			break;
		}
		case INT4OID:
		{
			int32 value = DatumGetInt32(constantValue);
			AppendBsonInt(queryDocument, keyName, toBsonType, value);
			break;
		}
		case INT8OID:
		{
			int64 value = DatumGetInt64(constantValue);
			AppendBsonLong(queryDocument, keyName, toBsonType, value);
			break;
		}
		case FLOAT4OID:
		{
			float4 value = DatumGetFloat4(constantValue);
			AppendBsonDouble(queryDocument, keyName, toBsonType, (double) value);
			break;
		}
		case FLOAT8OID:
		{
			float8 value = DatumGetFloat8(constantValue);
			AppendBsonDouble(queryDocument, keyName, toBsonType, value);
			break;
		}
		case NUMERICOID:
		{
			Datum valueDatum = DirectFunctionCall1(numeric_float8, constantValue);
			float8 value = DatumGetFloat8(valueDatum);
			AppendBsonDouble(queryDocument, keyName, toBsonType, value);
			break;
		}
		case BOOLOID:
		{
			bool value = DatumGetBool(constantValue);
			bson_append_int(queryDocument, keyName, (int) value);
			break;
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			char *outputString = NULL;
			Oid outputFunctionId = InvalidOid;
			bool typeVarLength = false;

			getTypeOutputInfo(constantTypeId, &outputFunctionId, &typeVarLength);
			outputString = OidOutputFunctionCall(outputFunctionId, constantValue);

			AppendBsonString(queryDocument, keyName, toBsonType, outputString);
			break;
		}
	    case NAMEOID:
		{
			char *outputString = NULL;
			Oid outputFunctionId = InvalidOid;
			bool typeVarLength = false;
			bson_oid_t bsonObjectId;
			memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));

			getTypeOutputInfo(constantTypeId, &outputFunctionId, &typeVarLength);
			outputString = OidOutputFunctionCall(outputFunctionId, constantValue);
			bson_oid_from_string(&bsonObjectId, outputString);

			bson_append_oid(queryDocument, keyName, &bsonObjectId);
			break;
		}
		case DATEOID:
		{
			Datum valueDatum = DirectFunctionCall1(date_timestamp, constantValue);
			Timestamp valueTimestamp = DatumGetTimestamp(valueDatum);
			int64 valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
			int64 valueMilliSecs = valueMicroSecs / 1000;
			AppendBsonDate(queryDocument, keyName, toBsonType, valueMilliSecs);
			break;
		}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			Timestamp valueTimestamp = DatumGetTimestamp(constantValue);
			int64 valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
			int64 valueMilliSecs = valueMicroSecs / 1000;
			AppendBsonDate(queryDocument, keyName, toBsonType, valueMilliSecs);
			break;
		}
		default:
		{
			/*
			 * We currently error out on other data types. Some types such as
			 * byte arrays are easy to add, but they need testing. Other types
			 * such as money or inet, do not have equivalents in MongoDB.
			 */
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert constant value to BSON value"),
							errhint("Constant value data type: %u", constantTypeId)));
			break;
		}
	}
}


/*
 * ColumnList takes in the planner's information about this foreign table. The
 * function then finds all columns needed for query execution, including those
 * used in projections, joins, and filter clauses, de-duplicates these columns,
 * and returns them in a new list.
 */
List *
ColumnList(RelOptInfo *baserel)
{
	List *columnList = NIL;
	List *neededColumnList = NIL;
	AttrNumber columnIndex = 1;
	AttrNumber columnCount = baserel->max_attr;
	List *targetColumnList = baserel->reltargetlist;
	List *restrictInfoList = baserel->baserestrictinfo;
	ListCell *restrictInfoCell = NULL;

	/* first add the columns used in joins and projections */
	neededColumnList = list_copy(targetColumnList);

	/* then walk over all restriction clauses, and pull up any used columns */
	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Node *restrictClause = (Node *) restrictInfo->clause;
		List *clauseColumnList = NIL;

		/* recursively pull up any columns used in the restriction clause */
		clauseColumnList = pull_var_clause(restrictClause,
										   PVC_RECURSE_AGGREGATES,
										   PVC_RECURSE_PLACEHOLDERS);

		neededColumnList = list_union(neededColumnList, clauseColumnList);
	}

	/* walk over all column definitions, and de-duplicate column list */
	for (columnIndex = 1; columnIndex <= columnCount; columnIndex++)
	{
		ListCell *neededColumnCell = NULL;
		Var *column = NULL;

		/* look for this column in the needed column list */
		foreach(neededColumnCell, neededColumnList)
		{
			Var *neededColumn = (Var *) lfirst(neededColumnCell);
			if (neededColumn->varattno == columnIndex)
			{
				column = neededColumn;
				break;
			}
		}

		if (column != NULL)
		{
			columnList = lappend(columnList, column);
		}
	}

	return columnList;
}
