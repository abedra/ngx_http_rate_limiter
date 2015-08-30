#include <stdio.h>
#include <stdlib.h>
#include "libpq-fe.h"

typedef struct {
        char *service_name;
        char *client_id;
        int rate_limit;
        int window_size;
} rate_limit_configuration_t;

static void print_configuration(rate_limit_configuration_t config)
{
        printf("%s (%s): %d, %d\n",
               config.client_id,
               config.service_name,
               config.rate_limit,
               config.window_size);
}

int main(void)
{
        char       query_string[256];
        int        i;
        PGconn     *conn;
        PGresult   *res;
        rate_limit_configuration_t config;

        conn = PQconnectdb("dbname=rate_limiter");

        if (PQstatus(conn) == CONNECTION_BAD) {
                fprintf(stderr, "Connection to database failed.\n");
                fprintf(stderr, "%s", PQerrorMessage(conn));
                exit(1);
        }

        sprintf(query_string, "SELECT * FROM configuration");

        res = PQexec(conn, query_string);

        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                fprintf(stderr, "SELECT query failed.\n");
                PQclear(res);
                PQfinish(conn);
                exit(1);
        }

	rate_limit_configuration_t clients[PQntuples(res)];

	for (i = 0; i < PQntuples(res); i++) {
                config.service_name = PQgetvalue(res, i, 1);
                config.client_id = PQgetvalue(res, i, 2);
                config.rate_limit = strtol(PQgetvalue(res, i, 3), 0, 10);
                config.window_size = strtol(PQgetvalue(res, i, 4), 0, 10);
		clients[i] = config;
        }

	for (i = 0; i < PQntuples(res); i++) {
		print_configuration(clients[i]);
	}

        PQclear(res);
        PQfinish(conn);

        return 0;
}
