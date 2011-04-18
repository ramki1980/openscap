/**
 * @file   sql57.c
 * @brief  sql57 probe
 * @author "Daniel Kopecek" <dkopecek@redhat.com>
 *
 */

/*
 * Copyright 2011 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      "Daniel Kopecek" <dkopecek@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <seap.h>
#include <probe-api.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <common/assume.h>
#include <common/debug_priv.h>
#include <common/bfind.h>
#include <time.h>
#include <errno.h>
#include <opendbx/api.h>

#ifndef SQLPROBE_DEFAULT_CONNTIMEOUT
# define SQLPROBE_DEFAULT_CONNTIMEOUT 30
#endif

void *probe_init(void)
{
	return (NULL);
}

void probe_fini(void *arg)
{
	return;
}

typedef struct {
	char *o_engine; /* object engine  */
	char *b_engine; /* backend engine */
} dbEngineMap_t;

dbEngineMap_t engine_map[] = {
	{ "access",    NULL       },
	{ "db2",       NULL       },
	{ "cache",     NULL       },
	{ "firebird",  "firebird" },
	{ "firstsql",  NULL       },
	{ "foxpro",    NULL       },
	{ "informix",  NULL       },
	{ "ingres",    NULL       },
	{ "interbase", NULL       },
	{ "lightbase", NULL       },
	{ "maxdb",     NULL       },
	{ "monetdb",   NULL       },
	{ "mimer",     NULL       },
	{ "mssql",     "mssql"    }, /* non-standard */
	{ "mysql",     "mysql"    }, /* non-standard */
	{ "oracle",    "oracle"   },
	{ "paradox",   NULL       },
	{ "pervasive", NULL       },
	{ "postgre",   "pgsql"    },
	{ "sqlbase",   NULL       },
	{ "sqlite",    "sqlite"   },
	{ "sqlite3",   "sqlite3"  }, /* non-standard */
	{ "sqlserver", NULL       },
	{ "sybase",    "sybase"   }
};

#define ENGINE_MAP_COUNT (sizeof engine_map / sizeof (dbEngineMap_t))

static int engine_cmp (const dbEngineMap_t *a, const dbEngineMap_t *b)
{
	assume_d(a != NULL, -1);
	assume_d(b != NULL, -1);
	return strcmp(a->o_engine, b->o_engine);
}

typedef struct {
	char    *host;
	char    *port;
	char    *user;
	char    *pass;
	char    *db;
	long     conn_timeout;
} dbURIInfo_t;

static void __clearmem(void *ptr, int len)
{
	if (ptr != NULL) {
		do {
			register int       l = len / sizeof(uint32_t) - 1;
			register uint32_t *p = (uint32_t *)ptr;

			while (l >= 0) {
				p[l] = (uint32_t)random();
				--l;
			}
		} while (0);

		do {
			register int      l = len % sizeof(uint32_t);
			register uint8_t *p = (uint8_t *)ptr;

			switch (l) {
			case 3:
				p[2] = (uint8_t)(random() % (1 << 8));
			case 2:
				p[1] = (uint8_t)(random() % (1 << 8));
			case 1:
				p[0] = (uint8_t)(random() % (1 << 8));
			}
		} while (0);
	}
	return;
}

static void dbURIInfo_clear(dbURIInfo_t *ui)
{
	if (ui == NULL)
		return;

	srandom((long)clock()^(long)(ui->pass));

	if (ui->host != NULL) {
		__clearmem(ui->host, strlen(ui->host));
		oscap_free(ui->host);
	}

	if (ui->user != NULL) {
		__clearmem(ui->user, strlen(ui->user));
		oscap_free(ui->user);
	}

	if (ui->pass != NULL) {
		__clearmem(ui->pass, strlen(ui->pass));
		oscap_free(ui->pass);
	}

	if (ui->db != NULL) {
		__clearmem(ui->db, strlen(ui->db));
		oscap_free(ui->db);
	}

	ui->host = NULL;
	ui->port = 0;
	ui->user = NULL;
	ui->pass = NULL;
	ui->db   = NULL;

	return;
}

static int dbURIInfo_parse(dbURIInfo_t *info, const char *conn)
{
	char *tmp, *tok, *copy = strdup(conn);

	if (copy == NULL)
		return (-1);

#define skipspace(s) while (isspace(*s)) ++s;

#define matchitem1(tok, first, rest, dst)			\
	case first:						\
		if (strcasecmp((rest), ++(tok)) == 0) {		\
			tok += strlen(rest);			\
			skipspace(tok);				\
			if (*(tok) != '=') goto __fail;		\
			else (dst) = strdup((tok) + 1);		\
		}						\
	while(0)

#define matchitem2(tok, first, rest1, dst1, rest2, dst2)		\
	case first:							\
		if (strcasecmp((rest1), (tok)+1) == 0) {		\
			tok += 1+strlen(rest1);				\
			skipspace(tok);					\
			if (*(tok) != '=') goto __fail;			\
			else (dst1) = strdup((tok) + 1);		\
		}							\
		else if (strcasecmp((rest2), (tok)+1) == 0) {		\
			tok += 1+strlen(rest2);				\
			skipspace(tok);					\
			if (*(tok) != '=') goto __fail;			\
			else (dst2) = strdup((tok) + 1);		\
		}							\
		while(0)

	tmp = NULL;

	while ((tok = strsep (&copy, ";")) != NULL) {
		switch (tolower(*tok)) {
			matchitem1(tok, 's',
				  "erver", info->host); break;
			matchitem2(tok, 'p',
				   "ort", info->port,
				   "wd",  info->pass); break;
			matchitem1(tok, 'd',
				   "atabase", info->db); break;
			matchitem1(tok, 'u',
				   "id",info->user); break;
			matchitem1(tok, 'c',
				   "onnecttimeout", tmp);
			if (tmp != NULL) {
				info->conn_timeout = strtol(tmp, NULL, 10);

				if (errno == ERANGE || errno == EINVAL)
					info->conn_timeout = SQLPROBE_DEFAULT_CONNTIMEOUT;

				oscap_free(tmp);
			}
			break;
		}
	}

	oscap_free(copy);
	return (0);
__fail:
	oscap_free(copy);
	return (-1);
}

static int dbSQL_eval(const char *engine, const char *version,
                      const char *conn, const char *sql, SEXP_t *probe_out)
{
	int err = -1;
	dbURIInfo_t uriInfo = { .host = NULL,
				.port = 0,
				.user = NULL,
				.pass = NULL,
				.db   = NULL};

	/*
	 * parse the connection string
	 */
	if (dbURIInfo_parse(&uriInfo, conn) != 0) {
		dE("Malformed connection string: %s\n", conn);
		goto __exit;
	} else {
		int            sql_err = 0;
		odbx_t        *sql_dbh = NULL; /* handle */
		dbEngineMap_t *sql_dbe; /* engine */
		odbx_result_t *sql_dbr; /* result */
		SEXP_t        *item;

		sql_dbe = oscap_bfind (engine_map, ENGINE_MAP_COUNT, sizeof(dbEngineMap_t), (char *)engine,
				       (int(*)(void *, void *))&engine_cmp);

		if (sql_dbe == NULL) {
			dE("DB engine not found: %s\n", engine);
			goto __exit;
		}

		if (sql_dbe->b_engine == NULL) {
			dE("DB engine not supported: %s\n", engine);
			goto __exit;
		}

		if (odbx_init (&sql_dbh, sql_dbe->b_engine,
			       uriInfo.host, uriInfo.port) != ODBX_ERR_SUCCESS)
		{
			dE("odbx_init failed: e=%s, h=%s:%s\n",
			   sql_dbe->b_engine, uriInfo.host, uriInfo.port);
			goto __exit;
		}

		/* set options */

		if (odbx_bind (sql_dbh, uriInfo.db, uriInfo.user, uriInfo.pass,
			       ODBX_BIND_SIMPLE) != ODBX_ERR_SUCCESS)
		{
			dE("odbx_bind failed: db=%s, u=%s, p=%s\n",
			   uriInfo.db, uriInfo.user, uriInfo.pass);
			goto __exit;
		}

		if (odbx_query(sql_dbh, sql, strlen (sql)) != ODBX_ERR_SUCCESS) {
			dE("odbx_query failed: q=%s\n", sql);
			odbx_finish(sql_dbh);

			goto __exit;
		} else {
			sql_dbr = NULL;
                        item    = probe_item_create(OVAL_INDEPENDENT_SQL57, NULL,
                                                    "engine",            OVAL_DATATYPE_STRING, engine,
                                                    "version",           OVAL_DATATYPE_STRING, version,
                                                    "sql",               OVAL_DATATYPE_STRING, sql,
                                                    "connection_string", OVAL_DATATYPE_STRING, conn,
                                                    NULL);

			while ((sql_err = odbx_result (sql_dbh, &sql_dbr, NULL, 0)) == ODBX_RES_ROWS) {
				while (odbx_row_fetch(sql_dbr) == ODBX_ROW_NEXT) {
                                        SEXP_t se_tmp_mem, *field, *result;
                                        const char *col_val, *col_name;
                                        oval_datatype_t col_type;
                                        unsigned int ci;

                                        result = probe_ent_creat1("result", NULL, NULL);
                                        probe_ent_setdatatype(result, OVAL_DATATYPE_RECORD);

                                        for (ci = 0; ci < odbx_column_count(sql_dbr); ++ci) {
                                                col_val  = odbx_field_value (sql_dbr, ci);
                                                col_name = odbx_column_name (sql_dbr, ci);
                                                col_type = OVAL_DATATYPE_UNKNOWN;
                                                field    = NULL;

                                                switch(odbx_column_type(sql_dbr, ci)) {
                                                case ODBX_TYPE_BOOLEAN:
                                                        break;
                                                case ODBX_TYPE_INTEGER:
                                                case ODBX_TYPE_SMALLINT: {
                                                        char *end = NULL;
                                                        int   val = strtol(col_val, &end, 10);

                                                        if (val == 0 && (end == col_val))
                                                                dE("strtol(%s) failed\n", col_val);

                                                        field    = probe_ent_creat1("field", NULL, SEXP_number_newi_r(&se_tmp_mem, val));
                                                        col_type = OVAL_DATATYPE_INTEGER;
                                                        SEXP_free_r(&se_tmp_mem);

                                                }       break;
                                                case ODBX_TYPE_REAL:
                                                case ODBX_TYPE_DOUBLE:
                                                case ODBX_TYPE_FLOAT: {
                                                        char  *end = NULL;
                                                        double val = strtod(col_val, &end);

                                                        if (val == 0 && (end == col_val))
                                                                dE("strtod(%s) failed\n", col_val);

                                                        field    = probe_ent_creat1("field", NULL, SEXP_number_newf_r(&se_tmp_mem, val));
                                                        col_type = OVAL_DATATYPE_FLOAT;
                                                        SEXP_free_r(&se_tmp_mem);

                                                }       break;
                                                case ODBX_TYPE_CHAR:
                                                case ODBX_TYPE_NCHAR:
                                                case ODBX_TYPE_VARCHAR:
                                                        field    = probe_ent_creat1("field", NULL, SEXP_string_new_r(&se_tmp_mem, col_val, strlen(col_val)));
                                                        col_type = OVAL_DATATYPE_STRING;
                                                        SEXP_free_r(&se_tmp_mem);
                                                        break;
                                                case ODBX_TYPE_TIMESTAMP:
                                                        break;
                                                }

                                                if (field != NULL) {
                                                        probe_ent_setdatatype(field, col_type);
                                                        probe_ent_attr_add(field, "name", SEXP_string_new_r(&se_tmp_mem, col_name, strlen(col_name)));

                                                        SEXP_list_add(result, field);
                                                        SEXP_free_r(&se_tmp_mem);
                                                        SEXP_free(field);
                                                }
                                        }

                                        SEXP_list_add(item, result);
                                        SEXP_free(result);
				}

				odbx_result_finish(sql_dbr);
			}

			probe_cobj_add_item(probe_out, item);
			SEXP_free(item);
		}

		if (odbx_finish(sql_dbh) != ODBX_ERR_SUCCESS) {
			dE("odbx_finish failed\n");
			goto __exit;
		}
	}

	err = 0;
__exit:
	dbURIInfo_clear(&uriInfo);
	return (err);
}

int probe_main(SEXP_t *probe_in, SEXP_t *probe_out, void *arg, SEXP_t *filters)
{
	char       *engine, *version, *conn, *sqlexp;
	int err;

#define get_string(dst, obj, ent_name)					\
	do {								\
		SEXP_t *__sval;						\
									\
		__sval = probe_obj_getentval (obj, #ent_name, 1);	\
									\
		if (__sval == NULL) {					\
			dE("Missing entity or value: obj=%p, ent=%s\n", obj, #ent_name); \
			err = PROBE_ENOENT;				\
			goto __exit;					\
		}							\
									\
		(dst) = SEXP_string_cstr (__sval);			\
									\
		if ((dst) == NULL) {					\
			SEXP_free(__sval);				\
			err = PROBE_EINVAL;				\
			goto __exit;					\
		}							\
									\
		SEXP_free(__sval);					\
	} while (0)

	if (probe_in == NULL || probe_out == NULL) {
		return (PROBE_EINVAL);
	}

	engine  = NULL;
	version = NULL;
	conn    = NULL;
	sqlexp  = NULL;

	get_string(engine,  probe_in, "engine");
	get_string(version, probe_in, "version");
	get_string(conn,    probe_in, "connection_string");
	get_string(sqlexp,  probe_in, "sql");

	/*
	 * evaluate the SQL statement
	 */
	err = dbSQL_eval(engine, version, conn, sqlexp, probe_out);
__exit:
	if (sqlexp != NULL) {
		__clearmem(sqlexp, strlen(sqlexp));
		oscap_free(sqlexp);
	}

	if (conn != NULL) {
		__clearmem(conn, strlen(conn));
		oscap_free(conn);
	}

	if (version != NULL) {
		__clearmem(version, strlen(version));
		oscap_free(version);
	}

	return (err);
}
