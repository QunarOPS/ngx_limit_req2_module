
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define LIMIT_REQ2_BLOCK_ACTION_NONE   0
#define LIMIT_REQ2_BLOCK_ACTION_QUERY  1
#define LIMIT_REQ2_BLOCK_ACTION_SET    2
#define LIMIT_REQ2_BLOCK_ACTION_CLEAR  3

typedef struct {
    u_char                       color;
    u_char                       dummy;
    u_short                      len;
    ngx_queue_t                  queue;
    ngx_msec_t                   last;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   excess;
    ngx_uint_t                   count;

    uint64_t                     block_stat;
    ngx_uint_t                   block_stat_base;
    ngx_uint_t                   block_stop_time;

    /* range count for computing qps */
    ngx_uint_t                   last_seg;
    ngx_uint_t                   curr_seg;

    u_char                       data[1];
} ngx_http_limit_req2_node_t;


typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
    ngx_queue_t                   queue;
} ngx_http_limit_req2_shctx_t;


typedef struct {
    ngx_int_t                    index;
    ngx_str_t                    var;
} ngx_http_limit_req2_variable_t;


typedef struct {
    ngx_int_t                    index;
    ngx_str_t                    var;
} ngx_http_limit_req_variable_t;


typedef struct {
    ngx_http_limit_req2_shctx_t *sh;
    ngx_slab_pool_t             *shpool;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   rate;
    ngx_http_complex_value_t     key;
    ngx_http_limit_req2_node_t  *node;

    ngx_uint_t                   rate_seg;
    ngx_uint_t                   last_seg;
    ngx_uint_t                   curr_seg;
    ngx_uint_t                   curr_seg_time_diff;

    ngx_array_t                 *limit_vars;
} ngx_http_limit_req2_ctx_t;


typedef struct {
    ngx_shm_zone_t              *shm_zone;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   burst;
    ngx_uint_t                   nodelay; /* unsigned  nodelay:1 */
    ngx_str_t                    forbid_action;

    /* 5x60x1800 */
    ngx_uint_t                   block_stat_interval;   /* 60 */
    ngx_uint_t                   block_stat_times;      /* 5 */
    ngx_uint_t                   block_time;            /* 1800 */

    ngx_uint_t                   rate_seg;
} ngx_http_limit_req2_t;


typedef struct {
    ngx_flag_t                   enable;

    ngx_array_t                 *rules;

    ngx_str_t                    geo_var_name;
    ngx_int_t                    geo_var_index;
    ngx_str_t                    geo_var_value;

    ngx_array_t                  limits;
    ngx_uint_t                   limit_log_level;
    ngx_uint_t                   delay_log_level;
    ngx_uint_t                   status_code;

    ngx_int_t                    block_action;
    ngx_int_t                    block_time;
    ngx_shm_zone_t              *block_shm_zone;
    ngx_array_t                 *block_limit_vars;

    ngx_int_t                    enable_record_rate;
} ngx_http_limit_req2_conf_t;

static ngx_str_t   ngx_http_limit_req2_rate = ngx_string("limit_req2_rate");

static void ngx_http_limit_req2_delay(ngx_http_request_t *r);
static ngx_int_t ngx_http_limit_req2_lookup(ngx_http_request_t *r,
    ngx_http_limit_req2_t *limit_req2, ngx_uint_t hash, ngx_uint_t *ep,
    ngx_uint_t *bst, ngx_uint_t *last_seg, ngx_uint_t *curr_seg,
    ngx_uint_t *curr_seg_time_diff, ngx_int_t block_action);

static void ngx_http_limit_req2_expire(ngx_http_request_t *r,
    ngx_http_limit_req2_ctx_t *ctx, ngx_uint_t n);

static void *ngx_http_limit_req2_create_conf(ngx_conf_t *cf);
static char *ngx_http_limit_req2_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_limit_req2_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_req2(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_req2_whitelist(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_req2_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_limit_req2_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_limit_req2_add_variables(ngx_conf_t *cf);


static inline
int get_stat_bit(uint64_t stat, int idx)
{
    stat >>= idx;
    return stat & 1;
}

static inline
void set_stat_bit(uint64_t *pstat, int idx, int val)
{
    uint64_t tmp = val <<= idx;
    *pstat |= tmp;
}

static ngx_conf_enum_t  ngx_http_limit_req2_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_limit_req2_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_limit_req2_commands[] = {

    { ngx_string("limit_req2_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_limit_req2_zone,
      0,
      0,
      NULL },

    { ngx_string("limit_req2"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1234,
      ngx_http_limit_req2,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_req2_whitelist"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_limit_req2_whitelist,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_req2_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_req2_conf_t, limit_log_level),
      &ngx_http_limit_req2_log_levels },

    { ngx_string("limit_req2_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_req2_conf_t, status_code),
      &ngx_http_limit_req2_status_bounds },

      { ngx_string("limit_req2_block"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
      ngx_http_limit_req2_block,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_req2_conf_t, enable),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_limit_req2_module_ctx = {
    ngx_http_limit_req2_add_variables,     /* preconfiguration */
    ngx_http_limit_req2_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_limit_req2_create_conf,       /* create location configuration */
    ngx_http_limit_req2_merge_conf         /* merge location configuration */
};


ngx_module_t  ngx_http_limit_req2_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_req2_module_ctx,       /* module context */
    ngx_http_limit_req2_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static inline ngx_int_t
ngx_http_limit_req2_ip_filter(ngx_http_request_t *r,
    ngx_http_limit_req2_conf_t *lrcf)
{
    ngx_http_variable_value_t    *vv;

    if (lrcf->geo_var_index != NGX_CONF_UNSET) {
        vv = ngx_http_get_indexed_variable(r, lrcf->geo_var_index);

        if (vv == NULL || vv->not_found) {
            return NGX_DECLINED;
        }

        if ((vv->len == lrcf->geo_var_value.len)
             && (ngx_memcmp(vv->data, lrcf->geo_var_value.data, vv->len) == 0))
        {
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_limit_req2_copy_variables(ngx_http_request_t *r, uint32_t *hash,
    ngx_array_t *limit_vars, ngx_http_limit_req2_node_t *node)
{
    u_char                        *p;
    size_t                         len, total_len;
    ngx_uint_t                     j;
    ngx_http_variable_value_t     *vv;
    ngx_http_limit_req2_variable_t *lrv;

    len = 0;
    total_len = 0;
    p = NULL;

    if (node != NULL) {
        p = node->data;
    }

    lrv = limit_vars->elts;
    for (j = 0; j < limit_vars->nelts; j++) {
        vv = ngx_http_get_indexed_variable(r, lrv[j].index);
        if (vv == NULL || vv->not_found) {
            total_len = 0;
            break;
        }

        len = vv->len;

        if (len == 0) {
            total_len = 0;
            break;
        }

        if (len > 65535) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" variable "
                          "is more than 65535 bytes: \"%v\"",
                          &lrv[j].var, vv);
            total_len = 0;
            break;
        }

        if (node == NULL) {
            total_len += len;
            ngx_crc32_update(hash, vv->data, len);
        } else {
            p = ngx_cpymem(p, vv->data, len);
        }
    }

    return total_len;
}


static ngx_int_t
ngx_http_limit_req2_handler(ngx_http_request_t *r)
{
    size_t                         n, total_len;
    uint32_t                       hash;
    ngx_int_t                      rc;
    ngx_msec_t                     delay_time;
    ngx_uint_t                     excess, delay_excess, delay_postion,
                                   nodelay, i;
    ngx_time_t                    *tp;
    ngx_rbtree_node_t             *node;
    ngx_http_limit_req2_t         *limit_req2;
    ngx_http_limit_req2_ctx_t      *ctx;
    ngx_http_limit_req2_node_t     *lr;
    ngx_http_limit_req2_conf_t     *lrcf;

    ngx_uint_t                     last_seg, curr_seg, curr_seg_time_diff;
    ngx_uint_t                     block_stop_time = 0;

    delay_excess = 0;
    excess = 0;
    delay_postion = 0;
    nodelay = 0;
    ctx = NULL;
    rc = NGX_DECLINED;

    last_seg = 0;
    curr_seg = 0;
    curr_seg_time_diff = 0;

    if (r->main->limit_req_set) {
        return NGX_DECLINED;
    }

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_req2_module);
    if (lrcf->rules == NULL) {
        return NGX_DECLINED;
    }

    if (!lrcf->enable) {
        return NGX_DECLINED;
    }

    /* filter whitelist */
    if (ngx_http_limit_req2_ip_filter(r, lrcf) == NGX_OK) {
        return NGX_DECLINED;
    }

    /* to match limit_req2 rule*/
    limit_req2 = lrcf->rules->elts;
    for (i = 0; i < lrcf->rules->nelts; i++) {
        ctx = limit_req2[i].shm_zone->data;

        ngx_crc32_init(hash);

        total_len = ngx_http_limit_req2_copy_variables(
          r, &hash, ctx->limit_vars, NULL);
        if (total_len == 0) {
            continue;
        }

        ngx_crc32_final(hash);

        ngx_shmtx_lock(&ctx->shpool->mutex);

        ngx_http_limit_req2_expire(r, ctx, 1);

        excess = 0;
        rc = ngx_http_limit_req2_lookup(r, &limit_req2[i], hash, &excess,
                &block_stop_time, &last_seg, &curr_seg,
                &curr_seg_time_diff, 0);

        ngx_log_debug6(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit_req2 module: %i %ui.%03ui "
                       "block_stop_time: %ui "
                       "hash is %D total_len is %uz",
                       rc, excess / 1000, excess % 1000,
                       block_stop_time,
                       hash, total_len);

        /*
         * add variable for computing rate
         */
        if (limit_req2[i].rate_seg > 0) {

            ctx->rate_seg = limit_req2[i].rate_seg;
            ctx->last_seg = last_seg;
            ctx->curr_seg = curr_seg;
            ctx->curr_seg_time_diff = curr_seg_time_diff;

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "limit_req2 rate_seg : %ui "
                "last_seg : %ui "
                "curr_seg : %ui "
                "curr_seg_time_diff : %ui",
                limit_req2[i].rate_seg, last_seg, curr_seg, curr_seg_time_diff);
        }

        /* first limit_req2 */
        if (rc == NGX_DECLINED) {

            n = offsetof(ngx_rbtree_node_t, color)
                + offsetof(ngx_http_limit_req2_node_t, data)
                + total_len;

            node = ngx_slab_alloc_locked(ctx->shpool, n);
            if (node == NULL) {
                ngx_http_limit_req2_expire(r, ctx, 0);
                node = ngx_slab_alloc_locked(ctx->shpool, n);
                if (node == NULL) {
                    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                                  "could not allocate node%s",
                                  ctx->shpool->log_ctx);

                    ngx_shmtx_unlock(&ctx->shpool->mutex);
                    return lrcf->status_code;
                }
            }

            lr = (ngx_http_limit_req2_node_t *) &node->color;

            node->key = hash;
            lr->len = (u_short) total_len;

            tp = ngx_timeofday();
            lr->last = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

            lr->excess = 0;
            lr->block_stat = 0;
            lr->block_stat_base = 0;
            lr->block_stop_time = 0;

            lr->last_seg = 0;
            lr->curr_seg = 1;
            ngx_http_limit_req2_copy_variables(r, &hash, ctx->limit_vars, lr);

            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);
            ngx_rbtree_insert(&ctx->sh->rbtree, node);

            ngx_shmtx_unlock(&ctx->shpool->mutex);

            continue;
        }

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        if (rc == NGX_BUSY || rc == NGX_ERROR) {
            break;
        }

        /* NGX_AGAIN or NGX_OK */

        if (delay_excess < excess) {
            delay_excess = excess;
            nodelay = limit_req2[i].nodelay;
            delay_postion = i;
        }
    }

    r->main->limit_req_set = 1;

    if (rc == NGX_BUSY || rc == NGX_ERROR) {
        if (rc == NGX_BUSY) {
            if (block_stop_time) {
                ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                            "limit_req2 blocking requests, "
                            "block_stop_time: %ui by zone \"%V\"",
                            block_stop_time,
                            &limit_req2[i].shm_zone->shm.name);

            } else {
                ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                            "limit_req2 limiting requests, "
                            "excess: %ui.%03ui by zone \"%V\"",
                            excess / 1000, excess % 1000,
                            &limit_req2[i].shm_zone->shm.name);
            }
        }

        if (rc == NGX_ERROR || limit_req2[i].forbid_action.len == 0) {
            return lrcf->status_code;

        } else if (limit_req2[i].forbid_action.data[0] == '@') {

            ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                          "limiting requests, forbid_action is %V",
                          &limit_req2[i].forbid_action);
            (void) ngx_http_named_location(r, &limit_req2[i].forbid_action);

        } else {

            ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                          "limiting requests, forbid_action is %V",
                          &limit_req2[i].forbid_action);
            (void) ngx_http_internal_redirect(r,
                                             &limit_req2[i].forbid_action,
                                             &r->args);
        }

        ngx_http_finalize_request(r, NGX_DONE);
        return NGX_DONE;
    }

    /* rc = NGX_AGAIN */
    if (delay_excess != 0) {

        if (nodelay) {
            return NGX_DECLINED;
        }

        delay_time = (ngx_msec_t) delay_excess * 1000 / ctx->rate;
        ngx_log_error(lrcf->delay_log_level, r->connection->log, 0,
                      "delaying request,"
                      "excess: %ui.%03ui, by zone \"%V\", delay \"%M\" ms",
                      delay_excess / 1000, delay_excess % 1000,
                      &limit_req2[delay_postion].shm_zone->shm.name, delay_time);

        if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->read_event_handler = ngx_http_test_reading;
        r->write_event_handler = ngx_http_limit_req2_delay;
        ngx_add_timer(r->connection->write, delay_time);

        return NGX_AGAIN;
    }

    /* rc == NGX_OK or rc == NGX_DECLINED */

    return NGX_DECLINED;
}


static void
ngx_http_limit_req2_delay(ngx_http_request_t *r)
{
    ngx_event_t  *wev;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit_req2 delay");

    wev = r->connection->write;

    if (!wev->timedout) {

        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }

        return;
    }

    wev->timedout = 0;

    if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    r->read_event_handler = ngx_http_block_reading;
    r->write_event_handler = ngx_http_core_run_phases;

    ngx_http_core_run_phases(r);
}


static void
ngx_http_limit_req2_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t          **p;
    ngx_http_limit_req2_node_t  *lrn, *lrnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lrn = (ngx_http_limit_req2_node_t *) &node->color;
            lrnt = (ngx_http_limit_req2_node_t *) &temp->color;

            p = (ngx_memn2cmp(lrn->data, lrnt->data, lrn->len, lrnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_http_limit_req2_lookup(ngx_http_request_t *r,
    ngx_http_limit_req2_t *limit_req2, ngx_uint_t hash, ngx_uint_t *ep,
    ngx_uint_t *bst, ngx_uint_t *last_seg, ngx_uint_t *curr_seg,
    ngx_uint_t *curr_seg_time_diff, ngx_int_t block_action)
{
    u_char                          *lr_data, *lr_last;
    size_t                           lr_vv_len;
    ngx_int_t                        rc, excess;
    ngx_uint_t                       i;
    ngx_time_t                      *tp;
    ngx_msec_t                       now;
    ngx_msec_int_t                   ms;
    ngx_rbtree_node_t               *node, *sentinel;
    ngx_http_limit_req2_ctx_t       *ctx;
    ngx_http_limit_req2_node_t      *lr;
    ngx_http_variable_value_t       *vv;
    ngx_http_limit_req2_variable_t  *lrv;

    ngx_http_limit_req2_conf_t      *lrcf;

    ngx_uint_t                       stat_interval, stat_times, now_sec, diff;
    ngx_int_t                        check_all_bit, last_zero_pos;
    ngx_uint_t                       j;

    ngx_msec_t                       last_rate_seg;
    ngx_msec_t                       curr_rate_seg;

    ctx = limit_req2->shm_zone->data;

    tp = ngx_timeofday();
    now_sec = (ngx_uint_t) (tp->sec);

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;
    rc = -1;

    lrv = ctx->limit_vars->elts;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "limit_req2_lookup hash : %i", hash);

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lr = (ngx_http_limit_req2_node_t *) &node->color;

        lr_data = lr->data;
        lr_last = lr_data + lr->len;

        for (i = 0; i < ctx->limit_vars->nelts; i++) {
            vv = ngx_http_get_indexed_variable(r, lrv[i].index);

            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "limit_req2 vv is %i %v node is %s",
                           lrv[i].index, vv, lr_data);

            lr_vv_len = ngx_min(lr_last - lr_data, vv->len);

            if ((rc = ngx_memcmp(vv->data, lr_data, lr_vv_len)) != 0) {
                break;
            }

            if (lr_vv_len != vv->len) {
                rc = 1;
                break;
            }

            /* lr_vv_len == vv->len */
            lr_data += lr_vv_len;
        }

        if (rc == 0 && lr_last > lr_data) {
            rc = -1;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit_req2 lookup is : %i, size is %ui",
                       rc, ctx->limit_vars->nelts);

        if (rc == 0) {
            ngx_queue_remove(&lr->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

            ngx_log_debug5(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "limit_req2 lookup sucess "
                    "block_action: %i "
                    "now_sec: %ui "
                    "block_stop_time: %i "
                    "excess: %ui.%03ui",
                    block_action, now_sec, lr->block_stop_time,
                    lr->excess / 1000, lr->excess % 1000);

            if (block_action == LIMIT_REQ2_BLOCK_ACTION_QUERY) {

                if (lr->block_stop_time > now_sec) {
                    *bst = lr->block_stop_time;
                } else {
                    *bst = 0;
                }

                return NGX_OK;

            } else if (block_action == LIMIT_REQ2_BLOCK_ACTION_SET) {

                lrcf = ngx_http_get_module_loc_conf(r,
                                            ngx_http_limit_req2_module);

                lr->block_stop_time = now_sec + lrcf->block_time;

                *bst = lr->block_stop_time;

                return NGX_OK;

            } else if (block_action == LIMIT_REQ2_BLOCK_ACTION_CLEAR) {

                lr->block_stop_time = 0;
                lr->excess = 0;

                return NGX_OK;

            } else {

                now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);
                ms = (ngx_msec_int_t) (now - lr->last);

                /* block check */
                if (lr->block_stop_time >= now_sec) {
                    *bst = lr->block_stop_time;
                    return NGX_BUSY;
                }

                excess = lr->excess - ctx->rate * ngx_abs(ms) / 1000 + 1000;

                if (excess < 0) {
                    excess = 0;
                }

                *ep = excess;

                if ((ngx_uint_t) excess > limit_req2->burst) {

                    /* stat for block */
                    stat_times = limit_req2->block_stat_times;

                    if (stat_times != 0) {

                        stat_interval = limit_req2->block_stat_interval;
                        diff = now_sec - lr->block_stat_base;

                        if (diff >= stat_interval * stat_times) {

                            lr->block_stat_base = now_sec;
                            lr->block_stat = 1;

                        } else if (diff >= (stat_times-1) * stat_interval) {

                            set_stat_bit(&lr->block_stat,
                                                        stat_times - 1, 1);
                            check_all_bit = 1;
                            last_zero_pos = 0;

                            for (j = 0; j < stat_times - 1; ++j) {
                                if (!get_stat_bit(lr->block_stat, j)) {
                                    check_all_bit = 0;
                                    last_zero_pos = j;
                                }
                            }

                            if (check_all_bit) {
                                /* auto block */
                                lr->block_stop_time = now_sec
                                                + limit_req2->block_time;

                                lr->block_stat >>= 1;
                                lr->block_stat_base += stat_interval;
                            } else {
                                lr->block_stat >>= last_zero_pos + 1;
                                lr->block_stat_base += (last_zero_pos + 1)
                                    * stat_interval;
                            }
                        } else {
                            set_stat_bit(&lr->block_stat,
                                        diff / stat_interval, 1);
                        }

                        ngx_log_debug4(NGX_LOG_DEBUG_HTTP,
                                r->connection->log, 0,
                                "limit_req2 now_sec: %ui "
                                "block stop_time: %ui "
                                "block_stat_base: %ui "
                                "block stat: %ul ",
                                now_sec, lr->block_stop_time,
                                lr->block_stat_base, lr->block_stat);
                    }

                    return NGX_BUSY;
                }

                if (limit_req2->rate_seg != 0) {
                    last_rate_seg = lr->last / limit_req2->rate_seg;
                    curr_rate_seg = now / limit_req2->rate_seg;
                    if (curr_rate_seg > last_rate_seg + 1) {

                        lr->last_seg = 0;
                        lr->curr_seg = 1;

                    } else if (curr_rate_seg == last_rate_seg + 1) {

                        lr->last_seg = lr->curr_seg;
                        lr->curr_seg = 1;

                    } else if (curr_rate_seg == last_rate_seg) {

                        ++lr->curr_seg;

                    } else {
                        /* never appear */
                        lr->last_seg = 0;
                        lr->curr_seg = 0;
                    }

                    *last_seg = lr->last_seg;
                    *curr_seg = lr->curr_seg;
                    *curr_seg_time_diff = now % limit_req2->rate_seg;
                }

                lr->excess = excess;
                lr->last = now;


                if (excess) {
                    return NGX_AGAIN;

                }

                return NGX_OK;
            }
        }

        node = (rc < 0) ? node->left : node->right;
    }

    *ep = 0;

    *last_seg = 0;
    *curr_seg = 1;
    *curr_seg_time_diff = 0;

    return NGX_DECLINED;
}


static void
ngx_http_limit_req2_expire(ngx_http_request_t *r,
    ngx_http_limit_req2_ctx_t *ctx, ngx_uint_t n)
{
    ngx_int_t                   excess;
    ngx_time_t                 *tp;
    ngx_msec_t                  now;
    ngx_queue_t                *q;
    ngx_msec_int_t              ms;
    ngx_rbtree_node_t          *node;
    ngx_http_limit_req2_node_t *lr;
    ngx_http_limit_req2_node_t *first_lr;
    ngx_uint_t                  m;

    tp = ngx_timeofday();

    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

    /*
     * n == 1 deletes one or two zero rate entries
     * n == 0 deletes oldest entry by force
     *        and one or two zero rate entries
     */

    m = 0;
    first_lr = NULL;

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        lr = ngx_queue_data(q, ngx_http_limit_req2_node_t, queue);

        if (lr->block_stop_time > (ngx_uint_t)tp->sec
                && first_lr != lr && m++ < 100) {

            ngx_queue_remove(&lr->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

            continue;
        }

        if (m == 1) {
            first_lr = lr;
        }

        if (n++ != 0) {

            ms = (ngx_msec_int_t) (now - lr->last);
            ms = ngx_abs(ms);

            if (ms < 60000) {
                return;
            }

            excess = lr->excess - ctx->rate * ms / 1000;

            if (excess > 0) {
                return;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) lr - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
    }
}


static ngx_int_t
ngx_http_limit_req2_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_req2_ctx_t  *octx = data;

    size_t                       len;
    ngx_uint_t                   i, j;
    ngx_http_limit_req2_ctx_t    *ctx;
    ngx_http_limit_req2_variable_t *v1, *v2;

    ctx = shm_zone->data;
    v1 = ctx->limit_vars->elts;

    if (octx) {
        v2 = octx->limit_vars->elts;
        if (ctx->limit_vars->nelts != octx->limit_vars->nelts) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "limit_req2 \"%V\" uses the \"%V\" variable "
                          "while previously it used the \"%V\" variable",
                          &shm_zone->shm.name, &v1[0].var, &v2[0].var);
            return NGX_ERROR;
        }

        for (i = 0, j = 0;
             i < ctx->limit_vars->nelts && j < octx->limit_vars->nelts;
             i++, j++)
        {
            if (ngx_strcmp(v1[i].var.data, v2[j].var.data) != 0) {
                ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                              "limit_req2 \"%V\" uses the \"%V\" variable "
                              "while previously it used the \"%V\" variable",
                              &shm_zone->shm.name, &v1[i].var,
                              &v2[j].var);
                return NGX_ERROR;
            }
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_limit_req2_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_limit_req2_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in limit_req2 zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_req2 zone \"%V\"%Z",
                &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_req2_block_handler(ngx_http_request_t *r)
{
    size_t                         n, total_len;
    uint32_t                       hash;
    ngx_int_t                      rc;
    ngx_int_t                      block_action;
    ngx_uint_t                     excess;
    ngx_uint_t                     block_stop_time = 0;
    ngx_time_t                    *tp;
    ngx_rbtree_node_t             *node;
    ngx_http_limit_req2_ctx_t      *ctx;
    ngx_http_limit_req2_node_t     *lr;
    ngx_http_limit_req2_conf_t     *lrcf;
    ngx_buf_t                      *b;
    ngx_chain_t                    out;

    ngx_uint_t                     last_seg, curr_seg, curr_seg_time_diff;

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_req2_module);

    if (lrcf->block_action == 0) {
        return NGX_DECLINED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    b = ngx_create_temp_buf(r->pool, 1024);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    block_action = lrcf->block_action;
    ctx = lrcf->block_shm_zone->data;

    ngx_crc32_init(hash);

    total_len = 0;
    total_len = ngx_http_limit_req2_copy_variables(r, &hash,
            lrcf->block_limit_vars, NULL);

    if (total_len != 0) {
        ngx_crc32_final(hash);
    }

    if (total_len == 0) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                        "limit_req2_block limit vars is empty");

        b->last = ngx_cpymem(b->last,
                "{\"ret\": false, \"errmsg\": \"limit vars is empty\"}",
            sizeof("{\"ret\": false, \"errmsg\": \"limit vars is empty\"}") - 1);

    } else if (block_action == LIMIT_REQ2_BLOCK_ACTION_QUERY) { /* query */

        ngx_shmtx_lock(&ctx->shpool->mutex);
        rc = ngx_http_limit_req2_lookup(r,
                NULL, hash, &excess,
                &block_stop_time,
                &last_seg, &curr_seg, &curr_seg_time_diff,
                block_action);
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        if (rc == NGX_OK) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                            "limit_req2_block_query, "
                            "block_stop_time: %ui "
                            "zone: \"%V\"",
                            block_stop_time,
                            &lrcf->block_shm_zone->shm.name);
        } else {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                            "limit_req2_block_query not exists node, "
                            "zone: \"%V\"",
                            &lrcf->block_shm_zone->shm.name);
        }

        b->last = ngx_sprintf(b->last,
                "{\"ret\": true, \"block_stop_time\": %ui}", block_stop_time);

    } else if (block_action == LIMIT_REQ2_BLOCK_ACTION_SET) { /* set */
        ngx_shmtx_lock(&ctx->shpool->mutex);
        rc = ngx_http_limit_req2_lookup(r,
                NULL, hash, &excess,
                &block_stop_time,
                &last_seg, &curr_seg, &curr_seg_time_diff,
                block_action);

        if (rc == NGX_DECLINED) {
            n = offsetof(ngx_rbtree_node_t, color)
                + offsetof(ngx_http_limit_req2_node_t, data)
                + total_len;

            node = ngx_slab_alloc_locked(ctx->shpool, n);
            if (node == NULL) {
                ngx_http_limit_req2_expire(r, ctx, 0);
                node = ngx_slab_alloc_locked(ctx->shpool, n);
                if (node == NULL) {
                    ngx_shmtx_unlock(&ctx->shpool->mutex);
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
            }

            lr = (ngx_http_limit_req2_node_t *) &node->color;

            node->key = hash;
            lr->len = (u_char) total_len;

            tp = ngx_timeofday();
            lr->last = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

            lr->excess = 0;
            lr->block_stat = 0;
            lr->block_stat_base = 0;
            lr->block_stop_time = tp->sec + lrcf->block_time;

            block_stop_time = tp->sec + lrcf->block_time;

            ngx_http_limit_req2_copy_variables(r, &hash,
                                               lrcf->block_limit_vars, lr);

            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);
            ngx_rbtree_insert(&ctx->sh->rbtree, node);

            ngx_shmtx_unlock(&ctx->shpool->mutex);

            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                        "limit_req2_block_set not exists but create new node, "
                        "block_stop_time: %ui "
                        "now_sec: %ui "
                        "block_time: %ui "
                        "zone: \"%V\"",
                        lr->block_stop_time,
                        tp->sec,
                        lrcf->block_time,
                        &lrcf->block_shm_zone->shm.name);

        } else {
            ngx_shmtx_unlock(&ctx->shpool->mutex);

            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                            "limit_req2_block_set exists node, "
                            "block_stop_time: %ui "
                            "block_time: %ui "
                            "zone: \"%V\"",
                            block_stop_time,
                            lrcf->block_time,
                            &lrcf->block_shm_zone->shm.name);
        }

        b->last = ngx_sprintf(b->last,
            "{\"ret\": true, \"block_stop_time\": %ui, \"block_time\": %ui}",
            block_stop_time, lrcf->block_time);

    } else if (block_action == LIMIT_REQ2_BLOCK_ACTION_CLEAR) { /*clear*/
        ngx_shmtx_lock(&ctx->shpool->mutex);
        rc = ngx_http_limit_req2_lookup(r,
                NULL, hash, &excess,
                &block_stop_time,
                &last_seg, &curr_seg, &curr_seg_time_diff,
                block_action);
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        if (rc == NGX_OK) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                            "limit_req2_block_clear exists node, "
                            "zone: \"%V\"",
                            &lrcf->block_shm_zone->shm.name);
        } else {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                            "limit_req2_block_clear not exists node, "
                            "zone: \"%V\"",
                            &lrcf->block_shm_zone->shm.name);
        }

        b->last = ngx_cpymem(b->last,
                "{\"ret\": true}", sizeof("{\"ret\": true}") - 1);
    } else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                            "limit_req2_block no such action");

        b->last = ngx_cpymem(b->last,
                "{\"ret\": false}", sizeof("{\"ret\": false}") - 1);
    }

    ngx_str_set(&r->headers_out.content_type, "application/json;charset=UTF-8");

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }


    out.buf = b;
    out.next = NULL;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    if (r == r->main)
        b->last_buf = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static void *
ngx_http_limit_req2_create_conf(ngx_conf_t *cf)
{
    ngx_http_limit_req2_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_req2_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->shm_zone = NULL;
     *     conf->burst = 0;
     *     conf->nodelay = 0;
     */

    conf->enable = NGX_CONF_UNSET;
    conf->limit_log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->geo_var_index = NGX_CONF_UNSET;

    conf->block_action = NGX_CONF_UNSET;
    conf->block_time = 1800;
    conf->block_shm_zone = NULL;
    conf->block_limit_vars = NULL;

    conf->enable_record_rate = 0;

    return conf;
}


static char *
ngx_http_limit_req2_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_req2_conf_t *prev = parent;
    ngx_http_limit_req2_conf_t *conf = child;

    if (conf->rules == NULL) {
        conf->rules = prev->rules;
    }

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    ngx_conf_merge_uint_value(conf->limit_log_level, prev->limit_log_level,
                              NGX_LOG_ERR);

    conf->delay_log_level = (conf->limit_log_level == NGX_LOG_INFO) ?
                                NGX_LOG_INFO : conf->limit_log_level + 1;

    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    ngx_conf_merge_value(conf->geo_var_index, prev->geo_var_index,
                         NGX_CONF_UNSET);

    ngx_conf_merge_str_value(conf->geo_var_value, prev->geo_var_value, "");

    ngx_conf_merge_value(conf->block_action, prev->block_action, 0);

    ngx_conf_merge_value(conf->block_time, prev->block_time, 1800);

    if (conf->block_shm_zone == NULL) {
        conf->block_shm_zone = prev->block_shm_zone;
    }

    if (conf->block_limit_vars == NULL) {
        conf->block_limit_vars = prev->block_limit_vars;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_req2_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                         *p;
    size_t                          len;
    ssize_t                         size;
    ngx_str_t                      *value, name, s;
    ngx_int_t                       rate, scale;
    ngx_uint_t                      i;
    ngx_array_t                    *variables;
    ngx_shm_zone_t                 *shm_zone;
    ngx_http_limit_req2_ctx_t      *ctx;
    ngx_http_limit_req2_variable_t *v;

    value = cf->args->elts;

    ctx = NULL;
    v = NULL;
    size = 0;
    rate = 1;
    scale = 1;
    name.len = 0;

    variables = ngx_array_create(cf->pool, 5,
                                 sizeof(ngx_http_limit_req2_variable_t));
    if (variables == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                scale = 1;
                len -= 3;

            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                scale = 60;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (value[i].data[0] == '$') {

            value[i].len--;
            value[i].data++;

            v = ngx_array_push(variables);
            if (v == NULL) {
                return NGX_CONF_ERROR;
            }

            v->index = ngx_http_get_variable_index(cf, &value[i]);
            if (v->index == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            v->var = value[i];

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }


    if (variables->nelts == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no variable is defined for %V \"%V\"",
                           &cmd->name, &name);
        return NGX_CONF_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_req2_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }
    ctx->rate = rate * 1000 / scale;
    ctx->limit_vars = variables;

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_limit_req2_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                   "limit_req2_zone \"%V\" is already bound to variable \"%V\"",
                   &value[1], &v->var);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_req2_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}

static char *
ngx_http_limit_req2(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_limit_req2_conf_t    *lrcf = conf;

    ngx_int_t                      burst;
    ngx_str_t                     *value, s, forbid_action;
    ngx_uint_t                     i, nodelay;
    ngx_shm_zone_t                *shm_zone;
    ngx_http_limit_req2_t         *limit_req2;
    ngx_uint_t                     rate_seg;
    u_char                        *p1, *p2;

    if (lrcf->rules == NULL) {
        lrcf->rules = ngx_array_create(cf->pool, 5,
                                       sizeof(ngx_http_limit_req2_t));
        if (lrcf->rules == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    limit_req2 = ngx_array_push(lrcf->rules);
    if (limit_req2 == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(limit_req2, sizeof(ngx_http_limit_req2_t));

    value = cf->args->elts;
    if (cf->args->nelts == 2) {
        if (ngx_strncmp(value[1].data, "off", 3) == 0) {
            lrcf->enable = 0;
            return NGX_CONF_OK;
        }
    }

    lrcf->enable = 1;

    shm_zone = NULL;
    burst = 0;
    nodelay = 0;
    ngx_str_null(&forbid_action);
    rate_seg = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                             &ngx_http_limit_req2_module);
            if (shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "forbid_action=", 14) == 0) {

            s.len = value[i].len - 14;
            s.data = value[i].data + 14;

            if (s.len < 2 || (s.data[0] != '@' && s.data[0] != '/')) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid forbid_action \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            forbid_action = s;

            continue;
        }

        if (ngx_strcmp(value[i].data, "nodelay") == 0) {
            nodelay = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "block=", 6) == 0) {

            s.len = value[i].len - 6;
            s.data = value[i].data + 6;

            /* 5x60x1000 */
            p1 = (u_char *)ngx_strchr((u_char *)s.data, 'x');
            if (!p1) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid limit_req block \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            p2 = (u_char *)ngx_strchr((u_char *)(p1 + 1), 'x');
            if (!p2) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "invalid limit_req block \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            limit_req2->block_stat_times = ngx_atoi(s.data, p1 - s.data);
            if (limit_req2->block_stat_times <= 0) {
                limit_req2->block_stat_times = 0;

                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                             "invalid block_stat_times \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (limit_req2->block_stat_times > 64 ) {
                limit_req2->block_stat_times = 64;
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                             "block_stat_times must <= 64 \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            limit_req2->block_stat_interval = ngx_atoi(p1 + 1, p2 - p1 - 1);
            if (limit_req2->block_stat_interval <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "invalid block_stat_interval \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            limit_req2->block_time = ngx_atoi(p2 + 1, s.data + s.len - p2 - 1);
            if (limit_req2->block_time <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "invalid block_time \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            ngx_conf_log_error(NGX_LOG_DEBUG_HTTP, cf, 0,
                       "limit_req2 module: "
                       "block_stat_times: %ui "
                       "block_stat_interval: %ui "
                       "block_time: %ui ",
                       limit_req2->block_stat_times,
                       limit_req2->block_stat_interval,
                       limit_req2->block_time
                       );

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate_seg=", 9) == 0) {

            rate_seg = ngx_atoi(value[i].data + 9, value[i].len - 9);

            if (lrcf->enable_record_rate && rate_seg > 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "duplicate rate_seg \"%V\" in limit_req2 config",
                        &value[i]);

                return NGX_CONF_ERROR;
            }

            lrcf->enable_record_rate = 1;

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown limit_req_zone \"%V\"",
                           &shm_zone->shm.name);
        return NGX_CONF_ERROR;
    }

    limit_req2->shm_zone = shm_zone;
    limit_req2->burst = burst * 1000;
    limit_req2->rate_seg = rate_seg;
    limit_req2->nodelay = nodelay;
    limit_req2->forbid_action = forbid_action;

    limit_req2 = lrcf->rules->elts;
    for (i = 0; i < lrcf->rules->nelts - 1; i++) {
        if (shm_zone == limit_req2[i].shm_zone) {
            return "is duplicate";
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_req2_whitelist(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_limit_req2_conf_t  *lrcf = conf;

    ngx_str_t              *value, s;
    ngx_uint_t              i;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "geo_var_name=", 13) == 0) {

            s.len = value[i].len - 13;
            s.data = value[i].data + 13;

            lrcf->geo_var_name = s;

            lrcf->geo_var_index = ngx_http_get_variable_index(cf,
                &lrcf->geo_var_name);

            if (lrcf->geo_var_index == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "geo_var_value=", 14) == 0) {

            s.len = value[i].len - 14;
            s.data = value[i].data + 14;

            lrcf->geo_var_value = s;

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_req2_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_limit_req2_conf_t  *lrcf = conf;
    char                         *rv;
    ngx_str_t                    s, *value;
    ngx_int_t                    block_action = 0;
    ngx_int_t                    block_time = 0;
    ngx_uint_t                   i;
    ngx_shm_zone_t              *shm_zone = NULL;
    ngx_array_t                 *variables;
    ngx_http_limit_req2_variable_t  *v;

    variables = ngx_array_create(cf->pool, 5,
                                 sizeof(ngx_http_limit_req2_variable_t));
    if (variables == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (cf->args->nelts == 2) {
        rv = ngx_conf_set_flag_slot(cf, cmd, lrcf);
        return rv;
    }


    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "action=", 7) == 0) {
            s.len = value[i].len - 7;
            s.data = value[i].data + 7;

            if (ngx_strncmp(s.data, "query",  5) == 0) {
                block_action = LIMIT_REQ2_BLOCK_ACTION_QUERY; /* query */
            } else if (ngx_strncmp(s.data, "set",  3) == 0) {
                block_action = LIMIT_REQ2_BLOCK_ACTION_SET; /* set */
            } else if (ngx_strncmp(s.data, "clear", 5) == 0) {
                block_action = LIMIT_REQ2_BLOCK_ACTION_CLEAR; /* clear */
            } else  {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                     "limit_req2_block invalid action \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                                &ngx_http_limit_req2_module);
            if (shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "block_time=", 11) == 0) {
            s.len = value[i].len - 11;
            s.data = value[i].data + 11;

            block_time = ngx_atoi(s.data, s.len);
            if (block_time <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                     "limit_req2_block invalid block_time \"%V\"", &value[i]);
            }

            continue;
        }

        if (value[i].data[0] == '$') {

            value[i].len--;
            value[i].data++;

            v = ngx_array_push(variables);
            if (v == NULL) {
                return NGX_CONF_ERROR;
            }

            v->index = ngx_http_get_variable_index(cf, &value[i]);
            if (v->index == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            v->var = value[i];

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "limit_req2_block invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (variables->nelts == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "limit_req2_block no variable is defined \"%V\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "limit_req2_block no zone gived \"%V\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }


    lrcf->block_shm_zone = shm_zone;
    lrcf->block_action = block_action;
    lrcf->block_time = block_time;
    lrcf->block_limit_vars = variables;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_limit_req2_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_req2_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_req2_block_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_limit_req2_rate_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_limit_req2_ctx_t      *ctx;
    ngx_http_limit_req2_conf_t     *lrcf;
    ngx_http_limit_req2_t          *limit_req2;
    ngx_uint_t                      i, rate;
    ngx_buf_t                      *b;

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_req2_module);
    if (lrcf->rules == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (!lrcf->enable) {
        v->not_found = 1;
        return NGX_OK;
    }

    b = ngx_create_temp_buf(r->pool, 1024);
    if (b == NULL) {
        return NGX_ERROR;
    }

    limit_req2 = lrcf->rules->elts;
    for (i = 0; i < lrcf->rules->nelts; i++) {
        ctx = limit_req2[i].shm_zone->data;

        if (ctx == NULL) {
            return NGX_ERROR;
        }

        if (ctx->rate_seg > 0) {

            rate = (ctx->last_seg + ctx->curr_seg) * 1000 * 1000 /
                        (ctx->rate_seg + ctx->curr_seg_time_diff);

            b->last = ngx_sprintf(b->last, "COUNT=%ui;SEG=%ui;QPS=%ui.%03ui",
                        ctx->last_seg + ctx->curr_seg,
                        ctx->rate_seg + ctx->curr_seg_time_diff,
                        rate / 1000, rate % 1000);


            v->len = b->last - b->pos;

            v->data = ngx_pnalloc(r->pool, v->len);
            if (v->data == NULL) {
                return NGX_ERROR;
            }

            v->valid = 1;
            v->no_cacheable = 0;
            v->not_found = 0;

            ngx_memcpy(v->data, b->pos, v->len);

            return NGX_OK;
        }
    }

    v->not_found = 1;
    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_req2_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_limit_req2_rate, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_limit_req2_rate_variable;

    return NGX_OK;
}

