/*-------------------------------------------------------------------------
 *
 * mongo_fdw.c
 *
 * Function definitions for MongoDB foreign data wrapper. These functions access
 * data stored in MongoDB through the official C driver.
 *
 * Copyright (c) 2012, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mongo_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#if PG_VERSION_NUM >= 90200
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/planmain.h"
#include "utils/rel.h"
#endif
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"

/*
 * MongoFdwPlanState keeps some information specific of mongo_fdw
 * used as fdw_private
 */
typedef struct MongoFdwPlanState {
	Const *queryBuffer;
	List *columnList;
	double outputRowCount;
	Cost startupCost;
	Cost totalCost;
} MongoFdwPlanState;

/* Local functions forward declarations */
static StringInfo OptionNamesString(Oid currentContextId);

#if PG_VERSION_NUM >= 90200
static void MongoGetForeignRelSize (PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void MongoGetForeignPaths (PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static ForeignScan * MongoGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
				   ForeignPath *best_path, List *tlist, List *scan_clauses);
#else
static FdwPlan * MongoPlanForeignScan(Oid foreignTableId, PlannerInfo *root,
									  RelOptInfo *baserel);
#endif
static void MongoExplainForeignScan(ForeignScanState *scanState,
									ExplainState *explainState);
static void MongoBeginForeignScan(ForeignScanState *scanState, int executorFlags);
static TupleTableSlot * MongoIterateForeignScan(ForeignScanState *scanState);
static void MongoEndForeignScan(ForeignScanState *scanState);
static void MongoReScanForeignScan(ForeignScanState *scanState);
static Const * SerializeDocument(bson *document);
static bson * DeserializeDocument(Const *constant);
static double ForeignTableDocumentCount(Oid foreignTableId);
static void ForeignTableEstimateCosts(PlannerInfo *root, RelOptInfo *baserel,
									  List *documentClauseList, double documentCount,
									  double *rows, Cost *startupCost, Cost *totalCost);
static MongoFdwOptions * MongoGetOptions(Oid foreignTableId);
static char * MongoGetOptionValue(List *optionList, const char *optionName);
static HTAB * ColumnMappingHash(Oid foreignTableId, List *columnList);
static void FillTupleSlot(const bson *currentDocument, const bson *parentDocument,
						  HTAB *columnMappingHash, Datum *columnValues,
						  bool *columnNulls);
static void FillTupleSlotHelper(const bson *currentDocument, const bson *parentDocument,
							    HTAB *columnMappingHash, Datum *columnValues,
							    bool *columnNulls, const char *prefix);
static void FillTupleSlotColumn(const ColumnMapping *columnMapping,
					const bson_type bsonType, bson_iterator *bsonIterator,
					Datum *columnValues, bool *columnNulls);
static bool ColumnTypesCompatible(bson_type bsonType, Oid columnTypeId);
static Datum ColumnValueArray(bson_iterator *bsonIterator, Oid valueTypeId);
static ColumnValue CoerceColumnValue(bson_iterator *bsonIterator, const bson_type bsonType,
									 Oid columnTypeId, int32 columnTypeMod);
static void MongoFreeScanState(MongoFdwExecState *executionState);


/* declarations for dynamic loading */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(mongo_fdw_handler);
PG_FUNCTION_INFO_V1(mongo_fdw_validator);


/*
 * mongo_fdw_handler creates and returns a struct with pointers to foreign table
 * callback functions.
 */
Datum
mongo_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

#if PG_VERSION_NUM >= 90200
	fdwRoutine->GetForeignRelSize = MongoGetForeignRelSize;
	fdwRoutine->GetForeignPaths = MongoGetForeignPaths;
	fdwRoutine->GetForeignPlan = MongoGetForeignPlan;
#else
	fdwRoutine->PlanForeignScan = MongoPlanForeignScan;
#endif
	fdwRoutine->ExplainForeignScan = MongoExplainForeignScan;
	fdwRoutine->BeginForeignScan = MongoBeginForeignScan;
	fdwRoutine->IterateForeignScan = MongoIterateForeignScan;
	fdwRoutine->ReScanForeignScan = MongoReScanForeignScan;
	fdwRoutine->EndForeignScan = MongoEndForeignScan;

	PG_RETURN_POINTER(fdwRoutine);
}


/*
 * mongo_fdw_validator validates options given to one of the following commands:
 * foreign data wrapper, server, user mapping, or foreign table. This function
 * errors out if the given option name or its value is considered invalid.
 */
Datum
mongo_fdw_validator(PG_FUNCTION_ARGS)
{
	Datum optionArray = PG_GETARG_DATUM(0);
	Oid optionContextId = PG_GETARG_OID(1);
	List *optionList = untransformRelOptions(optionArray);
	ListCell *optionCell = NULL;

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionName = optionDef->defname;
		bool optionValid = false;

		int32 optionIndex = 0;
		for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
		{
			const MongoValidOption *validOption = &(ValidOptionArray[optionIndex]);

			if ((optionContextId == validOption->optionContextId) &&
				(strncmp(optionName, validOption->optionName, NAMEDATALEN) == 0))
			{
				optionValid = true;
				break;
			}
		}

		/* if invalid option, display an informative error message */
		if (!optionValid)
		{
			StringInfo optionNamesString = OptionNamesString(optionContextId);

			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("invalid option \"%s\"", optionName),
							errhint("Valid options in this context are: %s",
									optionNamesString->data)));
		}

		/* if port option is given, error out if its value isn't an integer */
		if (strncmp(optionName, OPTION_NAME_PORT, NAMEDATALEN) == 0)
		{
			char *optionValue = defGetString(optionDef);
			int32 portNumber = pg_atoi(optionValue, sizeof(int32), 0);
			(void) portNumber;
		}
	}

	PG_RETURN_VOID();
}


/*
 * OptionNamesString finds all options that are valid for the current context,
 * and concatenates these option names in a comma separated string.
 */
static StringInfo
OptionNamesString(Oid currentContextId)
{
	StringInfo optionNamesString = makeStringInfo();
	bool firstOptionPrinted = false;

	int32 optionIndex = 0;
	for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
	{
		const MongoValidOption *validOption = &(ValidOptionArray[optionIndex]);

		/* if option belongs to current context, append option name */
		if (currentContextId == validOption->optionContextId)
		{
			if (firstOptionPrinted)
			{
				appendStringInfoString(optionNamesString, ", ");
			}

			appendStringInfoString(optionNamesString, validOption->optionName);
			firstOptionPrinted = true;
		}
	}

	return optionNamesString;
}

static MongoFdwPlanState *
MongoGeneratePlanState(Oid foreignTableId, PlannerInfo *root, RelOptInfo *baserel)
{
	List *opExpressionList = NIL;
	bson *queryDocument = NULL;
	Const *queryBuffer = NULL;
	List *columnList = NIL;
	double documentCount = 0.0;
	MongoFdwOptions *mongoFdwOptions = NULL;
	MongoFdwPlanState *fdw_private;
	HTAB *columnMappingHash;

	/* we don't need to serialize column list as lists are copiable */
	columnList = ColumnList(baserel);
	columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

	/*
	 * We construct the query document to have MongoDB filter its rows. We could
	 * also construct a column name document here to retrieve only the needed
	 * columns. However, we found this optimization to degrade performance on
	 * the MongoDB server-side, so we instead filter out columns on our side.
	 */
	mongoFdwOptions = MongoGetOptions(foreignTableId);
	opExpressionList = ApplicableOpExpressionList(baserel);
	queryDocument = QueryDocument(foreignTableId, opExpressionList, mongoFdwOptions,
								  columnMappingHash);
	queryBuffer = SerializeDocument(queryDocument);

	/* only clean up the query struct, but not its data */
	bson_dispose(queryDocument);

	/* construct foreign plan with query document and column list */
	fdw_private = (MongoFdwPlanState *)palloc(sizeof(MongoFdwPlanState));
	fdw_private->queryBuffer = queryBuffer;
	fdw_private->columnList = columnList;

	/*
	 * We now try to retrieve the number of documents in the mongo collection;
	 * and if we can, we provide cost estimates to the query planner.
	 */
	documentCount = ForeignTableDocumentCount(foreignTableId);
	ereport(INFO, (errmsg_internal("Planning foreign scan, found %f documents in collection", documentCount)));
	if (documentCount > 0.0)
	{
		double outputRowCount = 0.0;
		Cost startupCost = 0.0;
		Cost totalCost = 0.0;

		ForeignTableEstimateCosts(root, baserel, opExpressionList, documentCount,
								  &outputRowCount, &startupCost, &totalCost);

		ereport(INFO, (errmsg_internal("Found %f documents after applying quals", outputRowCount)));
		baserel->rows = outputRowCount;
		fdw_private->outputRowCount = outputRowCount;
		fdw_private->startupCost = startupCost;
		fdw_private->totalCost = totalCost;
	}
	else
	{
		ereport(DEBUG1, (errmsg("could not retrieve document count for collection"),
						 errhint("Falling back to default estimates in planning")));
	}
	return fdw_private;
}

#if PG_VERSION_NUM >= 90200
/*
 * MongoGetForeignRelSize creates a foreign plan to scan the table, and provides a
 * cost estimate for the plan. The foreign plan has two components; the first
 * uses restriction qualifiers (WHERE clauses) to create the query to send to
 * MongoDB. The second includes the columns used in executing the query.
 */
static void
MongoGetForeignRelSize (PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId)
{
	baserel->fdw_private = (void *)MongoGeneratePlanState(foreignTableId, root, baserel);
}

/*
 * MongoGetForeignPaths creates only one path used to execute the query.
 */
static void
MongoGetForeignPaths (PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	MongoFdwPlanState *fdw_private = (MongoFdwPlanState *) baserel->fdw_private;

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 baserel->rows,
									 fdw_private->startupCost,
									 fdw_private->totalCost,
									 NIL,		/* no pathkeys */
									 NULL,		/* no outer rel either */
									 NIL));		/* no fdw_private data */
}

/*
 * MongoGetForeignPlan creates the plan.
 */
static ForeignScan *
MongoGetForeignPlan(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid,
				   ForeignPath *best_path,
				   List *tlist,
				   List *scan_clauses)
{
	Index		scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							baserel->fdw_private);
}
#else
/*
 * MongoPlanForeignScan creates a foreign plan to scan the table, and provides a
 * cost estimate for the plan. The foreign plan has two components; the first
 * uses restriction qualifiers (WHERE clauses) to create the query to send to
 * MongoDB. The second includes the columns used in executing the query.
 */
static FdwPlan *
MongoPlanForeignScan(Oid foreignTableId, PlannerInfo *root, RelOptInfo *baserel)
{
	FdwPlan *foreignPlan = NULL;
	MongoFdwPlanState *fdw_private;
	fdw_private = MongoGeneratePlanState(foreignTableId, root, baserel);
	foreignPlan = makeNode(FdwPlan);
	foreignPlan->fdw_private = (void *)fdw_private;
	foreignPlan->startup_cost = fdw_private->startupCost;
	foreignPlan->total_cost = fdw_private->totalCost;
	return foreignPlan;
}
#endif


/*
 * MongoExplainForeignScan produces extra output for the Explain command.
 */
static void
MongoExplainForeignScan(ForeignScanState *scanState, ExplainState *explainState)
{
	MongoFdwOptions *mongoFdwOptions = NULL;
	StringInfo namespaceName = NULL;
	Oid foreignTableId = InvalidOid;

	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	mongoFdwOptions = MongoGetOptions(foreignTableId);

	/* construct fully qualified collection name */
	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", mongoFdwOptions->databaseName,
					 mongoFdwOptions->collectionName);

	ExplainPropertyText("Foreign Namespace", namespaceName->data, explainState);
}


/*
 * MongoBeginForeignScan connects to the MongoDB server, and opens a cursor that
 * uses the database name, collection name, and the remote query to send to the
 * server. The function also creates a hash table that maps referenced column
 * names to column index and type information.
 */
static void
MongoBeginForeignScan(ForeignScanState *scanState, int executorFlags)
{
	mongo *mongoConnection = NULL;
	mongo_cursor *mongoCursor = NULL;
	int32 connectStatus = MONGO_ERROR;
	int32 authStatus = MONGO_ERROR;
	Oid foreignTableId = InvalidOid;
	List *columnList = NIL;
	HTAB *columnMappingHash = NULL;
	char *addressName = NULL;
	int32 portNumber = 0;
	bool useAuth = false;
	char *username = NULL;
	char *password = NULL;
	char *databaseName = NULL;
	int32 errorCode = 0;
	StringInfo namespaceName = NULL;
	ForeignScan *foreignScan = NULL;
	MongoFdwPlanState *fdw_private = NULL;
	Const *queryBuffer = NULL;
	bson *queryDocument = NULL;
	MongoFdwOptions *mongoFdwOptions = NULL;
	MongoFdwExecState *executionState = NULL;

	/* if Explain with no Analyze, do nothing */
	if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		return;
	}

	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	mongoFdwOptions = MongoGetOptions(foreignTableId);

	/* resolve hostname and port number; and connect to mongo server */
	addressName = mongoFdwOptions->addressName;
	portNumber = mongoFdwOptions->portNumber;

	mongoConnection = mongo_create();
	mongo_init(mongoConnection);

	connectStatus = mongo_connect(mongoConnection, addressName, portNumber);
	if (connectStatus != MONGO_OK)
	{
		errorCode = (int32) mongoConnection->err;

		mongo_destroy(mongoConnection);
		mongo_dispose(mongoConnection);

		ereport(ERROR, (errmsg("could not connect to %s:%d", addressName, portNumber),
						errhint("Mongo driver connection error: %d", errorCode)));
	}

	useAuth = mongoFdwOptions->useAuth;
	if (useAuth)
	{
		username = mongoFdwOptions->username;
		password = mongoFdwOptions->password;
		databaseName = mongoFdwOptions->databaseName;

		authStatus = mongo_cmd_authenticate(
				mongoConnection, databaseName, username, password);

		if (authStatus != MONGO_OK)
		{
			mongo_destroy(mongoConnection);
			mongo_dispose(mongoConnection);

			ereport(ERROR, (errmsg("could not authenticate with user %s on database %s", 
								   username, databaseName),
							errhint("Update user mapping for user.")));
		}
	}

	/* deserialize query document; and create column info hash */
	foreignScan = (ForeignScan *) scanState->ss.ps.plan;
#if PG_VERSION_NUM >= 90200
	fdw_private = (MongoFdwPlanState *)foreignScan->fdw_private;
#else
	fdw_private = (MongoFdwPlanState *)((FdwPlan *) foreignScan->fdwplan)->fdw_private;
#endif

	queryBuffer = fdw_private->queryBuffer;
	queryDocument = DeserializeDocument(queryBuffer);

	columnList = fdw_private->columnList;
	columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", mongoFdwOptions->databaseName,
					 mongoFdwOptions->collectionName);

	/* create cursor for collection name and set query */
	mongoCursor = mongo_cursor_create();
	mongo_cursor_init(mongoCursor, mongoConnection, namespaceName->data);
	mongo_cursor_set_options(mongoCursor, MONGO_SLAVE_OK);
	mongo_cursor_set_query(mongoCursor, queryDocument);

	/* create and set foreign execution state */
	executionState = (MongoFdwExecState *) palloc0(sizeof(MongoFdwExecState));
	executionState->columnMappingHash = columnMappingHash;
	executionState->mongoConnection = mongoConnection;
	executionState->mongoCursor = mongoCursor;
	executionState->parentDocument = NULL;
	executionState->arrayCursor = NULL;
	executionState->arrayFieldName = mongoFdwOptions->fieldName;
	executionState->queryDocument = queryDocument;

	scanState->fdw_state = (void *) executionState;
}

static bson_type 
BsonFindSubobject(bson_iterator *bsonIterator, const bson *bsonObject, const char* path)
{
	bson_type bsonCursorStatus;
	char *dot = strchr(path, '.');
	if (dot)
	{
		*dot = '\0';
	}
	bson_iterator_init(bsonIterator, bsonObject);
	bsonCursorStatus = bson_find(bsonIterator, bsonObject, path);

	if (!dot)
	{
		return bsonCursorStatus;
	}

	*dot = '.';

	if (bsonCursorStatus != BSON_OBJECT) {
		return BSON_EOO;
	}

	bson *sub = bson_create();
	bson_iterator_subobject(bsonIterator, sub);
	bsonCursorStatus = BsonFindSubobject(bsonIterator, sub, (dot + 1));
	bson_dispose(sub);
	return bsonCursorStatus;
}

/*
 * MongoIterateForeignScan reads the next document from MongoDB, converts it to
 * a PostgreSQL tuple, and stores the converted tuple into the ScanTupleSlot as
 * a virtual tuple.
 */
static TupleTableSlot *
MongoIterateForeignScan(ForeignScanState *scanState)
{
	MongoFdwExecState *executionState = (MongoFdwExecState *) scanState->fdw_state;
	TupleTableSlot *tupleSlot = scanState->ss.ss_ScanTupleSlot;
	mongo_cursor *mongoCursor = executionState->mongoCursor;
	HTAB *columnMappingHash = executionState->columnMappingHash;
	bson_iterator *arrayCursor = executionState->arrayCursor;
	int32 mongoCursorStatus = MONGO_ERROR;
	bson_type bsonCursorStatus;
	const bson *collectionDocument = executionState->parentDocument;

	TupleDesc tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	Datum *columnValues = tupleSlot->tts_values;
	bool *columnNulls = tupleSlot->tts_isnull;
	int32 columnCount = tupleDescriptor->natts;

	/*
	 * We execute the protocol to load a virtual tuple into a slot. We first
	 * call ExecClearTuple, then fill in values / isnull arrays, and last call
	 * ExecStoreVirtualTuple. If we are done fetching documents from Mongo, we
	 * just return an empty slot as required.
	 */
	ExecClearTuple(tupleSlot);

	/* initialize all values for this row to null */
	memset(columnValues, 0, columnCount * sizeof(Datum));
	memset(columnNulls, true, columnCount * sizeof(bool));

	while(true)
	{
		if(!collectionDocument)
		{
			ereport(DEBUG2, (errmsg_internal("Getting collection document")));
			mongoCursorStatus = mongo_cursor_next(mongoCursor);
			if(mongoCursorStatus == MONGO_OK)
			{
				collectionDocument = mongo_cursor_bson(mongoCursor);
			}
			else {
				/*
				 * The following is a courtesy check. In practice when Mongo shuts down,
				 * mongo_cursor_next() could possibly crash. This function first frees
				 * cursor->reply, and then references reply in mongo_cursor_destroy().
				 */
				mongo_cursor_error_t errorCode = mongoCursor->err;
				if (errorCode != MONGO_CURSOR_EXHAUSTED)
				{
					MongoFreeScanState(executionState);

					ereport(ERROR, (errmsg("could not iterate over mongo collection"),
									errhint("Mongo driver cursor error code: %d", errorCode)));
				}
				else
				{
					ereport(DEBUG1, (errmsg_internal("Mongo cursor exhausted")));
				}
				/* EXIT */
				return tupleSlot;
			}
		}

		/* Now we have a document from the collection */

		char *arrayFieldName = executionState->arrayFieldName;
		if(!arrayFieldName || *arrayFieldName == '\0')
		{
			ereport(DEBUG2, (errmsg_internal("Filling tuple from collection document.")));
			/* We're not iterating over any embedded arrays, so just fill the slot and return */
			FillTupleSlot(collectionDocument, NULL, columnMappingHash, columnValues, columnNulls);
			ExecStoreVirtualTuple(tupleSlot);

			/* EXIT */
			return tupleSlot;
		}
		else
		{
			ereport(DEBUG2, (errmsg_internal("Getting embedded array '%s'", arrayFieldName)));
			/* We're iterating over an embedded array. */
			if(!arrayCursor)
			{
				ereport(DEBUG2, (errmsg_internal("Getting array cursor from collection document")));
				bson_iterator bsonIterator = { NULL, 0 };
				bsonCursorStatus = BsonFindSubobject(&bsonIterator, collectionDocument, arrayFieldName);

				if (bsonCursorStatus == BSON_ARRAY) {
					arrayCursor = bson_iterator_create();
					bson_iterator_subiterator(&bsonIterator, arrayCursor);
				}
				else
				{
					ereport(DEBUG2, (errmsg_internal("Can't find array on collection document.")));
					/* The embedded array field is not present on this document.  Next! */
					executionState->parentDocument = collectionDocument = NULL;
					continue;
				}
			}

			/* Now we have an array cursor */

			bsonCursorStatus = bson_iterator_next(arrayCursor);
			while(bsonCursorStatus && bsonCursorStatus != BSON_OBJECT)
			{
				ereport(DEBUG2, (errmsg_internal("Didn't get an object from array cursor.  Trying again.")));
				bsonCursorStatus = bson_iterator_next(arrayCursor);
			}

			if (bsonCursorStatus != BSON_OBJECT) 
			{
				ereport(DEBUG2, (errmsg_internal("Ran out of documents in array cursor.  Freeing it and moving to next collection document.")));
				/* No more objects in this array.  Next document! */
				executionState->parentDocument = collectionDocument = NULL;
				bson_iterator_dispose(arrayCursor);
				executionState->arrayCursor = arrayCursor = NULL;
				continue;
			}

			/* Now we have a document from the embedded array */
			ereport(DEBUG2, (errmsg_internal("Found document in array")));
			bson *arrayDocument = bson_create();
			bson_iterator_subobject(arrayCursor, arrayDocument);
			ereport(DEBUG2, (errmsg_internal("Filling tuple slot from array document")));
			FillTupleSlot(arrayDocument, collectionDocument, columnMappingHash,
					      columnValues, columnNulls);
			ereport(DEBUG2, (errmsg_internal("Freeing array document")));
			bson_dispose(arrayDocument);
			ereport(DEBUG2, (errmsg_internal("Storing tuple")));
			ExecStoreVirtualTuple(tupleSlot);

			executionState->parentDocument = collectionDocument;
			executionState->arrayCursor = arrayCursor;

			return tupleSlot;
		}
	}
}


/*
 * MongoEndForeignScan finishes scanning the foreign table, closes the cursor
 * and the connection to MongoDB, and reclaims scan related resources.
 */
static void
MongoEndForeignScan(ForeignScanState *scanState)
{
	mongo_cursor *mongoCursor = NULL;

	MongoFdwExecState *executionState = (MongoFdwExecState *) scanState->fdw_state;
	mongoCursor = executionState->mongoCursor;

	ereport(INFO, (errmsg_internal("Query returned %d documents.", 
								   mongoCursor->seen)));


	/* if we executed a query, reclaim mongo related resources */
	if (executionState != NULL)
	{
		MongoFreeScanState(executionState);
	}
}


/*
 * MongoReScanForeignScan rescans the foreign table. Note that rescans in Mongo
 * end up being notably more expensive than what the planner expects them to be,
 * since MongoDB cursors don't provide reset/rewind functionality.
 */
static void
MongoReScanForeignScan(ForeignScanState *scanState)
{
	ereport(DEBUG2, (errmsg_internal("Rescanning")));
	MongoFdwExecState *executionState = (MongoFdwExecState *) scanState->fdw_state;
	mongo *mongoConnection = executionState->mongoConnection;
	MongoFdwOptions *mongoFdwOptions = NULL;
	mongo_cursor *mongoCursor = NULL;
	StringInfo namespaceName = NULL;
	Oid foreignTableId = InvalidOid;

	/* close down the old cursor */
	mongo_cursor_destroy(executionState->mongoCursor);
	mongo_cursor_dispose(executionState->mongoCursor);

	executionState->parentDocument = NULL;

	if (executionState->arrayCursor)
	{
		ereport(DEBUG2, (errmsg_internal("Freeing arrayCursor")));
		bson_iterator_dispose(executionState->arrayCursor);
		executionState->arrayCursor = NULL;
	}

	/* reconstruct full collection name */
	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	mongoFdwOptions = MongoGetOptions(foreignTableId);

	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", mongoFdwOptions->databaseName,
					 mongoFdwOptions->collectionName);

	/* reconstruct cursor for collection name and set query */
	mongoCursor = mongo_cursor_create();
	mongo_cursor_init(mongoCursor, mongoConnection, namespaceName->data);
	mongo_cursor_set_query(mongoCursor, executionState->queryDocument);

	executionState->mongoCursor = mongoCursor;
}


/*
 * SerializeDocument serializes the document's data to a constant, as advised in
 * foreign/fdwapi.h. Note that this function shallow-copies the document's data;
 * and the caller should therefore not free it.
 */
static Const *
SerializeDocument(bson *document)
{
	Const *serializedDocument = NULL;
	Datum documentDatum = 0;

	/*
	 * We access document data and wrap a datum around it. Note that even when
	 * we have an empty document, the document size can't be zero according to
	 * bson apis.
	 */
	const char *documentData = bson_data(document);
	int32 documentSize = bson_buffer_size(document);
	Assert(documentSize != 0);

	documentDatum = CStringGetDatum(documentData);
	serializedDocument = makeConst(CSTRINGOID, -1, InvalidOid, documentSize,
								   documentDatum, false, false);

	return serializedDocument;
}


/*
 * DeserializeDocument deserializes the constant to a bson document. For this,
 * the function creates a document, and explicitly sets the document's data.
 */
static bson *
DeserializeDocument(Const *constant)
{
	bson *document = NULL;
	Datum documentDatum = constant->constvalue;
	char *documentData = DatumGetCString(documentDatum);

	Assert(constant->constlen > 0);
	Assert(constant->constisnull == false);

	document = bson_create();
	bson_init_size(document, 0);
	bson_init_finished_data(document, documentData);

	return document;
}


/*
 * ForeignTableDocumentCount connects to the MongoDB server, and queries it for
 * the number of documents in the foreign collection. On success, the function
 * returns the document count. On failure, the function returns -1.0.
 */
static double
ForeignTableDocumentCount(Oid foreignTableId)
{
	MongoFdwOptions *options = NULL;
	mongo *mongoConnection = NULL;
	const bson *emptyQuery = NULL;
	int32 connectStatus = MONGO_ERROR;
	int32 authStatus = MONGO_OK;
	double documentCount = 0.0;

	/* resolve foreign table options; and connect to mongo server */
	options = MongoGetOptions(foreignTableId);

	mongoConnection = mongo_create();
	mongo_init(mongoConnection);

	connectStatus = mongo_connect(mongoConnection, options->addressName, options->portNumber);

	if (connectStatus == MONGO_OK && options->useAuth)
	{
		authStatus = mongo_cmd_authenticate(
				mongoConnection, options->databaseName, options->username,
				options->password);
	}

	if (connectStatus == MONGO_OK && authStatus == MONGO_OK)
	{

		documentCount = mongo_count(mongoConnection, options->databaseName,
									options->collectionName, emptyQuery);
	}
	else
	{
		documentCount = -1.0;
	}

	mongo_destroy(mongoConnection);
	mongo_dispose(mongoConnection);

	return documentCount;
}


/*
 * ForeignTableEstimateCosts estimates the cost of scanning a foreign table. The
 * estimates should become much more accurate once we upgrade to PostgreSQL 9.2;
 * this new version includes APIs to acquire random samples from the data and
 * allows us to keep accurate statistics.
 */
static void
ForeignTableEstimateCosts(PlannerInfo *root, RelOptInfo *baserel,
						  List *documentClauseList, double documentCount,
						  double *outputRowCount, Cost *startupCost, Cost *totalCost)
{
	List *rowClauseList = baserel->baserestrictinfo;
	double tupleFilterCost = baserel->baserestrictcost.per_tuple;
	double inputRowCount = 0.0;
	double documentSelectivity = 0.0;
	double rowSelectivity = 0.0;
	double foreignTableSize = 0;
	int32 documentWidth = 0;
	BlockNumber pageCount = 0;
	double totalDiskAccessCost = 0.0;
	double cpuCostPerDoc = 0.0;
	double cpuCostPerRow = 0.0;
	double totalCpuCost = 0.0;
	double connectionCost = 0.0;

	/* resolve foreign table id first */
	RangeTblEntry *rangeTableEntry = root->simple_rte_array[baserel->relid];
	Oid foreignTableId = rangeTableEntry->relid;

	/*
	 * We estimate disk costs assuming a sequential scan over the data. This is
	 * an inaccurate assumption as Mongo scatters the data over disk pages, and
	 * may rely on an index to retrieve the data. Still, this should at least
	 * give us a relative cost.
	 */
	documentWidth = get_relation_data_width(foreignTableId, baserel->attr_widths);
	foreignTableSize = documentCount * documentWidth;

	pageCount = (BlockNumber) rint(foreignTableSize / BLCKSZ);
	totalDiskAccessCost = seq_page_cost * pageCount;

	/*
	 * We estimate the number of documents returned by MongoDB and the number of
	 * rows returned after restriction qualifiers are applied. Both are rough
	 * estimates since the planner doesn't have any stats about the relation.
	 */
	documentSelectivity = clauselist_selectivity(root, documentClauseList,
												 0, JOIN_INNER, NULL);
	inputRowCount = clamp_row_est(documentCount * documentSelectivity);

	rowSelectivity = clauselist_selectivity(root, rowClauseList,
											0, JOIN_INNER, NULL);
	(*outputRowCount) = clamp_row_est(documentCount * rowSelectivity);

	/*
	 * The cost of processing a document returned by Mongo (input row) is 5x the
	 * cost of processing a regular row.
	 */
	cpuCostPerDoc = cpu_tuple_cost;
	cpuCostPerRow = (cpu_tuple_cost * MONGO_TUPLE_COST_MULTIPLIER) + tupleFilterCost;
	totalCpuCost = (cpuCostPerDoc * documentCount) + (cpuCostPerRow * inputRowCount);

	connectionCost = MONGO_CONNECTION_COST_MULTIPLIER * seq_page_cost;
	(*startupCost) = baserel->baserestrictcost.startup + connectionCost;
	(*totalCost) = (*startupCost) + totalDiskAccessCost + totalCpuCost;
}


/*
 * MongoGetOptions returns the option values to be used when connecting to and
 * querying MongoDB. To resolve these values, the function checks the foreign
 * table's options, and if not present, falls back to default values.
 */
static MongoFdwOptions *
MongoGetOptions(Oid foreignTableId)
{
	MongoFdwOptions *mongoFdwOptions = NULL;
	char *addressName = NULL;
	char *portName = NULL;
	int32 portNumber = 0;
	char *databaseName = NULL;
	char *collectionName = NULL;
	char *fieldName = NULL;
	char *username = NULL;
	char *password = NULL;
	char *useAuthStr = NULL;
	bool useAuth = false;

	ForeignTable *foreignTable = NULL;
	ForeignServer *foreignServer = NULL;
	UserMapping *userMapping = NULL;
	List *optionList = NIL;

	foreignTable = GetForeignTable(foreignTableId);
	foreignServer = GetForeignServer(foreignTable->serverid);

	optionList = list_concat(optionList, foreignTable->options);
	optionList = list_concat(optionList, foreignServer->options);

	addressName = MongoGetOptionValue(optionList, OPTION_NAME_ADDRESS);
	if (addressName == NULL)
	{
		addressName = pstrdup(DEFAULT_IP_ADDRESS);
	}

	portName = MongoGetOptionValue(optionList, OPTION_NAME_PORT);
	if (portName == NULL)
	{
		portNumber = DEFAULT_PORT_NUMBER;
	}
	else
	{
		portNumber = pg_atoi(portName, sizeof(int32), 0);
	}

	databaseName = MongoGetOptionValue(optionList, OPTION_NAME_DATABASE);
	if (databaseName == NULL)
	{
		databaseName = pstrdup(DEFAULT_DATABASE_NAME);
	}

	collectionName = MongoGetOptionValue(optionList, OPTION_NAME_COLLECTION);
	if (collectionName == NULL)
	{
		collectionName = get_rel_name(foreignTableId);
	}
	
	fieldName = MongoGetOptionValue(optionList, OPTION_NAME_FIELD);

	useAuthStr = MongoGetOptionValue(optionList, OPTION_NAME_USE_AUTH);
	if (useAuthStr != NULL)
	{
		if (!parse_bool(useAuthStr, &useAuth))
		{
			useAuth = false;
		}
	}
	if (useAuth)
	{
		userMapping = GetUserMapping(GetUserId(), foreignTable->serverid);
		optionList = list_concat(optionList, userMapping->options);
		username = MongoGetOptionValue(optionList, OPTION_NAME_USERNAME);
		password = MongoGetOptionValue(optionList, OPTION_NAME_PASSWORD);
	}

	mongoFdwOptions = (MongoFdwOptions *) palloc0(sizeof(MongoFdwOptions));
	mongoFdwOptions->addressName = addressName;
	mongoFdwOptions->portNumber = portNumber;
	mongoFdwOptions->databaseName = databaseName;
	mongoFdwOptions->collectionName = collectionName;
	mongoFdwOptions->fieldName = fieldName;
	mongoFdwOptions->username = username;
	mongoFdwOptions->password = password;
	mongoFdwOptions->useAuth = useAuth;

	return mongoFdwOptions;
}


/*
 * MongoGetOptionValue walks over foreign table and foreign server options, and
 * looks for the option with the given name. If found, the function returns the
 * option's value.
 */
static char *
MongoGetOptionValue(List *optionList, const char *optionName)
{
	ListCell *optionCell = NULL;
	char *optionValue = NULL;

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionDefName = optionDef->defname;

		if (strncmp(optionDefName, optionName, NAMEDATALEN) == 0)
		{
			optionValue = defGetString(optionDef);
			break;
		}
	}

	return optionValue;
}

static void
AddColumnOptions(ColumnMapping *columnMapping, Oid foreignTableId, AttrNumber columnId)
{
	List *options;
	ListCell *lc;

	options = GetForeignColumnOptions(foreignTableId, columnId);
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, OPTION_NAME_MONGO_TYPE) == 0)
		{
			char *type = defGetString(def);
			bson_type bsonType = 0;
			if (pg_strcasecmp(type, "integer") == 0)
			{
				bsonType = BSON_INT;
			}
			else if (pg_strcasecmp(type, "long") == 0)
			{
				bsonType = BSON_LONG;
			}
			else if (pg_strcasecmp(type, "double") == 0)
			{
				bsonType = BSON_DOUBLE;
			}
			else if (pg_strcasecmp(type, "string") == 0)
			{
				bsonType = BSON_STRING;
			}
			else if (pg_strcasecmp(type, "oid") == 0)
			{
				bsonType = BSON_OID;
			}
			else if (pg_strcasecmp(type, "bool") == 0)
			{
				bsonType = BSON_BOOL;
			}
			else if (pg_strcasecmp(type, "date") == 0)
			{
				bsonType = BSON_DATE;
			}
			else if (pg_strcasecmp(type, "timestamp") == 0)
			{
				bsonType = BSON_TIMESTAMP;
			}
			else if (*type != '\0')
			{
				ereport(ERROR, (errmsg("Type %s is not a valid column mongo_type.", type)));
			}

			columnMapping->columnBsonType = bsonType;
		}
	}
}

/*
 * ColumnMappingHash creates a hash table that maps column names to column index
 * and types. This table helps us quickly translate BSON document key/values to
 * the corresponding PostgreSQL columns.
 */
static HTAB *
ColumnMappingHash(Oid foreignTableId, List *columnList)
{
	ListCell *columnCell = NULL;
	const long hashTableSize = 2048;
	HTAB *columnMappingHash = NULL;

	/* create hash table */
	HASHCTL hashInfo;
	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = NAMEDATALEN;
	hashInfo.entrysize = sizeof(ColumnMapping);
	hashInfo.hash = string_hash;
	hashInfo.hcxt = CurrentMemoryContext;

	columnMappingHash = hash_create("Column Mapping Hash", hashTableSize, &hashInfo,
									(HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT));
	Assert(columnMappingHash != NULL);

	foreach(columnCell, columnList)
	{
		Var *column = (Var *) lfirst(columnCell);
		AttrNumber columnId = column->varattno;

		ColumnMapping *columnMapping = NULL;
		char *columnName = NULL;
		bool handleFound = false;
		void *hashKey = NULL;

		columnName = get_relid_attribute_name(foreignTableId, columnId);
		hashKey = (void *) columnName;

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
													  HASH_ENTER, &handleFound);
		Assert(columnMapping != NULL);

		columnMapping->columnIndex = columnId - 1;
		columnMapping->columnTypeId = column->vartype;
		columnMapping->columnTypeMod = column->vartypmod;
		columnMapping->columnArrayTypeId = get_element_type(column->vartype);
		AddColumnOptions(columnMapping, foreignTableId, columnId);
	}

	return columnMappingHash;
}


/*
 * FillTupleSlot walks over all key/value pairs in the given document. For each
 * pair, the function checks if the key appears in the column mapping hash, and
 * if the value type is compatible with the one specified for the column. If so,
 * the function converts the value and fills the corresponding tuple position.
 */
static void
FillTupleSlot(const bson *bsonDocument, const bson *parentDocument,
			  HTAB *columnMappingHash, Datum *columnValues, bool *columnNulls)
{
	FillTupleSlotHelper(bsonDocument, parentDocument, columnMappingHash,
			columnValues, columnNulls, NULL);
}

static void
FillTupleSlotHelper(const bson *bsonDocument, const bson *parentDocument,
		            HTAB *columnMappingHash, Datum *columnValues,
					bool *columnNulls, const char *prefix)
{
	if(parentDocument)
	{
		FillTupleSlotHelper(parentDocument, NULL, columnMappingHash, columnValues,
							columnNulls, "parent");
	}

	bson_iterator bsonIterator = { NULL, 0 };
	bson_iterator_init(&bsonIterator, bsonDocument);

	while (bson_iterator_next(&bsonIterator))
	{
		const char *bsonKey = bson_iterator_key(&bsonIterator);
		ereport(DEBUG2, (errmsg_internal("Next document field: %s", bsonKey)));
		bson_type bsonType = bson_iterator_type(&bsonIterator);

		ColumnMapping *columnMapping = NULL;
		bool handleFound = false;
		const char *qualifiedKey = NULL;

		if (prefix)
		{
			/* for fields in nested BSON objects, use fully qualified field
			 * name to check the column mapping */
			StringInfo qualifiedKeyInfo = makeStringInfo();
			appendStringInfo(qualifiedKeyInfo, "%s.%s", prefix, bsonKey);
			qualifiedKey = qualifiedKeyInfo->data;
		}
		else
		{
			qualifiedKey = bsonKey;
		}

		/* recurse into nested objects */
		if (bsonType == BSON_OBJECT)
		{
			bson *sub = bson_create();
			bson_iterator_subobject(&bsonIterator, sub);
			ereport(DEBUG2, (errmsg_internal("Recursing into sub-document: %p", sub)));
			FillTupleSlotHelper(sub, parentDocument, columnMappingHash,
								columnValues, columnNulls, qualifiedKey);
			ereport(DEBUG2, (errmsg_internal("Destroying sub-document: %p, %p", sub,
											 sub->data)));
			bson_dispose(sub);
			ereport(DEBUG2, (errmsg_internal("Destroyed sub-document")));
			continue;
		}

		/* look up the corresponding column for this bson key */
		void *hashKey = (void *) qualifiedKey;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey, HASH_FIND,
													  &handleFound);
		FillTupleSlotColumn(columnMapping, bsonType, &bsonIterator,
							columnValues, columnNulls);

		if (bsonType == BSON_OID)
		{
			StringInfo generatedTimeKeyInfo = makeStringInfo();
			appendStringInfo(generatedTimeKeyInfo, "%s.generated",
							 qualifiedKey);
			columnMapping = (ColumnMapping *) hash_search(
				columnMappingHash, (void *)generatedTimeKeyInfo->data,
				HASH_FIND, &handleFound);
			FillTupleSlotColumn(columnMapping, bsonType, &bsonIterator,
								columnValues, columnNulls);
		}
	}


	ereport(DEBUG2, (errmsg_internal("Finished document.")));
}

static void
FillTupleSlotColumn(const ColumnMapping *columnMapping,
					const bson_type bsonType, bson_iterator *bsonIterator,
					Datum *columnValues, bool *columnNulls)
{
	Oid columnTypeId = InvalidOid;
	Oid columnArrayTypeId = InvalidOid;
	bool compatibleTypes = false;
	/* if no corresponding column or null bson value, continue */
	if (columnMapping == NULL || bsonType == BSON_NULL)
	{
		return;
	}

	/* check if columns have compatible types */
	columnTypeId = columnMapping->columnTypeId;
	columnArrayTypeId = columnMapping->columnArrayTypeId;

	if (OidIsValid(columnArrayTypeId) && bsonType == BSON_ARRAY)
	{
		compatibleTypes = true;
	}
	else if(!OidIsValid(columnArrayTypeId))
	{
		compatibleTypes = ColumnTypesCompatible(bsonType, columnTypeId);
	}

	/* if types are incompatible, leave this column null */
	if (!compatibleTypes)
	{
		return;
	}

	/* fill in corresponding column value and null flag */
	if (OidIsValid(columnArrayTypeId))
	{
		int32 columnIndex = columnMapping->columnIndex;

		columnValues[columnIndex] = ColumnValueArray(bsonIterator,
													 columnArrayTypeId);
		columnNulls[columnIndex] = false;
	}
	else
	{
		int32 columnIndex = columnMapping->columnIndex;
		Oid columnTypeMod = columnMapping->columnTypeMod;

		ColumnValue columnValue = CoerceColumnValue(bsonIterator, bsonType,
										columnTypeId, columnTypeMod);
		if (!columnValue.isNull)
		{
			columnValues[columnIndex] = columnValue.datum;
			columnNulls[columnIndex] = false;
		}
	}
}

/*
 * ColumnTypesCompatible checks if the given BSON type can be converted to the
 * given PostgreSQL type. In this check, the function also uses its knowledge of
 * internal conversions applied by BSON APIs.
 */
static bool
ColumnTypesCompatible(bson_type bsonType, Oid columnTypeId)
{
	bool compatibleTypes = false;

	/* we consider the PostgreSQL column type as authoritative */
	switch(columnTypeId)
	{
		case INT2OID: case INT4OID:
		case INT8OID: case FLOAT4OID:
		case FLOAT8OID: case NUMERICOID:
		{
			if (bsonType == BSON_INT || bsonType == BSON_LONG ||
				bsonType == BSON_DOUBLE || bsonType == BSON_STRING)
			{
				compatibleTypes = true;
			}
			break;
		}
		case BOOLOID:
		{
			if (bsonType == BSON_INT || bsonType == BSON_LONG ||
				bsonType == BSON_DOUBLE || bsonType == BSON_BOOL ||
				bsonType == BSON_STRING)
			{
				compatibleTypes = true;
			}
			break;
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			if (bsonType == BSON_INT || bsonType == BSON_LONG ||
				bsonType == BSON_DOUBLE || bsonType == BSON_BOOL ||
				bsonType == BSON_STRING || bsonType == BSON_OID)
			{
				compatibleTypes = true;
			}
			break;
		}
	    case NAMEOID:
		{
			/*
			 * We currently overload the NAMEOID type to represent the BSON
			 * object identifier. We can safely overload this 64-byte data type
			 * since it's reserved for internal use in PostgreSQL.
			 */
			if (bsonType == BSON_OID)
			{
				compatibleTypes = true;
			}
			break;
		}
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			if (bsonType == BSON_DATE || bsonType == BSON_OID ||
				bsonType == BSON_INT || bsonType == BSON_LONG ||
				bsonType == BSON_DOUBLE)
			{
				compatibleTypes = true;
			}
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
							errmsg("cannot convert bson type to column type"),
							errhint("Column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return compatibleTypes;
}


/*
 * ColumnValueArray uses array element type id to read the current array pointed
 * to by the BSON iterator, and converts each array element (with matching type)
 * to the corresponding PostgreSQL datum. Then, the function constructs an array
 * datum from element datums, and returns the array datum.
 */
static Datum
ColumnValueArray(bson_iterator *bsonIterator, Oid valueTypeId)
{
	Datum *columnValueArray = palloc0(INITIAL_ARRAY_CAPACITY * sizeof(Datum));
	uint32 arrayCapacity = INITIAL_ARRAY_CAPACITY;
	uint32 arrayGrowthFactor = 2;
	uint32 arrayIndex = 0;

	ArrayType *columnValueObject = NULL;
	Datum columnValueDatum = 0;
	bool typeByValue = false;
	char typeAlignment = 0;
	int16 typeLength = 0;

	bson_iterator bsonSubIterator = { NULL, 0 };
	bson_iterator_subiterator(bsonIterator, &bsonSubIterator);

	while (bson_iterator_next(&bsonSubIterator))
	{
		bson_type bsonType = bson_iterator_type(&bsonSubIterator);
		bool compatibleTypes = false;

		compatibleTypes = ColumnTypesCompatible(bsonType, valueTypeId);
		if (bsonType == BSON_NULL || !compatibleTypes)
		{
			continue;
		}

		ColumnValue columnValue = CoerceColumnValue(&bsonSubIterator, bsonType, valueTypeId, 0);
		if (columnValue.isNull)
		{
			continue;
		}

		if (arrayIndex >= arrayCapacity)
		{
			arrayCapacity *= arrayGrowthFactor;
			columnValueArray = repalloc(columnValueArray, arrayCapacity * sizeof(Datum));
		}

		/* use default type modifier (0) to convert column value */
		columnValueArray[arrayIndex] = columnValue.datum;
		arrayIndex++;
	}

	get_typlenbyvalalign(valueTypeId, &typeLength, &typeByValue, &typeAlignment);
	columnValueObject = construct_array(columnValueArray, arrayIndex, valueTypeId,
										typeLength, typeByValue, typeAlignment);

	columnValueDatum = PointerGetDatum(columnValueObject);
	return columnValueDatum;
}

static bool
BsonLong(bson_iterator *bsonIterator, const bson_type bsonType, long *result)
{
	switch(bsonType)
	{
		case BSON_STRING:
		{
			long value = 0;
			if(ParseLong(bson_iterator_string(bsonIterator), &value))
			{
				*result = value;
				return true;
			}
			else
			{
				return false;
			}
		}
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		{
			*result = bson_iterator_long(bsonIterator);
			return true;
		}
		default:
		{
			return false;
		}
	}
}

static bool
BsonDouble(bson_iterator *bsonIterator, const bson_type bsonType, double *result)
{
	switch(bsonType)
	{
		case BSON_STRING:
		{
			double value = 0;
			if(ParseDouble(bson_iterator_string(bsonIterator), &value)) {
				*result = value;
				return true;
			}
			else
			{
				return false;
			}
		}
		case BSON_INT:
		case BSON_LONG:
		case BSON_DOUBLE:
		{
			*result = bson_iterator_double(bsonIterator);
			return true;
		}
		default:
		{
			return false;
			break;
		}
	}
}

static const char *
BsonString(bson_iterator *bsonIterator, const bson_type bsonType)
{
	char *result;
	switch(bsonType)
	{
		case BSON_STRING:
		{
			result = bson_iterator_string(bsonIterator);
			break;
		}
		case BSON_INT:
		case BSON_LONG:
		{
			long value = bson_iterator_long(bsonIterator);
			/* Maximum length of a 64 bit number is 20 digits.  Add one for 
			 * sign and another for \0. */
			result = palloc0(22 * sizeof(char));
			snprintf(result, 22, "%ld", value);
			break;
		}
		case BSON_DOUBLE:
		{
			double value = bson_iterator_double(bsonIterator);
			/* Executive decision.  You get 20 digits for floats when
			 * converting to a string.  Want more?  Just use a real double
			 * precision field.
			 */
			result = palloc0(22 * sizeof(char));
			snprintf(result, 22, "%g", value);
			break;
		}
		case BSON_BOOL:
		{
			bool value = bson_iterator_bool(bsonIterator);
			if(value)
			{
				result = "true";
			}
			else
			{
				result = "false";
			}
			break;
		}
		case BSON_OID:
		{
			bson_oid_t *oid = bson_iterator_oid(bsonIterator);
			result = palloc0(25 * sizeof(char));
			bson_oid_to_string(oid, result);
			break;
		}
		default:
		{
			result = "";
			break;
		}
	}
	return result;
}

/*
 * ColumnValue uses column type information to read the current value pointed to
 * by the BSON iterator, and converts this value to the corresponding PostgreSQL
 * datum. The function then returns this datum.
 */
static ColumnValue
CoerceColumnValue(bson_iterator *bsonIterator, const bson_type bsonType, Oid columnTypeId, int32 columnTypeMod)
{
	ColumnValue columnValue = { false, 0 };

	switch(columnTypeId)
	{
		case INT2OID:
		{
			long value;
			if(BsonLong(bsonIterator, bsonType, &value)) {
				columnValue.datum = Int16GetDatum((int16) value);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case INT4OID:
		{
			long value;
			if(BsonLong(bsonIterator, bsonType, &value)) {
				columnValue.datum = Int32GetDatum((int32) value);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case INT8OID:
		{
			long value;
			if(BsonLong(bsonIterator, bsonType, &value)) {
				columnValue.datum = Int64GetDatum((int64) value);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case FLOAT4OID:
		{
			double value;
			if(BsonDouble(bsonIterator, bsonType, &value)) {
				columnValue.datum = Float4GetDatum((float4) value);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case FLOAT8OID:
		{
			double value;
			if(BsonDouble(bsonIterator, bsonType, &value)) {
				columnValue.datum = Float8GetDatum((float8) value);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case NUMERICOID:
		{
			double value;
			if(BsonDouble(bsonIterator, bsonType, &value)) {
				Datum datum = Float8GetDatum((float8) value);
				/* overlook type modifiers for numeric */
				columnValue.datum = DirectFunctionCall1(float8_numeric, datum);
			}
			else
			{
				columnValue.isNull = true;
			}
			break;
		}
		case BOOLOID:
		{
			switch(bsonType)
			{
				case BSON_BOOL:
				case BSON_INT:
				case BSON_LONG:
				case BSON_DOUBLE:
				{
					bool value = bson_iterator_bool(bsonIterator);
					columnValue.datum = BoolGetDatum(value);
					break;
				}
				case BSON_STRING:
				{
					const char *stringValue = bson_iterator_string(bsonIterator);
					if(!stringValue[0] || pg_strcasecmp(stringValue, "f") == 0 ||
						strcmp(stringValue, "false") == 0)
					{
						columnValue.datum = BoolGetDatum(false);
					}
					else if(pg_strcasecmp(stringValue, "t") == 0 ||
							strcmp(stringValue, "true") == 0)
					{
						columnValue.datum = BoolGetDatum(true);
					}
					else
					{
						columnValue.isNull = true;
					}
					break;
				}
				default:
				{
					columnValue.isNull = true;
					break;
				}
			}
			break;
		}
		case BPCHAROID:
		{
			const char *value = BsonString(bsonIterator, bsonType);
			Datum valueDatum = CStringGetDatum(value);

			columnValue.datum = DirectFunctionCall3(bpcharin, valueDatum,
													ObjectIdGetDatum(InvalidOid),
													Int32GetDatum(columnTypeMod));
			break;
		}
		case VARCHAROID:
		{
			const char *value = BsonString(bsonIterator, bsonType);
			Datum valueDatum = CStringGetDatum(value);

			columnValue.datum = DirectFunctionCall3(varcharin, valueDatum,
													ObjectIdGetDatum(InvalidOid),
													Int32GetDatum(columnTypeMod));
			break;
		}
		case TEXTOID:
		{
			const char *value = BsonString(bsonIterator, bsonType);
			columnValue.datum = CStringGetTextDatum(value);
			break;
		}
    	case NAMEOID:
		{
			char value[NAMEDATALEN];
			Datum valueDatum = 0;

			bson_oid_t *bsonObjectId = bson_iterator_oid(bsonIterator);
			bson_oid_to_string(bsonObjectId, value);

			valueDatum = CStringGetDatum(value);
			columnValue.datum = DirectFunctionCall3(namein, valueDatum,
													ObjectIdGetDatum(InvalidOid),
													Int32GetDatum(columnTypeMod));
			break;
		}
		case DATEOID:
		{
			int64 valueMillis = 0;
			switch (bsonType)
			{
				case BSON_DATE:
				{
					valueMillis = bson_iterator_date(bsonIterator);
					break;
				}
				case BSON_OID:
				{
					bson_oid_t *oid = bson_iterator_oid(bsonIterator);
					valueMillis = ((int32) bson_oid_generated_time(oid)) * 1000L;
					break;
				}
				case BSON_INT:
				{
					valueMillis = bson_iterator_long(bsonIterator) * 1000L;
					break;
				}
				case BSON_LONG:
				{
					valueMillis = bson_iterator_long(bsonIterator) * 1000L;
					break;
				}
				case BSON_DOUBLE:
				{
					valueMillis = bson_iterator_double(bsonIterator) * 1000L;
					break;
				}
				default:
				{
					ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
									errmsg("cannot convert bson type to column type"),
									errhint("Column type: %u", (uint32) columnTypeId)));
					break;
				}
			}
			int64 valueMicros = (valueMillis * 1000L);
			int64 timestamp = valueMicros - POSTGRES_TO_UNIX_EPOCH_USECS;
			Datum timestampDatum = TimestampGetDatum(timestamp);

			columnValue.datum = DirectFunctionCall1(timestamp_date, timestampDatum);
			break;
		}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			int64 valueMillis = 0;
			switch (bsonType)
			{
				case BSON_DATE:
				{
					valueMillis = bson_iterator_date(bsonIterator);
					break;
				}
				case BSON_OID:
				{
					bson_oid_t *oid = bson_iterator_oid(bsonIterator);
					valueMillis = ((int32) bson_oid_generated_time(oid)) * 1000L;
					break;
				}
				case BSON_INT:
				{
					valueMillis = bson_iterator_long(bsonIterator) * 1000L;
					break;
				}
				case BSON_LONG:
				{
					valueMillis = bson_iterator_long(bsonIterator) * 1000L;
					break;
				}
				case BSON_DOUBLE:
				{
					valueMillis = bson_iterator_double(bsonIterator) * 1000L;
					break;
				}
				default:
				{
					ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
									errmsg("cannot convert bson type to column type"),
									errhint("Column type: %u", (uint32) columnTypeId)));
					break;
				}
			}
			int64 valueMicros = (valueMillis * 1000L);
			int64 timestamp = valueMicros - POSTGRES_TO_UNIX_EPOCH_USECS;

			/* overlook type modifiers for timestamp */
			columnValue.datum = TimestampGetDatum(timestamp);
			break;
		}
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert bson type to column type"),
							errhint("Column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return columnValue;
}


/*
 * MongoFreeScanState closes the cursor and connection to MongoDB, and reclaims
 * all Mongo related resources allocated for the foreign scan.
 */
static void
MongoFreeScanState(MongoFdwExecState *executionState)
{
	if (executionState == NULL)
	{
		return;
	}

	bson_destroy(executionState->queryDocument);
	bson_dispose(executionState->queryDocument);

	mongo_cursor_destroy(executionState->mongoCursor);
	mongo_cursor_dispose(executionState->mongoCursor);

	if (executionState->arrayCursor)
	{
		ereport(DEBUG2, (errmsg_internal("Freeing arrayCursor")));
		bson_iterator_dispose(executionState->arrayCursor);
	}

	/* also close the connection to mongo server */
	mongo_destroy(executionState->mongoConnection);
	mongo_dispose(executionState->mongoConnection);
}
