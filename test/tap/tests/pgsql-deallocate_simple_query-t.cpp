/**
 * @file pgsql-deallocate_simple_query-t.cpp
 * @brief A DEALLOCATE of a SQL-level prepared statement, sent over the Simple
 *        Query protocol, must reach the backend that holds the statement.
 *
 * @details
 *   A PREPARE sent as text (the Simple Query protocol) creates its statement
 *   on the backend connection only: ProxySQL's client-side statement map
 *   tracks just the extended query protocol's Parse, so the DEALLOCATE that
 *   follows names a statement the proxy has never seen. The DEALLOCATE
 *   handler used to answer such a name itself, with 'prepared statement
 *   "..." does not exist', and a plain PREPARE + EXECUTE + DEALLOCATE round
 *   trip over the Simple Query protocol could not complete.
 *
 *   The statement lives on the connection the PREPARE ran on -- a SQL-level
 *   PREPARE disables multiplexing there, keeping that connection attached to
 *   the session -- so the DEALLOCATE can be forwarded to it. The test drives
 *   one frontend connection with PQexec (the Simple Query protocol) and
 *   proves the whole round trip with PostgreSQL's own answers:
 *     1. PREPARE deallocate_test_stmt AS SELECT 1 succeeds;
 *     2. EXECUTE deallocate_test_stmt returns 1;
 *     3. DEALLOCATE deallocate_test_stmt succeeds (the regression: this is
 *        the statement that used to fail);
 *     4. EXECUTE deallocate_test_stmt now fails with SQLSTATE 26000: only
 *        PostgreSQL can report the statement as gone, so the DEALLOCATE
 *        really ran on the backend. A DEALLOCATE answered locally would
 *        have left the statement working.
 *
 *   INFRA: legacy-g4 (any group with a PostgreSQL backend behind ProxySQL
 *   works).
 */

#include "libpq-fe.h"

#include "command_line.h"
#include "tap.h"
#include "utils.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

using PGConnPtr = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using std::string;

static const char* SQLSTATE_INVALID_SQL_STATEMENT_NAME = "26000";

static PGConnPtr createProxyConn(CommandLine& cl) {
	std::stringstream ss;
	ss << "host=" << cl.pgsql_host << " port=" << cl.pgsql_port
	   << " user=" << cl.pgsql_username << " password=" << cl.pgsql_password
	   << " sslmode=disable";
	PGConnPtr conn(PQconnectdb(ss.str().c_str()), &PQfinish);
	if (PQstatus(conn.get()) != CONNECTION_OK) {
		diag("Connection to ProxySQL failed: %s", PQerrorMessage(conn.get()));
		return PGConnPtr(nullptr, &PQfinish);
	}
	return conn;
}

static string sqlstate_of(const PGresult* res) {
	const char* s = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	return s ? s : "";
}

// A statement the backend must accept: reported with PGRES_COMMAND_OK.
static void expect_command_ok(PGconn* conn, const string& query) {
	PGresult* res = PQexec(conn, query.c_str());
	const ExecStatusType st = PQresultStatus(res);
	ok(st == PGRES_COMMAND_OK, "'%s' succeeds: status=%s msg='%s'",
		query.c_str(), PQresStatus(st), PQresultErrorMessage(res));
	PQclear(res);
}

// "EXECUTE <name>" must return the one row the statement was prepared with.
static void expect_execute_returns_one(PGconn* conn, const string& query) {
	PGresult* res = PQexec(conn, query.c_str());
	const ExecStatusType st = PQresultStatus(res);
	const bool got_row = (st == PGRES_TUPLES_OK && PQntuples(res) == 1 && PQnfields(res) == 1);
	const char* got = got_row ? PQgetvalue(res, 0, 0) : "";
	ok(got_row && strcmp(got, "1") == 0,
		"'%s' returns 1: status=%s value='%s' msg='%s'",
		query.c_str(), PQresStatus(st), got, PQresultErrorMessage(res));
	PQclear(res);
}

// "EXECUTE <name>" must fail with SQLSTATE 26000. Only PostgreSQL reports the
// statement as gone, so this proves the DEALLOCATE ran on the backend: had it
// been answered locally, the statement would still be there and execute.
static void expect_execute_fails_missing_stmt(PGconn* conn, const string& query) {
	PGresult* res = PQexec(conn, query.c_str());
	const ExecStatusType st = PQresultStatus(res);
	const string got = sqlstate_of(res);
	ok(st == PGRES_FATAL_ERROR && got == SQLSTATE_INVALID_SQL_STATEMENT_NAME,
		"'%s' is refused with SQLSTATE %s: status=%s sqlstate='%s' msg='%s'",
		query.c_str(), SQLSTATE_INVALID_SQL_STATEMENT_NAME, PQresStatus(st),
		got.c_str(), PQresultErrorMessage(res));
	PQclear(res);
}

int main(int, char**) {
	CommandLine cl;
	plan(6);

	if (cl.getEnv()) {
		diag("Failed to get the required environmental variables.");
		return exit_status();
	}

	// A per-run name keeps a statement left on a pooled backend connection by
	// an earlier run out of the picture.
	const string stmt_name = string("deallocate_test_stmt_") + std::to_string(getpid());
	const string prepare_sql = string("PREPARE ") + stmt_name + " AS SELECT 1";
	const string execute_sql = string("EXECUTE ") + stmt_name;
	const string deallocate_sql = string("DEALLOCATE ") + stmt_name;

	PGConnPtr conn = createProxyConn(cl);
	ok(conn != nullptr, "connected to ProxySQL");
	if (!conn) {
		return exit_status();
	}
	PGconn* c = conn.get();

	// 1. The PREPARE itself: forwarded to the backend, which owns the
	//    statement from here on.
	expect_command_ok(c, prepare_sql);

	// 2. The statement exists where it was created.
	expect_execute_returns_one(c, execute_sql);

	// 3. The DEALLOCATE over the Simple Query protocol: the regression. It
	//    names a statement ProxySQL never tracked, so it must be forwarded
	//    to the backend connection the PREPARE pinned, not answered with a
	//    local 'does not exist' error.
	expect_command_ok(c, deallocate_sql);

	// 4. The statement is really gone from the backend.
	expect_execute_fails_missing_stmt(c, execute_sql);

	// The session survives the whole round trip unharmed.
	PGresult* res = PQexec(c, "SELECT 1");
	ok(PQresultStatus(res) == PGRES_TUPLES_OK && PQtransactionStatus(c) == PQTRANS_IDLE,
		"session still usable and idle: %s", PQerrorMessage(c));
	PQclear(res);

	return exit_status();
}
