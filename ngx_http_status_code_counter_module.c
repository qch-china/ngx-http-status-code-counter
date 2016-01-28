
/*
 * Copyright (C) Kennon Ballou
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NGX_HTTP_LAST_LEVEL_500  508
#define NGX_HTTP_NUM_STATUS_CODES (NGX_HTTP_LAST_LEVEL_500 - NGX_HTTP_OK)

//共存内存的信息
ngx_shm_t code_counter_shm;

//指向统计信息的指针（内容存在共存内存中）
ngx_atomic_t *code_counter_ptr[NGX_HTTP_NUM_STATUS_CODES];

static char *ngx_http_set_status_code_counter(ngx_conf_t *cf, ngx_command_t *cmd,
                                 void *conf);

static ngx_int_t ngx_http_status_code_counter_init(ngx_conf_t *cf);

static void ngx_http_status_code_counter_clean(ngx_cycle_t *cycle);

static ngx_command_t  ngx_http_status_code_counter_commands[] = {

    { ngx_string("show_status_code_count"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_http_set_status_code_counter,
      0,
      0,
      NULL },

      ngx_null_command
};



static ngx_http_module_t  ngx_http_status_code_counter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_status_code_counter_init,     /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_status_code_counter_module = {
    NGX_MODULE_V1,
    &ngx_http_status_code_counter_module_ctx,      /* module context */
    ngx_http_status_code_counter_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    ngx_http_status_code_counter_clean,    /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t ngx_http_status_code_counter_handler(ngx_http_request_t *r)
{
    size_t             size;
    ngx_int_t          rc, i, j;
    ngx_buf_t         *b;
    ngx_chain_t        out;
	ngx_atomic_t code_counts[NGX_HTTP_NUM_STATUS_CODES];

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

	for(i=0; i<NGX_HTTP_NUM_STATUS_CODES; i++)
	{
		code_counts[i] = *code_counter_ptr[i];
	}

    ngx_str_set(&r->headers_out.content_type, "text/plain");

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    /* count number of seen status codes to determine how much buffer we need */
    j = 0;
    for(i = 0; i < NGX_HTTP_NUM_STATUS_CODES; i++ )
    {
      if(code_counts[i] > 0)
      {
        j++;
      }
    }

    size = sizeof("HTTP status code counts:\n")
         + j * (sizeof("XXX \n") + NGX_ATOMIC_T_LEN);

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;

	b->last = ngx_sprintf(b->last, "Pid: %d\nHTTP status code counts:\n", getpid());

    for(i = 0; i < NGX_HTTP_NUM_STATUS_CODES; i++ )
    {
      if(code_counts[i] > 0)
      {
        b->last = ngx_sprintf(b->last, "%uA %uA\n", i+NGX_HTTP_OK, code_counts[i]);
      }
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static char *ngx_http_set_status_code_counter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_status_code_counter_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_status_code_count_handler(ngx_http_request_t *r)
{
	if( r->headers_out.status >= NGX_HTTP_OK && r->headers_out.status < NGX_HTTP_LAST_LEVEL_500)
	{
		//ngx_http_status_code_counts[r->headers_out.status - NGX_HTTP_OK]++;
		ngx_atomic_fetch_add(code_counter_ptr[r->headers_out.status - NGX_HTTP_OK], 1);
	}

  return NGX_OK;
}


static ngx_int_t
ngx_http_status_code_counter_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;
    ngx_int_t i;

	u_char *shared;
	size_t size = sizeof(ngx_atomic_t) * NGX_HTTP_NUM_STATUS_CODES;

	code_counter_shm.size = size;
	code_counter_shm.name.data = (u_char*)"ngx_code_counts";
	code_counter_shm.name.len = sizeof("ngx_code_counts");
	code_counter_shm.log = cf->log;
	if( ngx_shm_alloc(&code_counter_shm) != NGX_OK )
		return NGX_ERROR;
	shared = code_counter_shm.addr;

	//初始化统计数组
	for(i = 0; i < NGX_HTTP_NUM_STATUS_CODES; i++) 
	{
		code_counter_ptr[i] = (ngx_atomic_t *)(shared + sizeof(ngx_atomic_t)*i);
	}

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_status_code_count_handler;

    return NGX_OK;
}


static void ngx_http_status_code_counter_clean(ngx_cycle_t *cycle)
{
	ngx_shm_free(&code_counter_shm);
}