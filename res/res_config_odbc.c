/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Copyright (C) 2004 - 2005 Anthony Minessale II <anthmct@yahoo.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief odbc+odbc plugin for portable configuration engine
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Anthony Minessale II <anthmct@yahoo.com>
 *
 * \arg http://www.unixodbc.org
 */

/*** MODULEINFO
	<depend>unixodbc</depend>
	<depend>ltdl</depend>
	<depend>res_odbc</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/res_odbc.h"
#include "asterisk/utils.h"

struct custom_prepare_struct {
	const char *sql;
	const char *extra;
	va_list ap;
	unsigned long long skip;
};

/*!\brief The structures referenced are in include/asterisk/res_odbc.h */
static AST_RWLIST_HEAD_STATIC(odbc_tables, odbc_cache_tables);

static void destroy_table_cache(struct odbc_cache_tables *table) {
	struct odbc_cache_columns *col;
	ast_debug(1, "Destroying table cache for %s\n", table->table);
	AST_RWLIST_WRLOCK(&table->columns);
	while ((col = AST_RWLIST_REMOVE_HEAD(&table->columns, list))) {
		ast_free(col);
	}
	AST_RWLIST_UNLOCK(&table->columns);
	AST_RWLIST_HEAD_DESTROY(&table->columns);
	ast_free(table);
}

#define release_table(ptr) if (ptr) { AST_RWLIST_UNLOCK(&(ptr)->columns); }

/*!
 * \brief Find or create an entry describing the table specified.
 * \param obj An active ODBC handle on which to query the table
 * \param table Tablename to describe
 * \retval A structure describing the table layout, or NULL, if the table is not found or another error occurs.
 * When a structure is returned, the contained columns list will be
 * rdlock'ed, to ensure that it will be retained in memory.
 */
static struct odbc_cache_tables *find_table(const char *database, const char *tablename)
{
	struct odbc_cache_tables *tableptr;
	struct odbc_cache_columns *entry;
	char columnname[80];
	SQLLEN sqlptr;
	SQLHSTMT stmt = NULL;
	int res = 0, error = 0, try = 0;
	struct odbc_obj *obj = ast_odbc_request_obj(database, 0);

	AST_RWLIST_RDLOCK(&odbc_tables);
	AST_RWLIST_TRAVERSE(&odbc_tables, tableptr, list) {
		if (strcmp(tableptr->connection, database) == 0 && strcmp(tableptr->table, tablename) == 0) {
			break;
		}
	}
	if (tableptr) {
		AST_RWLIST_RDLOCK(&tableptr->columns);
		AST_RWLIST_UNLOCK(&odbc_tables);
		return tableptr;
	}

	if (!obj) {
		ast_log(LOG_WARNING, "Unable to retrieve database handle for table description '%s@%s'\n", tablename, database);
		return NULL;
	}

	/* Table structure not already cached; build it now. */
	do {
retry:
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			if (try == 0) {
				try = 1;
				ast_odbc_sanity_check(obj);
				goto retry;
			}
			ast_log(LOG_WARNING, "SQL Alloc Handle failed on connection '%s'!\n", database);
			break;
		}

		res = SQLColumns(stmt, NULL, 0, NULL, 0, (unsigned char *)tablename, SQL_NTS, (unsigned char *)"%", SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			if (try == 0) {
				try = 1;
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				ast_odbc_sanity_check(obj);
				goto retry;
			}
			ast_log(LOG_ERROR, "Unable to query database columns on connection '%s'.\n", database);
			break;
		}

		if (!(tableptr = ast_calloc(sizeof(char), sizeof(*tableptr) + strlen(database) + 1 + strlen(tablename) + 1))) {
			ast_log(LOG_ERROR, "Out of memory creating entry for table '%s' on connection '%s'\n", tablename, database);
			break;
		}

		tableptr->connection = (char *)tableptr + sizeof(*tableptr);
		tableptr->table = (char *)tableptr + sizeof(*tableptr) + strlen(database) + 1;
		strcpy(tableptr->connection, database); /* SAFE */
		strcpy(tableptr->table, tablename); /* SAFE */
		AST_RWLIST_HEAD_INIT(&(tableptr->columns));

		while ((res = SQLFetch(stmt)) != SQL_NO_DATA && res != SQL_ERROR) {
			SQLGetData(stmt,  4, SQL_C_CHAR, columnname, sizeof(columnname), &sqlptr);

			if (!(entry = ast_calloc(sizeof(char), sizeof(*entry) + strlen(columnname) + 1))) {
				ast_log(LOG_ERROR, "Out of memory creating entry for column '%s' in table '%s' on connection '%s'\n", columnname, tablename, database);
				error = 1;
				break;
			}
			entry->name = (char *)entry + sizeof(*entry);
			strcpy(entry->name, columnname);

			SQLGetData(stmt,  5, SQL_C_SHORT, &entry->type, sizeof(entry->type), NULL);
			SQLGetData(stmt,  7, SQL_C_LONG, &entry->size, sizeof(entry->size), NULL);
			SQLGetData(stmt,  9, SQL_C_SHORT, &entry->decimals, sizeof(entry->decimals), NULL);
			SQLGetData(stmt, 10, SQL_C_SHORT, &entry->radix, sizeof(entry->radix), NULL);
			SQLGetData(stmt, 11, SQL_C_SHORT, &entry->nullable, sizeof(entry->nullable), NULL);
			SQLGetData(stmt, 16, SQL_C_LONG, &entry->octetlen, sizeof(entry->octetlen), NULL);

			/* Specification states that the octenlen should be the maximum number of bytes
			 * returned in a char or binary column, but it seems that some drivers just set
			 * it to NULL. (Bad Postgres! No biscuit!) */
			if (entry->octetlen == 0) {
				entry->octetlen = entry->size;
			}

			ast_verb(10, "Found %s column with type %hd with len %ld, octetlen %ld, and numlen (%hd,%hd)\n", entry->name, entry->type, (long) entry->size, (long) entry->octetlen, entry->decimals, entry->radix);
			/* Insert column info into column list */
			AST_LIST_INSERT_TAIL(&(tableptr->columns), entry, list);
		}
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);

		AST_RWLIST_INSERT_TAIL(&odbc_tables, tableptr, list);
		AST_RWLIST_RDLOCK(&(tableptr->columns));
	} while (0);

	AST_RWLIST_UNLOCK(&odbc_tables);

	if (error) {
		destroy_table_cache(tableptr);
		tableptr = NULL;
	}
	if (obj) {
		ast_odbc_release_obj(obj);
	}
	return tableptr;
}

static struct odbc_cache_columns *find_column(struct odbc_cache_tables *table, const char *colname)
{
	struct odbc_cache_columns *col;
	AST_RWLIST_TRAVERSE(&table->columns, col, list) {
		if (strcasecmp(col->name, colname) == 0) {
			return col;
		}
	}
	return NULL;
}

static SQLHSTMT custom_prepare(struct odbc_obj *obj, void *data)
{
	int res, x = 1, count = 0;
	struct custom_prepare_struct *cps = data;
	const char *newparam, *newval;
	SQLHSTMT stmt;
	va_list ap;

	va_copy(ap, cps->ap);

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLPrepare(stmt, (unsigned char *)cps->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", cps->sql);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if ((1 << count) & cps->skip) {
			continue;
		}
		SQLBindParameter(stmt, x++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(newval), 0, (void *)newval, 0, NULL);
	}
	va_end(ap);

	if (!ast_strlen_zero(cps->extra))
		SQLBindParameter(stmt, x++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(cps->extra), 0, (void *)cps->extra, 0, NULL);
	return stmt;
}

/*!
 * \brief Excute an SQL query and return ast_variable list
 * \param database
 * \param table
 * \param ap list containing one or more field/operator/value set.
 *
 * Select database and preform query on table, prepare the sql statement
 * Sub-in the values to the prepared statement and execute it. Return results
 * as a ast_variable list.
 *
 * \retval var on success
 * \retval NULL on failure
*/
static struct ast_variable *realtime_odbc(const char *database, const char *table, va_list ap)
{
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	char sql[1024];
	char coltitle[256];
	char rowdata[2048];
	char *op;
	const char *newparam, *newval;
	char *stringp;
	char *chunk;
	SQLSMALLINT collen;
	int res;
	int x;
	struct ast_variable *var=NULL, *prev=NULL;
	SQLULEN colsize;
	SQLSMALLINT colcount=0;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLLEN indicator;
	va_list aq;
	struct custom_prepare_struct cps = { .sql = sql };

	va_copy(cps.ap, ap);
	va_copy(aq, ap);

	if (!table)
		return NULL;

	obj = ast_odbc_request_obj(database, 0);

	if (!obj) {
		ast_log(LOG_ERROR, "No database handle available with the name of '%s' (check res_odbc.conf)\n", database);
		return NULL;
	}

	newparam = va_arg(aq, const char *);
	if (!newparam)
		return NULL;
	newval = va_arg(aq, const char *);
	op = !strchr(newparam, ' ') ? " =" : "";
	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s ?%s", table, newparam, op,
		strcasestr(newparam, "LIKE") && !ast_odbc_backslash_is_escape(obj) ? " ESCAPE '\\'" : "");
	while((newparam = va_arg(aq, const char *))) {
		op = !strchr(newparam, ' ') ? " =" : "";
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s ?%s", newparam, op,
			strcasestr(newparam, "LIKE") && !ast_odbc_backslash_is_escape(obj) ? " ESCAPE '\\'" : "");
		newval = va_arg(aq, const char *);
	}
	va_end(aq);

	stmt = ast_odbc_prepare_and_execute(obj, custom_prepare, &cps);

	if (!stmt) {
		ast_odbc_release_obj(obj);
		return NULL;
	}

	res = SQLNumResultCols(stmt, &colcount);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}

	res = SQLFetch(stmt);
	if (res == SQL_NO_DATA) {
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}
	for (x = 0; x < colcount; x++) {
		rowdata[0] = '\0';
		collen = sizeof(coltitle);
		res = SQLDescribeCol(stmt, x + 1, (unsigned char *)coltitle, sizeof(coltitle), &collen, 
					&datatype, &colsize, &decimaldigits, &nullable);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
			if (var)
				ast_variables_destroy(var);
			ast_odbc_release_obj(obj);
			return NULL;
		}

		indicator = 0;
		res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), &indicator);
		if (indicator == SQL_NULL_DATA)
			rowdata[0] = '\0';
		else if (ast_strlen_zero(rowdata)) {
			/* Because we encode the empty string for a NULL, we will encode
			 * actual empty strings as a string containing a single whitespace. */
			ast_copy_string(rowdata, " ", sizeof(rowdata));
		}

		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			if (var)
				ast_variables_destroy(var);
			ast_odbc_release_obj(obj);
			return NULL;
		}
		stringp = rowdata;
		while(stringp) {
			chunk = strsep(&stringp, ";");
			if (!ast_strlen_zero(ast_strip(chunk))) {
				if (prev) {
					prev->next = ast_variable_new(coltitle, chunk, "");
					if (prev->next)
						prev = prev->next;
				} else 
					prev = var = ast_variable_new(coltitle, chunk, "");
			}
		}
	}


	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);
	return var;
}

/*!
 * \brief Excute an Select query and return ast_config list
 * \param database
 * \param table
 * \param ap list containing one or more field/operator/value set.
 *
 * Select database and preform query on table, prepare the sql statement
 * Sub-in the values to the prepared statement and execute it. 
 * Execute this prepared query against several ODBC connected databases.
 * Return results as an ast_config variable.
 *
 * \retval var on success
 * \retval NULL on failure
*/
static struct ast_config *realtime_multi_odbc(const char *database, const char *table, va_list ap)
{
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	char sql[1024];
	char coltitle[256];
	char rowdata[2048];
	const char *initfield=NULL;
	char *op;
	const char *newparam, *newval;
	char *stringp;
	char *chunk;
	SQLSMALLINT collen;
	int res;
	int x;
	struct ast_variable *var=NULL;
	struct ast_config *cfg=NULL;
	struct ast_category *cat=NULL;
	SQLULEN colsize;
	SQLSMALLINT colcount=0;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLLEN indicator;
	struct custom_prepare_struct cps = { .sql = sql };
	va_list aq;

	va_copy(cps.ap, ap);
	va_copy(aq, ap);

	if (!table)
		return NULL;

	obj = ast_odbc_request_obj(database, 0);
	if (!obj)
		return NULL;

	newparam = va_arg(aq, const char *);
	if (!newparam)  {
		ast_odbc_release_obj(obj);
		return NULL;
	}
	initfield = ast_strdupa(newparam);
	if ((op = strchr(initfield, ' '))) 
		*op = '\0';
	newval = va_arg(aq, const char *);
	op = !strchr(newparam, ' ') ? " =" : "";
	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s ?%s", table, newparam, op,
		strcasestr(newparam, "LIKE") && !ast_odbc_backslash_is_escape(obj) ? " ESCAPE '\\'" : "");
	while((newparam = va_arg(aq, const char *))) {
		op = !strchr(newparam, ' ') ? " =" : "";
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s ?%s", newparam, op,
			strcasestr(newparam, "LIKE") && !ast_odbc_backslash_is_escape(obj) ? " ESCAPE '\\'" : "");
		newval = va_arg(aq, const char *);
	}
	if (initfield)
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " ORDER BY %s", initfield);
	va_end(aq);

	stmt = ast_odbc_prepare_and_execute(obj, custom_prepare, &cps);

	if (!stmt) {
		ast_odbc_release_obj(obj);
		return NULL;
	}

	res = SQLNumResultCols(stmt, &colcount);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}

	cfg = ast_config_new();
	if (!cfg) {
		ast_log(LOG_WARNING, "Out of memory!\n");
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}

	while ((res=SQLFetch(stmt)) != SQL_NO_DATA) {
		var = NULL;
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			continue;
		}
		cat = ast_category_new("","",99999);
		if (!cat) {
			ast_log(LOG_WARNING, "Out of memory!\n");
			continue;
		}
		for (x=0;x<colcount;x++) {
			rowdata[0] = '\0';
			collen = sizeof(coltitle);
			res = SQLDescribeCol(stmt, x + 1, (unsigned char *)coltitle, sizeof(coltitle), &collen, 
						&datatype, &colsize, &decimaldigits, &nullable);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
				ast_category_destroy(cat);
				continue;
			}

			indicator = 0;
			res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), &indicator);
			if (indicator == SQL_NULL_DATA)
				continue;

			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
				ast_category_destroy(cat);
				continue;
			}
			stringp = rowdata;
			while(stringp) {
				chunk = strsep(&stringp, ";");
				if (!ast_strlen_zero(ast_strip(chunk))) {
					if (initfield && !strcmp(initfield, coltitle))
						ast_category_rename(cat, chunk);
					var = ast_variable_new(coltitle, chunk, "");
					ast_variable_append(cat, var);
				}
			}
		}
		ast_category_append(cfg, cat);
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);
	return cfg;
}

/*!
 * \brief Excute an UPDATE query
 * \param database
 * \param table
 * \param keyfield where clause field
 * \param lookup value of field for where clause
 * \param ap list containing one or more field/value set(s).
 *
 * Update a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int update_odbc(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	char sql[256];
	SQLLEN rowcount=0;
	const char *newparam, *newval;
	int res, count = 0;
	va_list aq;
	struct custom_prepare_struct cps = { .sql = sql, .extra = lookup };
	struct odbc_cache_tables *tableptr = find_table(database, table);
	struct odbc_cache_columns *column;

	va_copy(cps.ap, ap);
	va_copy(aq, ap);
	
	if (!table) {
		release_table(tableptr);
		return -1;
	}

	obj = ast_odbc_request_obj(database, 0);
	if (!obj) {
		release_table(tableptr);
		return -1;
	}

	newparam = va_arg(aq, const char *);
	if (!newparam)  {
		ast_odbc_release_obj(obj);
		release_table(tableptr);
		return -1;
	}
	newval = va_arg(aq, const char *);

	if (tableptr && !(column = find_column(tableptr, newparam))) {
		ast_log(LOG_WARNING, "Key field '%s' does not exist in table '%s@%s'.  Update will fail\n", newparam, table, database);
	}

	snprintf(sql, sizeof(sql), "UPDATE %s SET %s=?", table, newparam);
	while((newparam = va_arg(aq, const char *))) {
		if ((tableptr && (column = find_column(tableptr, newparam))) || count > 63) {
			snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), ", %s=?", newparam);
			newval = va_arg(aq, const char *);
		} else { /* the column does not exist in the table OR we've exceeded the space in our flag field */
			cps.skip |= (((long long)1) << count);
		}
		count++;
	}
	va_end(aq);
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " WHERE %s=?", keyfield);
	release_table(tableptr);

	stmt = ast_odbc_prepare_and_execute(obj, custom_prepare, &cps);

	if (!stmt) {
		ast_odbc_release_obj(obj);
		return -1;
	}

	res = SQLRowCount(stmt, &rowcount);
	SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Row Count error!\n[%s]\n\n", sql);
		return -1;
	}

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

/*!
 * \brief Excute an INSERT query
 * \param database
 * \param table
 * \param ap list containing one or more field/value set(s)
 *
 * Insert a new record into database table, prepare the sql statement.
 * All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int store_odbc(const char *database, const char *table, va_list ap)
{
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	char sql[256];
	char keys[256];
	char vals[256];
	SQLLEN rowcount=0;
	const char *newparam, *newval;
	int res;
	va_list aq;
	struct custom_prepare_struct cps = { .sql = sql, .extra = NULL };

	va_copy(cps.ap, ap);
	va_copy(aq, ap);
	
	if (!table)
		return -1;

	obj = ast_odbc_request_obj(database, 0);
	if (!obj)
		return -1;

	newparam = va_arg(aq, const char *);
	if (!newparam)  {
		ast_odbc_release_obj(obj);
		return -1;
	}
	newval = va_arg(aq, const char *);
	snprintf(keys, sizeof(keys), "%s", newparam);
	ast_copy_string(vals, "?", sizeof(vals));
	while ((newparam = va_arg(aq, const char *))) {
		snprintf(keys + strlen(keys), sizeof(keys) - strlen(keys), ", %s", newparam);
		snprintf(vals + strlen(vals), sizeof(vals) - strlen(vals), ", ?");
		newval = va_arg(aq, const char *);
	}
	va_end(aq);
	snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s)", table, keys, vals);

	stmt = ast_odbc_prepare_and_execute(obj, custom_prepare, &cps);

	if (!stmt) {
		ast_odbc_release_obj(obj);
		return -1;
	}

	res = SQLRowCount(stmt, &rowcount);
	SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Row Count error!\n[%s]\n\n", sql);
		return -1;
	}

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

/*!
 * \brief Excute an DELETE query
 * \param database
 * \param table
 * \param keyfield where clause field
 * \param lookup value of field for where clause
 * \param ap list containing one or more field/value set(s)
 *
 * Delete a row from a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. Additional params to match rows are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int destroy_odbc(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	char sql[256];
	SQLLEN rowcount=0;
	const char *newparam, *newval;
	int res;
	va_list aq;
	struct custom_prepare_struct cps = { .sql = sql, .extra = lookup };

	va_copy(cps.ap, ap);
	va_copy(aq, ap);
	
	if (!table)
		return -1;

	obj = ast_odbc_request_obj(database, 0);
	if (!obj)
		return -1;

	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE ", table);
	while((newparam = va_arg(aq, const char *))) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "%s=? AND ", newparam);
		newval = va_arg(aq, const char *);
	}
	va_end(aq);
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "%s=?", keyfield);

	stmt = ast_odbc_prepare_and_execute(obj, custom_prepare, &cps);

	if (!stmt) {
		ast_odbc_release_obj(obj);
		return -1;
	}

	res = SQLRowCount(stmt, &rowcount);
	SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Row Count error!\n[%s]\n\n", sql);
		return -1;
	}

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}


struct config_odbc_obj {
	char *sql;
	unsigned long cat_metric;
	char category[128];
	char var_name[128];
	char var_val[1024]; /* changed from 128 to 1024 via bug 8251 */
	SQLLEN err;
};

static SQLHSTMT config_odbc_prepare(struct odbc_obj *obj, void *data)
{
	struct config_odbc_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_ULONG, &q->cat_metric, sizeof(q->cat_metric), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->category, sizeof(q->category), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->var_name, sizeof(q->var_name), &q->err);
	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

static struct ast_config *config_odbc(const char *database, const char *table, const char *file, struct ast_config *cfg, struct ast_flags flags, const char *sugg_incl, const char *who_asked)
{
	struct ast_variable *new_v;
	struct ast_category *cur_cat;
	int res = 0;
	struct odbc_obj *obj;
	char sqlbuf[1024] = "";
	char *sql = sqlbuf;
	size_t sqlleft = sizeof(sqlbuf);
	unsigned int last_cat_metric = 0;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	char last[128] = "";
	struct config_odbc_obj q;
	struct ast_flags loader_flags = { 0 };

	memset(&q, 0, sizeof(q));

	if (!file || !strcmp (file, "res_config_odbc.conf"))
		return NULL;		/* cant configure myself with myself ! */

	obj = ast_odbc_request_obj(database, 0);
	if (!obj)
		return NULL;

	ast_build_string(&sql, &sqlleft, "SELECT cat_metric, category, var_name, var_val FROM %s ", table);
	ast_build_string(&sql, &sqlleft, "WHERE filename='%s' AND commented=0 ", file);
	ast_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = ast_odbc_prepare_and_execute(obj, config_odbc_prepare, &q);

	if (!stmt) {
		ast_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		ast_odbc_release_obj(obj);
		return NULL;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		return NULL;
	}

	if (!rowcount) {
		ast_log(LOG_NOTICE, "found nothing\n");
		ast_odbc_release_obj(obj);
		return cfg;
	}

	cur_cat = ast_config_get_current_category(cfg);

	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		if (!strcmp (q.var_name, "#include")) {
			if (!ast_config_internal_load(q.var_val, cfg, loader_flags, "", who_asked)) {
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				ast_odbc_release_obj(obj);
				return NULL;
			}
			continue;
		} 
		if (strcmp(last, q.category) || last_cat_metric != q.cat_metric) {
			cur_cat = ast_category_new(q.category, "", 99999);
			if (!cur_cat) {
				ast_log(LOG_WARNING, "Out of memory!\n");
				break;
			}
			strcpy(last, q.category);
			last_cat_metric	= q.cat_metric;
			ast_category_append(cfg, cur_cat);
		}

		new_v = ast_variable_new(q.var_name, q.var_val, "");
		ast_variable_append(cur_cat, new_v);
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);
	return cfg;
}

#define warn_length(col, size)	ast_log(LOG_WARNING, "Realtime table %s@%s: column '%s' is not long enough to contain realtime data (needs %d)\n", table, database, col->name, size)
#define warn_type(col, type)	ast_log(LOG_WARNING, "Realtime table %s@%s: column '%s' is of the incorrect type (%d) to contain the required realtime data\n", table, database, col->name, col->type)

static int require_odbc(const char *database, const char *table, va_list ap)
{
	struct odbc_cache_tables *tableptr = find_table(database, table);
	struct odbc_cache_columns *col;
	char *elm;
	int type, size;

	if (!tableptr) {
		return -1;
	}

	while ((elm = va_arg(ap, char *))) {
		type = va_arg(ap, require_type);
		size = va_arg(ap, int);
		/* Check if the field matches the criteria */
		AST_RWLIST_TRAVERSE(&tableptr->columns, col, list) {
			if (strcmp(col->name, elm) == 0) {
				/* Type check, first.  Some fields are more particular than others */
				switch (col->type) {
				case SQL_CHAR:
				case SQL_VARCHAR:
				case SQL_LONGVARCHAR:
				case SQL_BINARY:
				case SQL_VARBINARY:
				case SQL_LONGVARBINARY:
				case SQL_GUID:
#define CHECK_SIZE(n) \
						if (col->size < n) {      \
							warn_length(col, n);  \
						}                         \
						break;
					switch (type) {
					case RQ_UINTEGER1: CHECK_SIZE(3)  /*         255 */
					case RQ_INTEGER1:  CHECK_SIZE(4)  /*        -128 */
					case RQ_UINTEGER2: CHECK_SIZE(5)  /*       65535 */
					case RQ_INTEGER2:  CHECK_SIZE(6)  /*      -32768 */
					case RQ_UINTEGER3:                /*    16777215 */
					case RQ_INTEGER3:  CHECK_SIZE(8)  /*    -8388608 */
					case RQ_DATE:                     /*  2008-06-09 */
					case RQ_UINTEGER4: CHECK_SIZE(10) /*  4200000000 */
					case RQ_INTEGER4:  CHECK_SIZE(11) /* -2100000000 */
					case RQ_DATETIME:                 /* 2008-06-09 16:03:47 */
					case RQ_UINTEGER8: CHECK_SIZE(19) /* trust me    */
					case RQ_INTEGER8:  CHECK_SIZE(20) /* ditto       */
					case RQ_FLOAT:
					case RQ_CHAR:      CHECK_SIZE(size)
					}
#undef CHECK_SIZE
					break;
				case SQL_TYPE_DATE:
					if (type != RQ_DATE) {
						warn_type(col, type);
					}
					break;
				case SQL_TYPE_TIMESTAMP:
				case SQL_TIMESTAMP:
					if (type != RQ_DATE && type != RQ_DATETIME) {
						warn_type(col, type);
					}
					break;
				case SQL_BIT:
					warn_length(col, size);
					break;
#define WARN_TYPE_OR_LENGTH(n)	\
						if (!ast_rq_is_int(type)) {  \
							warn_type(col, type);    \
						} else {                     \
							warn_length(col, n);  \
						}
				case SQL_TINYINT:
					if (type != RQ_UINTEGER1) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_C_STINYINT:
					if (type != RQ_INTEGER1) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_C_USHORT:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 && type != RQ_UINTEGER2) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_SMALLINT:
				case SQL_C_SSHORT:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 && type != RQ_INTEGER2) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_C_ULONG:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 &&
						type != RQ_UINTEGER2 && type != RQ_INTEGER2 &&
						type != RQ_UINTEGER3 && type != RQ_INTEGER3 &&
						type != RQ_INTEGER4) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_INTEGER:
				case SQL_C_SLONG:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 &&
						type != RQ_UINTEGER2 && type != RQ_INTEGER2 &&
						type != RQ_UINTEGER3 && type != RQ_INTEGER3 &&
						type != RQ_UINTEGER4) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_C_UBIGINT:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 &&
						type != RQ_UINTEGER2 && type != RQ_INTEGER2 &&
						type != RQ_UINTEGER3 && type != RQ_INTEGER3 &&
						type != RQ_UINTEGER4 && type != RQ_INTEGER4 &&
						type != RQ_INTEGER8) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
				case SQL_BIGINT:
				case SQL_C_SBIGINT:
					if (type != RQ_UINTEGER1 && type != RQ_INTEGER1 &&
						type != RQ_UINTEGER2 && type != RQ_INTEGER2 &&
						type != RQ_UINTEGER3 && type != RQ_INTEGER3 &&
						type != RQ_UINTEGER4 && type != RQ_INTEGER4 &&
						type != RQ_UINTEGER8) {
						WARN_TYPE_OR_LENGTH(size)
					}
					break;
#undef WARN_TYPE_OR_LENGTH
				case SQL_NUMERIC:
				case SQL_DECIMAL:
				case SQL_FLOAT:
				case SQL_REAL:
				case SQL_DOUBLE:
					if (!ast_rq_is_int(type) && type != RQ_FLOAT) {
						warn_type(col, type);
					}
					break;
				default:
					ast_log(LOG_WARNING, "Realtime table %s@%s: column type (%d) unrecognized for column '%s'\n", table, database, col->type, elm);
				}
				break;
			}
		}
		if (!col) {
			ast_log(LOG_WARNING, "Realtime table %s@%s requires column '%s', but that column does not exist!\n", table, database, elm);
		}
	}
	va_end(ap);
	AST_RWLIST_UNLOCK(&tableptr->columns);
	return 0;
}
#undef warn_length
#undef warn_type

static int unload_odbc(const char *database, const char *tablename)
{
	struct odbc_cache_tables *tableptr;

	AST_RWLIST_RDLOCK(&odbc_tables);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&odbc_tables, tableptr, list) {
		if (strcmp(tableptr->connection, database) == 0 && strcmp(tableptr->table, tablename) == 0) {
			AST_LIST_REMOVE_CURRENT(list);
			destroy_table_cache(tableptr);
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END
	AST_RWLIST_UNLOCK(&odbc_tables);
	return tableptr ? 0 : -1;
}

static struct ast_config_engine odbc_engine = {
	.name = "odbc",
	.load_func = config_odbc,
	.realtime_func = realtime_odbc,
	.realtime_multi_func = realtime_multi_odbc,
	.store_func = store_odbc,
	.destroy_func = destroy_odbc,
	.update_func = update_odbc,
	.require_func = require_odbc,
	.unload_func = unload_odbc,
};

static int unload_module (void)
{
	struct odbc_cache_tables *table;

	ast_config_engine_deregister(&odbc_engine);

	/* Empty the cache */
	AST_RWLIST_WRLOCK(&odbc_tables);
	while ((table = AST_RWLIST_REMOVE_HEAD(&odbc_tables, list))) {
		destroy_table_cache(table);
	}
	AST_RWLIST_UNLOCK(&odbc_tables);

	ast_verb(1, "res_config_odbc unloaded.\n");
	return 0;
}

static int load_module (void)
{
	ast_config_engine_register(&odbc_engine);
	ast_verb(1, "res_config_odbc loaded.\n");
	return 0;
}

static int reload_module(void)
{
	struct odbc_cache_tables *table;

	/* Empty the cache; it will get rebuilt the next time the tables are needed. */
	AST_RWLIST_WRLOCK(&odbc_tables);
	while ((table = AST_RWLIST_REMOVE_HEAD(&odbc_tables, list))) {
		destroy_table_cache(table);
	}
	AST_RWLIST_UNLOCK(&odbc_tables);

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS, "Realtime ODBC configuration",
		.load = load_module,
		.unload = unload_module,
		.reload = reload_module,
		);
