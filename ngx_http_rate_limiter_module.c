#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <hiredis/hiredis.h>
#include <postgresql/libpq-fe.h>

#define NGX_HTTP_TOO_MANY_REQUESTS 429


typedef struct {
  char *service_name;
  char *client_id;
  int rate_limit;
  int window_size;
} rate_limit_configuration_t;


typedef struct {
  ngx_str_t  host;
  ngx_uint_t port;
  redisContext *connection;
} redis_t;


typedef struct {
  ngx_uint_t rate_limit;
  ngx_uint_t window_size;
  redis_t redis;
  ngx_str_t database_name;
  int num_clients;
  rate_limit_configuration_t clients[];
} rate_limiter_main_conf_t;


ngx_module_t ngx_http_rate_limiter_module;


static ngx_int_t
load_configuration(ngx_http_request_t *r, rate_limiter_main_conf_t *main_conf)
{
  char       *connect_string;
  char       query_string[256];
  int        i;
  PGconn     *conn;
  PGresult   *res;
  rate_limit_configuration_t config;

  int result = asprintf(&connect_string, "dbname=%s", main_conf->database_name.data);
  if (result == -1) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Could not allocate connection string");
    return NGX_DECLINED;
  }

  conn = PQconnectdb(connect_string);

  if (PQstatus(conn) == CONNECTION_BAD) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Could not connect to database: %s", PQerrorMessage(conn));
    return NGX_DECLINED;
  }

  sprintf(query_string, "SELECT * FROM configuration");

  res = PQexec(conn, query_string);

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Query failed");
    PQclear(res);
    PQfinish(conn);
    return NGX_DECLINED;
  }

  main_conf->num_clients = PQntuples(res);

  for (i = 0; i < PQntuples(res); i++) {
    config.service_name = PQgetvalue(res, i, 1);
    config.client_id = PQgetvalue(res, i, 2);
    config.rate_limit = strtol(PQgetvalue(res, i, 3), 0, 10);
    config.window_size = strtol(PQgetvalue(res, i, 4), 0, 10);
    main_conf->clients[i] = config;
  }

  PQclear(res);
  PQfinish(conn);

  return NGX_OK;
}


static void
set_rate_limit_remaining_header(ngx_http_request_t *r, int remaining)
{
  ngx_table_elt_t *h;
  h = ngx_list_push(&r->headers_out.headers);
  h->hash = 1;
  ngx_str_set(&h->key, "X-Rate-Limit-Remaining");
  char *str;
  int len = asprintf(&str, "%d", remaining);
  h->value.len = len;
  h->value.data = (u_char *) str;
}


static void
set_rate_limit_limit_header(ngx_http_request_t *r, int limit)
{
  ngx_table_elt_t *h;
  h = ngx_list_push(&r->headers_out.headers);
  h->hash = 1;
  ngx_str_set(&h->key, "X-Rate-Limit-Limit");
  char *str;
  int len = asprintf(&str, "%d", limit);
  h->value.len = len;
  h->value.data = (u_char *) str;
}


static void
set_rate_limit_reset_header(ngx_http_request_t *r, int remaining)
{
  ngx_table_elt_t *h;
  h = ngx_list_push(&r->headers_out.headers);
  h->hash = 1;
  ngx_str_set(&h->key, "X-Rate-Limit-Reset");
  char *str;
  int len = asprintf(&str, "%d", remaining);
  h->value.len = len;
  h->value.data = (u_char *) str;
}


static ngx_int_t
request_count(ngx_http_request_t *r, rate_limiter_main_conf_t *main_conf)
{
  redisReply *reply = redisCommand(main_conf->redis.connection, "GET %s", r->connection->addr_text.data);
  if (reply && reply->type != REDIS_REPLY_NIL) {
    int count = strtol(reply->str, 0, 10);
    freeReplyObject(reply);
    return count;
  }
  return 0;
}


static int
window_size_in_seconds(ngx_uint_t window_size)
{
  return window_size * 60;
}


static ngx_int_t
increment(ngx_http_request_t *r, rate_limiter_main_conf_t *main_conf)
{
  redisCommand(main_conf->redis.connection, "INCR %s", r->connection->addr_text.data);
  redisReply *reply = redisCommand(main_conf->redis.connection, "TTL %s", r->connection->addr_text.data);
  if (reply && reply->type == REDIS_REPLY_INTEGER) {
    if (reply->integer == -1) {
      int remaining = window_size_in_seconds((int)main_conf->window_size);
      redisCommand(main_conf->redis.connection,
                   "EXPIRE %s %d",
                   r->connection->addr_text.data,
                   remaining);
      freeReplyObject(reply);
      return remaining;
    } else if (reply->integer > 0) {
      int remaining = reply->integer;
      freeReplyObject(reply);
      return remaining;
    } else {
      freeReplyObject(reply);
      return NGX_DECLINED;
    }
  }

  return NGX_DECLINED;
}


static int
time_to_reset(ngx_http_request_t *r, rate_limiter_main_conf_t *main_conf)
{
  redisReply *reply = redisCommand(main_conf->redis.connection, "TTL %s", r->connection->addr_text.data);
  if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) {
    int remaining = reply->integer;
    freeReplyObject(reply);
    return remaining;
  }
  return NGX_DECLINED;
}


static ngx_int_t
ngx_http_rate_limiter_handler(ngx_http_request_t *r)
{
  if (r->main->internal) {
    return NGX_DECLINED;
  }

  rate_limiter_main_conf_t *main_conf = ngx_http_get_module_main_conf(r, ngx_http_rate_limiter_module);
  main_conf->redis.connection = redisConnect((const char *)main_conf->redis.host.data, main_conf->redis.port);

  // TODO: only do this once
  load_configuration(r, main_conf);
  int i;
  for (i = 0; i < main_conf->num_clients; i++) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "%s (%s): %d, %d", main_conf->clients[i].client_id, main_conf->clients[i].service_name, main_conf->clients[i].rate_limit, main_conf->clients[i].window_size);
  }

  ngx_int_t current = request_count(r, main_conf);
  ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Count: %d", current);

  if (current > (ngx_int_t)main_conf->rate_limit) {
    int remaining = time_to_reset(r, main_conf);
    set_rate_limit_reset_header(r, remaining);
    return NGX_HTTP_TOO_MANY_REQUESTS;
  } else {
    int remaining = increment(r, main_conf);
    set_rate_limit_remaining_header(r, main_conf->rate_limit - current);
    set_rate_limit_limit_header(r, main_conf->rate_limit);
    set_rate_limit_reset_header(r, remaining);
  }

  return NGX_OK;
}


static ngx_int_t
ngx_http_rate_limiter_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt *h;
  ngx_http_core_main_conf_t *cmcf;

  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
  h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);

  if (h == NULL) {
    return NGX_ERROR;
  }

  *h = ngx_http_rate_limiter_handler;

  return NGX_OK;
}


static ngx_command_t ngx_http_rate_limiter_commands[] = {
  {
    ngx_string("rate_limiter_rate_limit"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(rate_limiter_main_conf_t, rate_limit),
    NULL
  },
  {
    ngx_string("rate_limiter_window_size"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(rate_limiter_main_conf_t, window_size),
    NULL
  },
  {
    ngx_string("rate_limiter_redis_host"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(rate_limiter_main_conf_t, redis.host),
    NULL
  },
  {
    ngx_string("rate_limiter_redis_port"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(rate_limiter_main_conf_t, redis.port),
    NULL
  },
  {
    ngx_string("rate_limiter_database_name"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(rate_limiter_main_conf_t, database_name),
    NULL
  },


  ngx_null_command
};


static void*
ngx_http_rate_limiter_create_main_conf(ngx_conf_t *cf)
{
  rate_limiter_main_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(rate_limiter_main_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  conf->rate_limit = NGX_CONF_UNSET_UINT;
  conf->window_size = NGX_CONF_UNSET_UINT;
  conf->redis.port = NGX_CONF_UNSET_UINT;
  conf->redis.connection = NULL;

  return conf;
}


static ngx_http_module_t ngx_http_rate_limiter_module_ctx = {
  NULL,                                   /* preconfiguration */
  ngx_http_rate_limiter_init,             /* postconfiguration */
  ngx_http_rate_limiter_create_main_conf, /* create main configuration */
  NULL,                                   /* init main configuration */
  NULL,                                   /* create server configuration */
  NULL,                                   /* merge server configuration */
  NULL,                                   /* create location configuration */
  NULL                                    /* merge location configuration */
};


ngx_module_t ngx_http_rate_limiter_module = {
  NGX_MODULE_V1,
  &ngx_http_rate_limiter_module_ctx, /* module context */
  ngx_http_rate_limiter_commands,    /* module directives */
  NGX_HTTP_MODULE,                   /* module type */
  NULL,                              /* init master */
  NULL,                              /* init module */
  NULL,                              /* init process */
  NULL,                              /* init thread */
  NULL,                              /* exit thread */
  NULL,                              /* exit process */
  NULL,                              /* exit master */
  NGX_MODULE_V1_PADDING
};
