
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) NGINX, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <nxt_auto_config.h>
#include <nxt_types.h>
#include <nxt_clang.h>
#include <nxt_string.h>
#include <nxt_stub.h>
#include <nxt_array.h>

#include <njscript.h>


typedef struct {
    njs_vm_t            *vm;
    ngx_str_t            content;
    const njs_extern_t  *req_proto;
    const njs_extern_t  *res_proto;
} ngx_http_js_loc_conf_t;


typedef struct {
    njs_vm_t            *vm;
    ngx_log_t           *log;
    njs_opaque_value_t   args[2];
} ngx_http_js_ctx_t;


typedef struct {
    ngx_list_part_t     *part;
    ngx_uint_t           item;
} ngx_http_js_table_entry_t;


typedef struct {
    ngx_http_request_t  *request;
    njs_vm_event_t       vm_event;
    void                *unused;
    ngx_int_t            ident;
} ngx_http_js_event_t;


static ngx_int_t ngx_http_js_content_handler(ngx_http_request_t *r);
static void ngx_http_js_content_event_handler(ngx_http_request_t *r);
static void ngx_http_js_content_write_event_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_js_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_js_init_vm(ngx_http_request_t *r);
static void ngx_http_js_cleanup_ctx(void *data);
static void ngx_http_js_cleanup_vm(void *data);

static njs_ret_t ngx_http_js_ext_get_string(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_set_string(njs_vm_t *vm, void *obj,
    uintptr_t data, nxt_str_t *value);
static njs_ret_t ngx_http_js_ext_foreach_header(njs_vm_t *vm, void *obj,
    void *next, uintptr_t data);
static njs_ret_t ngx_http_js_ext_next_header(njs_vm_t *vm, njs_value_t *value,
    void *obj, void *next);
static ngx_table_elt_t *ngx_http_js_get_header(ngx_list_part_t *part,
    u_char *data, size_t len);
static njs_ret_t ngx_http_js_ext_get_header_out(njs_vm_t *vm,
    njs_value_t *value, void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_set_header_out(njs_vm_t *vm, void *obj,
    uintptr_t data, nxt_str_t *value);
static njs_ret_t ngx_http_js_ext_foreach_header_out(njs_vm_t *vm, void *obj,
    void *next); /*FIXME*/
static njs_ret_t ngx_http_js_ext_get_status(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_set_status(njs_vm_t *vm, void *obj,
    uintptr_t data, nxt_str_t *value);
static njs_ret_t ngx_http_js_ext_get_content_length(njs_vm_t *vm,
    njs_value_t *value, void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_set_content_length(njs_vm_t *vm, void *obj,
    uintptr_t data, nxt_str_t *value);
static njs_ret_t ngx_http_js_ext_send_header(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t ngx_http_js_ext_send(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t ngx_http_js_ext_finish(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t ngx_http_js_ext_log(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t ngx_http_js_ext_get_http_version(njs_vm_t *vm,
    njs_value_t *value, void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_get_remote_address(njs_vm_t *vm,
    njs_value_t *value, void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_get_header_in(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_foreach_header_in(njs_vm_t *vm, void *obj,
    void *next); /*FIXME*/
static njs_ret_t ngx_http_js_ext_get_arg(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_foreach_arg(njs_vm_t *vm, void *obj,
    void *next);
static njs_ret_t ngx_http_js_ext_next_arg(njs_vm_t *vm, njs_value_t *value,
    void *obj, void *next);
static njs_ret_t ngx_http_js_ext_get_variable(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);
static njs_ret_t ngx_http_js_ext_get_response(njs_vm_t *vm, njs_value_t *value,
    void *obj, uintptr_t data);

static njs_host_event_t ngx_http_js_set_timer(njs_external_ptr_t external,
    uint64_t delay, njs_vm_event_t vm_event);
static void ngx_http_js_clear_timer(njs_external_ptr_t external,
    njs_host_event_t event);
static void ngx_http_js_timer_handler(ngx_event_t *ev);
static void ngx_http_js_handle_event(ngx_http_request_t *r,
    njs_vm_event_t vm_event);

static char *ngx_http_js_include(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_js_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_js_content(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_js_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_js_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);


static ngx_command_t  ngx_http_js_commands[] = {

    { ngx_string("js_include"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_js_include,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("js_set"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_js_set,
      0,
      0,
      NULL },

    { ngx_string("js_content"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF|NGX_CONF_TAKE1,
      ngx_http_js_content,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};


static ngx_http_module_t  ngx_http_js_module_ctx = {
    NULL,                          /* preconfiguration */
    NULL,                          /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_js_create_loc_conf,   /* create location configuration */
    ngx_http_js_merge_loc_conf     /* merge location configuration */
};


ngx_module_t  ngx_http_js_module = {
    NGX_MODULE_V1,
    &ngx_http_js_module_ctx,       /* module context */
    ngx_http_js_commands,          /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};


static njs_external_t  ngx_http_js_ext_response[] = {

    { nxt_string("headers"),
      NJS_EXTERN_OBJECT,
      NULL,
      0,
      ngx_http_js_ext_get_header_out,
      ngx_http_js_ext_set_header_out,
      NULL,
      ngx_http_js_ext_foreach_header_out,
      ngx_http_js_ext_next_header,
      NULL,
      0 },

    { nxt_string("status"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_status,
      ngx_http_js_ext_set_status,
      NULL,
      NULL,
      NULL,
      NULL,
      offsetof(ngx_http_request_t, headers_out.status) },

    { nxt_string("contentType"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_string,
      ngx_http_js_ext_set_string,
      NULL,
      NULL,
      NULL,
      NULL,
      offsetof(ngx_http_request_t, headers_out.content_type) },

    { nxt_string("contentLength"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_content_length,
      ngx_http_js_ext_set_content_length,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },

    { nxt_string("sendHeader"),
      NJS_EXTERN_METHOD,
      NULL,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      ngx_http_js_ext_send_header,
      0 },

    { nxt_string("send"),
      NJS_EXTERN_METHOD,
      NULL,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      ngx_http_js_ext_send,
      0 },

    { nxt_string("finish"),
      NJS_EXTERN_METHOD,
      NULL,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      ngx_http_js_ext_finish,
      0 },
};


static njs_external_t  ngx_http_js_ext_request[] = {

    { nxt_string("log"),
      NJS_EXTERN_METHOD,
      NULL,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      ngx_http_js_ext_log,
      0 },

    { nxt_string("uri"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_string,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      offsetof(ngx_http_request_t, uri) },

    { nxt_string("method"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_string,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      offsetof(ngx_http_request_t, method_name) },

    { nxt_string("httpVersion"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_http_version,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },

    { nxt_string("remoteAddress"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_remote_address,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },

    { nxt_string("headers"),
      NJS_EXTERN_OBJECT,
      NULL,
      0,
      ngx_http_js_ext_get_header_in,
      NULL,
      NULL,
      ngx_http_js_ext_foreach_header_in,
      ngx_http_js_ext_next_header,
      NULL,
      0 },

    { nxt_string("args"),
      NJS_EXTERN_OBJECT,
      NULL,
      0,
      ngx_http_js_ext_get_arg,
      NULL,
      NULL,
      ngx_http_js_ext_foreach_arg,
      ngx_http_js_ext_next_arg,
      NULL,
      0 },

    { nxt_string("variables"),
      NJS_EXTERN_OBJECT,
      NULL,
      0,
      ngx_http_js_ext_get_variable,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },

    { nxt_string("response"),
      NJS_EXTERN_PROPERTY,
      NULL,
      0,
      ngx_http_js_ext_get_response,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },
};


static njs_external_t  ngx_http_js_externals[] = {

    { nxt_string("request"),
      NJS_EXTERN_OBJECT,
      ngx_http_js_ext_request,
      nxt_nitems(ngx_http_js_ext_request),
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },

    { nxt_string("response"),
      NJS_EXTERN_OBJECT,
      ngx_http_js_ext_response,
      nxt_nitems(ngx_http_js_ext_response),
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 },
};


static njs_vm_ops_t ngx_http_js_ops = {
    ngx_http_js_set_timer,
    ngx_http_js_clear_timer
};


static ngx_int_t
ngx_http_js_content_handler(ngx_http_request_t *r)
{
    ngx_int_t  rc;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http js content handler");

    rc = ngx_http_read_client_request_body(r,
                                           ngx_http_js_content_event_handler);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static void
ngx_http_js_content_event_handler(ngx_http_request_t *r)
{
    ngx_int_t                rc;
    nxt_str_t                name, exception;
    njs_function_t          *func;
    ngx_http_js_ctx_t       *ctx;
    ngx_http_js_loc_conf_t  *jlcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http js content event handler");

    rc = ngx_http_js_init_vm(r);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    jlcf = ngx_http_get_module_loc_conf(r, ngx_http_js_module);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http js content call \"%V\"" , &jlcf->content);

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    name.start = jlcf->content.data;
    name.length = jlcf->content.len;

    func = njs_vm_function(ctx->vm, &name);
    if (func == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "js function \"%V\" not found", &jlcf->content);
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (njs_vm_call(ctx->vm, func, ctx->args, 2) != NJS_OK) {
        njs_vm_retval_to_ext_string(ctx->vm, &exception);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "js exception: %*s", exception.length, exception.start);

        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (njs_vm_pending(ctx->vm)) {
        r->write_event_handler = ngx_http_js_content_write_event_handler;
        return;
    }

    ngx_http_finalize_request(r, NGX_OK);
}


static void
ngx_http_js_content_write_event_handler(ngx_http_request_t *r)
{
    ngx_event_t               *wev;
    ngx_connection_t          *c;
    ngx_http_js_ctx_t         *ctx;
    ngx_http_core_loc_conf_t  *clcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http js content write event handler");

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    if (!njs_vm_pending(ctx->vm)) {
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    c = r->connection;
    wev = c->write;

    if (wev->timedout) {
        ngx_connection_error(c, NGX_ETIMEDOUT, "client timed out");
        ngx_http_finalize_request(r, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    if (ngx_http_output_filter(r, NULL) == NGX_ERROR) {
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    clcf = ngx_http_get_module_loc_conf(r->main, ngx_http_core_module);

    if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    if (!wev->delayed) {
        if (wev->active && !wev->ready) {
            ngx_add_timer(wev, clcf->send_timeout);

        } else if (wev->timer_set) {
            ngx_del_timer(wev);
        }
    }
}


static ngx_int_t
ngx_http_js_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_str_t *fname = (ngx_str_t *) data;

    ngx_int_t           rc;
    nxt_int_t           pending;
    nxt_str_t           name, value, exception;
    njs_function_t     *func;
    ngx_http_js_ctx_t  *ctx;

    rc = ngx_http_js_init_vm(r);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED) {
        v->not_found = 1;
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http js variable call \"%V\"", fname);

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    name.start = fname->data;
    name.length = fname->len;

    func = njs_vm_function(ctx->vm, &name);
    if (func == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "js function \"%V\" not found", fname);
        v->not_found = 1;
        return NGX_OK;
    }

    pending = njs_vm_pending(ctx->vm);

    if (njs_vm_call(ctx->vm, func, ctx->args, 2) != NJS_OK) {
        njs_vm_retval_to_ext_string(ctx->vm, &exception);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "js exception: %*s", exception.length, exception.start);

        v->not_found = 1;
        return NGX_OK;
    }

    if (njs_vm_retval_to_ext_string(ctx->vm, &value) != NJS_OK) {
        return NGX_ERROR;
    }

    if (!pending && njs_vm_pending(ctx->vm)) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "async operation inside \"%V\" variable handler", fname);
        return NGX_ERROR;
    }

    v->len = value.length;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = value.start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_js_init_vm(ngx_http_request_t *r)
{
    nxt_int_t                rc;
    nxt_str_t                exception;
    ngx_http_js_ctx_t       *ctx;
    ngx_pool_cleanup_t      *cln;
    ngx_http_js_loc_conf_t  *jlcf;

    jlcf = ngx_http_get_module_loc_conf(r, ngx_http_js_module);
    if (jlcf->vm == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_js_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_js_module);
    }

    if (ctx->vm) {
        return NGX_OK;
    }

    ctx->vm = njs_vm_clone(jlcf->vm, r);
    if (ctx->vm == NULL) {
        return NGX_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    ctx->log = r->connection->log;

    cln->handler = ngx_http_js_cleanup_ctx;
    cln->data = ctx;

    if (njs_vm_run(ctx->vm) == NJS_ERROR) {
        njs_vm_retval_to_ext_string(ctx->vm, &exception);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "js exception: %*s", exception.length, exception.start);

        return NGX_ERROR;
    }

    rc = njs_vm_external_create(ctx->vm, &ctx->args[0], jlcf->req_proto, r);
    if (rc != NXT_OK) {
        return NGX_ERROR;
    }

    rc = njs_vm_external_create(ctx->vm, &ctx->args[1], jlcf->res_proto, r);
    if (rc != NXT_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_js_cleanup_ctx(void *data)
{
    ngx_http_js_ctx_t *ctx = data;

    if (njs_vm_pending(ctx->vm)) {
        ngx_log_error(NGX_LOG_ERR, ctx->log, 0, "pending events");
    }

    njs_vm_destroy(ctx->vm);
}


static void
ngx_http_js_cleanup_vm(void *data)
{
    njs_vm_t *vm = data;

    njs_vm_destroy(vm);
}


static njs_ret_t
ngx_http_js_ext_get_string(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    char *p = obj;

    ngx_str_t  *field;

    field = (ngx_str_t *) (p + data);

    return njs_string_create(vm, value, field->data, field->len, 0);
}


static njs_ret_t
ngx_http_js_ext_set_string(njs_vm_t *vm, void *obj, uintptr_t data,
    nxt_str_t *value)
{
    char *p = obj;

    ngx_str_t           *field;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    field = (ngx_str_t *) (p + data);
    field->len = value->length;

    field->data = ngx_pnalloc(r->pool, value->length);
    if (field->data == NULL) {
        return NJS_ERROR;
    }

    ngx_memcpy(field->data, value->start, value->length);

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_foreach_header(njs_vm_t *vm, void *obj, void *next,
    uintptr_t data)
{
    char *p = obj;

    ngx_list_t                 *headers;
    ngx_http_request_t         *r;
    ngx_http_js_table_entry_t  *entry, **e;

    r = (ngx_http_request_t *) obj;

    entry = ngx_palloc(r->pool, sizeof(ngx_http_js_table_entry_t));
    if (entry == NULL) {
        return NJS_ERROR;
    }

    headers = (ngx_list_t *) (p + data);

    entry->part = &headers->part;
    entry->item = 0;

    e = (ngx_http_js_table_entry_t **) next;
    *e = entry;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_next_header(njs_vm_t *vm, njs_value_t *value, void *obj,
    void *next)
{
    ngx_http_js_table_entry_t **e = next;

    ngx_table_elt_t            *header, *h;
    ngx_http_js_table_entry_t  *entry;

    entry = *e;

    while (entry->part) {

        if (entry->item >= entry->part->nelts) {
            entry->part = entry->part->next;
            entry->item = 0;
            continue;
        }

        header = entry->part->elts;
        h = &header[entry->item++];

        return njs_string_create(vm, value, h->key.data, h->key.len, 0);
    }

    return NJS_DONE;
}


static ngx_table_elt_t *
ngx_http_js_get_header(ngx_list_part_t *part, u_char *data, size_t len)
{
    ngx_uint_t        i;
    ngx_table_elt_t  *header, *h;

    header = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        h = &header[i];

        if (h->hash == 0) {
            continue;
        }

        if (h->key.len == len && ngx_strncasecmp(h->key.data, data, len) == 0) {
            return h;
        }
    }

    return NULL;
}


static njs_ret_t
ngx_http_js_ext_get_header_out(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    nxt_str_t           *v;
    ngx_table_elt_t     *h;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;
    v = (nxt_str_t *) data;

    h = ngx_http_js_get_header(&r->headers_out.headers.part, v->start,
                               v->length);
    if (h == NULL) {
        return njs_string_create(vm, value, NULL, 0, 0);
    }

    return njs_string_create(vm, value, h->value.data, h->value.len, 0);
}


static njs_ret_t
ngx_http_js_ext_set_header_out(njs_vm_t *vm, void *obj, uintptr_t data,
    nxt_str_t *value)
{
    u_char              *p;
    nxt_str_t           *v;
    ngx_table_elt_t     *h;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;
    v = (nxt_str_t *) data;

    h = ngx_http_js_get_header(&r->headers_out.headers.part, v->start,
                               v->length);

    if (h == NULL || h->hash == 0) {
        h = ngx_list_push(&r->headers_out.headers);
        if (h == NULL) {
            return NJS_ERROR;
        }

        p = ngx_pnalloc(r->pool, v->length);
        if (p == NULL) {
            return NJS_ERROR;
        }

        ngx_memcpy(p, v->start, v->length);

        h->key.data = p;
        h->key.len = v->length;
        h->hash = 1;
    }


    p = ngx_pnalloc(r->pool, value->length);
    if (p == NULL) {
        return NJS_ERROR;
    }

    ngx_memcpy(p, value->start, value->length);

    h->value.data = p;
    h->value.len = value->length;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_foreach_header_out(njs_vm_t *vm, void *obj, void *next)
{
    return ngx_http_js_ext_foreach_header(vm, obj, next,
                             offsetof(ngx_http_request_t, headers_out.headers));
}


static njs_ret_t
ngx_http_js_ext_get_status(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    size_t               len;
    u_char              *p;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    p = ngx_pnalloc(r->pool, 3);
    if (p == NULL) {
        return NJS_ERROR;
    }

    len = ngx_snprintf(p, 3, "%ui", r->headers_out.status) - p;

    return njs_string_create(vm, value, p, len, 0);
}


static njs_ret_t
ngx_http_js_ext_set_status(njs_vm_t *vm, void *obj, uintptr_t data,
    nxt_str_t *value)
{
    ngx_int_t            n;
    ngx_http_request_t  *r;

    n = ngx_atoi(value->start, value->length);
    if (n == NGX_ERROR) {
        return NJS_ERROR;
    }

    r = (ngx_http_request_t *) obj;

    r->headers_out.status = n;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_get_content_length(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    size_t               len;
    u_char              *p;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    p = ngx_pnalloc(r->pool, NGX_OFF_T_LEN);
    if (p == NULL) {
        return NJS_ERROR;
    }

    len = ngx_sprintf(p, "%O", r->headers_out.content_length_n) - p;

    return njs_string_create(vm, value, p, len, 0);
}


static njs_ret_t
ngx_http_js_ext_set_content_length(njs_vm_t *vm, void *obj, uintptr_t data,
    nxt_str_t *value)
{
    ngx_int_t            n;
    ngx_http_request_t  *r;

    n = ngx_atoi(value->start, value->length);
    if (n == NGX_ERROR) {
        return NJS_ERROR;
    }

    r = (ngx_http_request_t *) obj;

    r->headers_out.content_length_n = n;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_send_header(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    ngx_http_request_t  *r;

    r = njs_value_data(njs_argument(args, 0));

    if (ngx_http_send_header(r) == NGX_ERROR) {
        return NJS_ERROR;
    }

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_send(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_int_t            ret;
    nxt_str_t            s;
    ngx_buf_t           *b;
    uintptr_t            next;
    ngx_uint_t           n;
    ngx_chain_t         *out, *cl, **ll;
    ngx_http_request_t  *r;

    r = njs_value_data(njs_argument(args, 0));

    out = NULL;
    ll = &out;

    for (n = 1; n < nargs; n++) {
        next = 0;

        for ( ;; ) {
            ret = njs_value_string_copy(vm, &s, njs_argument(args, n), &next);

            if (ret == NJS_DECLINED) {
                break;
            }

            if (ret == NJS_ERROR) {
                return NJS_ERROR;
            }

            if (s.length == 0) {
                continue;
            }

            /* TODO: njs_value_release(vm, value) in buf completion */

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "http js send: \"%*s\"", s.length, s.start);

            b = ngx_calloc_buf(r->pool);
            if (b == NULL) {
                return NJS_ERROR;
            }

            b->start = s.start;
            b->pos = b->start;
            b->end = s.start + s.length;
            b->last = b->end;
            b->memory = 1;

            cl = ngx_alloc_chain_link(r->pool);
            if (cl == NULL) {
                return NJS_ERROR;
            }

            cl->buf = b;

            *ll = cl;
            ll = &cl->next;
        }
    }

    *ll = NULL;

    if (ngx_http_output_filter(r, out) == NGX_ERROR) {
        return NJS_ERROR;
    }

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_finish(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    ngx_http_request_t  *r;

    r = njs_value_data(njs_argument(args, 0));

    if (ngx_http_send_special(r, NGX_HTTP_LAST) == NGX_ERROR) {
        return NJS_ERROR;
    }

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_log(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_str_t            msg;
    ngx_connection_t    *c;
    ngx_log_handler_pt   handler;
    ngx_http_request_t  *r;

    r = njs_value_data(njs_argument(args, 0));
    c = r->connection;

    if (njs_vm_value_to_ext_string(vm, &msg, njs_argument(args, 1), 0)
        == NJS_ERROR)
    {
        return NJS_ERROR;
    }

    handler = c->log->handler;
    c->log->handler = NULL;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "js: %*s", msg.length, msg.start);

    c->log->handler = handler;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_get_http_version(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    ngx_str_t            v;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    switch (r->http_version) {

    case NGX_HTTP_VERSION_9:
        ngx_str_set(&v, "0.9");
        break;

    case NGX_HTTP_VERSION_10:
        ngx_str_set(&v, "1.0");
        break;

    default: /* NGX_HTTP_VERSION_11 */
        ngx_str_set(&v, "1.1");
        break;
    }

    return njs_string_create(vm, value, v.data, v.len, 0);
}


static njs_ret_t
ngx_http_js_ext_get_remote_address(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    ngx_connection_t    *c;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;
    c = r->connection;

    return njs_string_create(vm, value, c->addr_text.data, c->addr_text.len, 0);
}


static njs_ret_t
ngx_http_js_ext_get_header_in(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    nxt_str_t           *v;
    ngx_table_elt_t     *h;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;
    v = (nxt_str_t *) data;

    h = ngx_http_js_get_header(&r->headers_in.headers.part, v->start,
                               v->length);
    if (h == NULL) {
        return njs_string_create(vm, value, NULL, 0, 0);
    }

    return njs_string_create(vm, value, h->value.data, h->value.len, 0);
}


static njs_ret_t
ngx_http_js_ext_foreach_header_in(njs_vm_t *vm, void *obj, void *next)
{
    return ngx_http_js_ext_foreach_header(vm, obj, next,
                              offsetof(ngx_http_request_t, headers_in.headers));
}

static njs_ret_t
ngx_http_js_ext_get_arg(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    nxt_str_t           *v;
    ngx_str_t            arg;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;
    v = (nxt_str_t *) data;

    if (ngx_http_arg(r, v->start, v->length, &arg) == NGX_OK) {
        return njs_string_create(vm, value, arg.data, arg.len, 0);
    }

    return njs_string_create(vm, value, NULL, 0, 0);
}


static njs_ret_t
ngx_http_js_ext_foreach_arg(njs_vm_t *vm, void *obj, void *next)
{
    ngx_str_t           *entry, **e;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    entry = ngx_palloc(r->pool, sizeof(ngx_str_t));
    if (entry == NULL) {
        return NJS_ERROR;
    }

    *entry = r->args;

    e = (ngx_str_t **) next;
    *e = entry;

    return NJS_OK;
}


static njs_ret_t
ngx_http_js_ext_next_arg(njs_vm_t *vm, njs_value_t *value, void *obj,
    void *next)
{
    ngx_str_t **e = next;

    size_t      len;
    u_char     *p, *start, *end;
    ngx_str_t  *entry;

    entry = *e;

    if (entry->len == 0) {
        return NJS_DONE;
    }

    start = entry->data;
    end = start + entry->len;

    p = ngx_strlchr(start, end, '=');
    if (p == NULL) {
        return NJS_ERROR;
    }

    len = p - start;
    p++;

    p = ngx_strlchr(p, end, '&');

    if (p) {
        entry->data = &p[1];
        entry->len = end - entry->data;

    } else {
        entry->len = 0;
    }

    return njs_string_create(vm, value, start, len, 0);
}


static njs_ret_t
ngx_http_js_ext_get_variable(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    nxt_str_t                  *v;
    ngx_str_t                   name;
    ngx_uint_t                  key;
    ngx_http_request_t         *r;
    ngx_http_variable_value_t  *vv;

    r = (ngx_http_request_t *) obj;
    v = (nxt_str_t *) data;

    name.data = v->start;
    name.len = v->length;

    key = ngx_hash_strlow(name.data, name.data, name.len);

    vv = ngx_http_get_variable(r, &name, key);
    if (vv == NULL || vv->not_found) {
        return njs_string_create(vm, value, NULL, 0, 0);
    }

    return njs_string_create(vm, value, vv->data, vv->len, 0);
}


static njs_ret_t
ngx_http_js_ext_get_response(njs_vm_t *vm, njs_value_t *value, void *obj,
    uintptr_t data)
{
    ngx_http_js_ctx_t   *ctx;
    ngx_http_request_t  *r;

    r = (ngx_http_request_t *) obj;

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    njs_vm_retval_set(ctx->vm, &ctx->args[1]);

    return NJS_OK;
}


static njs_host_event_t
ngx_http_js_set_timer(njs_external_ptr_t external, uint64_t delay,
    njs_vm_event_t vm_event)
{
    ngx_event_t          *ev;
    ngx_http_request_t   *r;
    ngx_http_js_event_t  *js_event;

    r = (ngx_http_request_t *) external;

    ev = ngx_pcalloc(r->pool, sizeof(ngx_event_t));
    if (ev == NULL) {
        return NULL;
    }

    js_event = ngx_palloc(r->pool, sizeof(ngx_http_js_event_t));
    if (js_event == NULL) {
        return NULL;
    }

    js_event->request = r;
    js_event->vm_event = vm_event;
    js_event->ident = r->connection->fd;

    ev->data = js_event;
    ev->log = r->connection->log;
    ev->handler = ngx_http_js_timer_handler;

    ngx_add_timer(ev, delay);

    return ev;
}


static void
ngx_http_js_clear_timer(njs_external_ptr_t external, njs_host_event_t event)
{
    ngx_event_t  *ev = event;

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }
}


static void
ngx_http_js_timer_handler(ngx_event_t *ev)
{
    ngx_connection_t     *c;
    ngx_http_request_t   *r;
    ngx_http_js_event_t  *js_event;

    js_event = (ngx_http_js_event_t *) ev->data;

    r = js_event->request;

    c = r->connection;

    ngx_http_js_handle_event(r, js_event->vm_event);

    ngx_http_run_posted_requests(c);
}


static void
ngx_http_js_handle_event(ngx_http_request_t *r, njs_vm_event_t vm_event)
{
    njs_ret_t           rc;
    nxt_str_t           exception;
    ngx_http_js_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_js_module);

    njs_vm_post_event(ctx->vm, vm_event);

    rc = njs_vm_run(ctx->vm);

    if (rc == NJS_ERROR) {
        njs_vm_retval_to_ext_string(ctx->vm, &exception);

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "js exception: %*s", exception.length, exception.start);

        ngx_http_finalize_request(r, NGX_ERROR);
    }

    if (rc == NJS_OK) {
        ngx_http_post_request(r, NULL);
    }
}


static char *
ngx_http_js_include(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_js_loc_conf_t *jlcf = conf;

    size_t                 size;
    u_char                *start, *end;
    ssize_t                n;
    ngx_fd_t               fd;
    ngx_str_t             *value, file;
    nxt_int_t              rc;
    nxt_str_t              text;
    njs_vm_opt_t           options;
    ngx_file_info_t        fi;
    ngx_pool_cleanup_t    *cln;

    if (jlcf->vm) {
        return "is duplicate";
    }

    value = cf->args->elts;
    file = value[1];

    if (ngx_conf_full_name(cf->cycle, &file, 1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    fd = ngx_open_file(file.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           ngx_open_file_n " \"%s\" failed", file.data);
        return NGX_CONF_ERROR;
    }

    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, ngx_errno,
                      ngx_fd_info_n " \"%s\" failed", file.data);
        (void) ngx_close_file(fd);
        return NGX_CONF_ERROR;
    }

    size = ngx_file_size(&fi);

    start = ngx_pnalloc(cf->pool, size);
    if (start == NULL) {
        (void) ngx_close_file(fd);
        return NGX_CONF_ERROR;
    }

    n = ngx_read_fd(fd, start,  size);

    if (n == -1) {
        ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                      ngx_read_fd_n " \"%s\" failed", file.data);

        (void) ngx_close_file(fd);
        return NGX_CONF_ERROR;
    }

    if ((size_t) n != size) {
        ngx_log_error(NGX_LOG_ALERT, cf->log, 0,
                      ngx_read_fd_n " has read only %z of %O from \"%s\"",
                      n, size, file.data);

        (void) ngx_close_file(fd);
        return NGX_CONF_ERROR;
    }

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                      ngx_close_file_n " %s failed", file.data);
    }

    end = start + size;

    ngx_memzero(&options, sizeof(njs_vm_opt_t));

    options.backtrace = 1;
    options.ops = &ngx_http_js_ops;

    jlcf->vm = njs_vm_create(&options);
    if (jlcf->vm == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "failed to create JS VM");
        return NGX_CONF_ERROR;
    }

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        return NGX_CONF_ERROR;
    }

    cln->handler = ngx_http_js_cleanup_vm;
    cln->data = jlcf->vm;

    jlcf->req_proto = njs_vm_external_prototype(jlcf->vm,
                                                &ngx_http_js_externals[0]);
    if (jlcf->req_proto == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "failed to add request proto");
        return NGX_CONF_ERROR;
    }

    jlcf->res_proto = njs_vm_external_prototype(jlcf->vm,
                                                &ngx_http_js_externals[1]);
    if (jlcf->res_proto == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to add response proto");
        return NGX_CONF_ERROR;
    }

    rc = njs_vm_compile(jlcf->vm, &start, end);

    if (rc != NJS_OK) {
        njs_vm_retval_to_ext_string(jlcf->vm, &text);

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%*s, included",
                           text.length, text.start);
        return NGX_CONF_ERROR;
    }

    if (start != end) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "extra characters in js script: \"%*s\", included",
                           end - start, start);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_js_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t            *value, *fname;
    ngx_http_variable_t  *v;

    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;

    v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    fname = ngx_palloc(cf->pool, sizeof(ngx_str_t));
    if (fname == NULL) {
        return NGX_CONF_ERROR;
    }

    *fname = value[2];

    v->get_handler = ngx_http_js_variable;
    v->data = (uintptr_t) fname;

    return NGX_CONF_OK;
}


static char *
ngx_http_js_content(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_js_loc_conf_t *jlcf = conf;

    ngx_str_t                 *value;
    ngx_http_core_loc_conf_t  *clcf;

    if (jlcf->content.data) {
        return "is duplicate";
    }

    value = cf->args->elts;
    jlcf->content = value[1];

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_js_content_handler;

    return NGX_CONF_OK;
}


static void *
ngx_http_js_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_js_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_js_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->vm = NULL;
     *     conf->req_proto = NULL;
     *     conf->res_proto = NULL;
     */

    return conf;
}


static char *
ngx_http_js_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_js_loc_conf_t *prev = parent;
    ngx_http_js_loc_conf_t *conf = child;

    if (conf->vm == NULL) {
        conf->vm = prev->vm;
        conf->req_proto = prev->req_proto;
        conf->res_proto = prev->res_proto;
    }

    return NGX_CONF_OK;
}
