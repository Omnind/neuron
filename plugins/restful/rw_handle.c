#include <pthread.h>
#include <stdlib.h>
#include <sys/queue.h>

#include "neu_log.h"
#include "neu_plugin.h"
#include "json/neu_json_fn.h"
#include "json/neu_json_rw.h"

#include "handle.h"
#include "http.h"

#include "rw_handle.h"

struct cmd_ctx {
    uint32_t event_id;
    nng_aio *aio;

    TAILQ_ENTRY(cmd_ctx) node;
};

TAILQ_HEAD(, cmd_ctx) read_cmd_ctxs;
TAILQ_HEAD(, cmd_ctx) write_cmd_ctxs;
static pthread_mutex_t read_ctx_mtx;
static pthread_mutex_t write_ctx_mtx;

static void            read_add_ctx(uint32_t event_id, nng_aio *aio);
static void            write_add_ctx(uint32_t event_id, nng_aio *aio);
static struct cmd_ctx *read_find_ctx(uint32_t event_id);
static struct cmd_ctx *write_find_ctx(uint32_t event_id);

void handle_rw_init()
{
    TAILQ_INIT(&read_cmd_ctxs);
    TAILQ_INIT(&write_cmd_ctxs);

    pthread_mutex_init(&read_ctx_mtx, NULL);
    pthread_mutex_init(&write_ctx_mtx, NULL);
}

void handle_rw_uninit()
{
    struct cmd_ctx *ctx = NULL;

    pthread_mutex_lock(&read_ctx_mtx);
    ctx = TAILQ_FIRST(&read_cmd_ctxs);
    while (ctx != NULL) {
        TAILQ_REMOVE(&read_cmd_ctxs, ctx, node);
        nng_aio_finish(ctx->aio, 0);
        free(ctx);
        ctx = TAILQ_FIRST(&read_cmd_ctxs);
    }
    pthread_mutex_unlock(&read_ctx_mtx);

    pthread_mutex_lock(&write_ctx_mtx);
    ctx = TAILQ_FIRST(&write_cmd_ctxs);
    while (ctx != NULL) {
        TAILQ_REMOVE(&write_cmd_ctxs, ctx, node);
        nng_aio_finish(ctx->aio, 0);
        free(ctx);
        ctx = TAILQ_FIRST(&write_cmd_ctxs);
    }
    pthread_mutex_unlock(&write_ctx_mtx);

    pthread_mutex_destroy(&read_ctx_mtx);
    pthread_mutex_destroy(&write_ctx_mtx);
}

static void read_add_ctx(uint32_t event_id, nng_aio *aio)
{
    struct cmd_ctx *ctx = calloc(1, sizeof(struct cmd_ctx));

    pthread_mutex_lock(&read_ctx_mtx);

    ctx->aio      = aio;
    ctx->event_id = event_id;

    TAILQ_INSERT_TAIL(&read_cmd_ctxs, ctx, node);

    pthread_mutex_unlock(&read_ctx_mtx);
}

static void write_add_ctx(uint32_t event_id, nng_aio *aio)
{
    struct cmd_ctx *ctx = calloc(1, sizeof(struct cmd_ctx));

    pthread_mutex_lock(&write_ctx_mtx);

    ctx->aio      = aio;
    ctx->event_id = event_id;

    TAILQ_INSERT_TAIL(&write_cmd_ctxs, ctx, node);

    pthread_mutex_unlock(&write_ctx_mtx);
}

static struct cmd_ctx *read_find_ctx(uint32_t event_id)
{
    struct cmd_ctx *ctx = NULL;
    struct cmd_ctx *ret = NULL;

    pthread_mutex_lock(&read_ctx_mtx);
    TAILQ_FOREACH(ctx, &read_cmd_ctxs, node)
    {
        if (ctx->event_id == event_id) {
            TAILQ_REMOVE(&read_cmd_ctxs, ctx, node);
            ret = ctx;
            break;
        }
    }

    pthread_mutex_unlock(&read_ctx_mtx);

    return ret;
}

static struct cmd_ctx *write_find_ctx(uint32_t event_id)
{
    struct cmd_ctx *ctx = NULL;
    struct cmd_ctx *ret = NULL;

    pthread_mutex_lock(&write_ctx_mtx);

    TAILQ_FOREACH(ctx, &write_cmd_ctxs, node)
    {
        if (ctx->event_id == event_id) {
            TAILQ_REMOVE(&write_cmd_ctxs, ctx, node);
            ret = ctx;
            break;
        }
    }

    pthread_mutex_unlock(&write_ctx_mtx);

    return ret;
}

void handle_read(nng_aio *aio)
{
    neu_plugin_t *plugin = neu_rest_get_plugin();

    REST_PROCESS_HTTP_REQUEST(
        aio, neu_parse_read_req_t, neu_parse_decode_read, {
            neu_taggrp_config_t *config = neu_system_find_group_config(
                plugin, req->node_id, req->group_config_name);
            uint32_t event_id = neu_plugin_get_event_id(plugin);
            read_add_ctx(event_id, aio);
            neu_plugin_send_read_cmd(plugin, event_id, req->node_id, config);
        })
}

void handle_write(nng_aio *aio)
{
    neu_plugin_t *plugin = neu_rest_get_plugin();

    REST_PROCESS_HTTP_REQUEST(
        aio, neu_parse_write_req_t, neu_parse_decode_write, {
            neu_taggrp_config_t *config = neu_system_find_group_config(
                plugin, req->node_id, req->group_config_name);
            neu_data_val_t *data     = neu_parse_write_req_to_val(req);
            uint32_t        event_id = neu_plugin_get_event_id(plugin);
            write_add_ctx(event_id, aio);
            neu_plugin_send_write_cmd(plugin, event_id, req->node_id, config,
                                      data);
        })
}

void handle_read_resp(void *cmd_resp)
{
    neu_request_t *          req        = (neu_request_t *) cmd_resp;
    struct cmd_ctx *         ctx        = read_find_ctx(req->req_id);
    neu_reqresp_read_resp_t *resp       = (neu_reqresp_read_resp_t *) req->buf;
    neu_fixed_array_t *      array      = NULL;
    neu_parse_read_res_t     api_res    = { 0 };
    char *                   result     = NULL;
    neu_int_val_t *          iv         = NULL;
    int32_t                  error_code = 0;

    log_info("read resp id: %d, ctx: %p", req->req_id, ctx);
    assert(ctx != NULL);

    neu_dvalue_get_ref_array(resp->data_val, &array);

    iv = (neu_int_val_t *) neu_fixed_array_get(array, 0);
    assert(iv->key == 0);
    assert(neu_dvalue_get_value_type(iv->val) == NEU_DTYPE_ERRORCODE);

    neu_dvalue_get_errorcode(iv->val, &error_code);
    if (error_code != 0) {
        http_bad_request(ctx->aio, "{\"error\": 1}");
        free(ctx);
        return;
    }

    api_res.n_tag = array->length - 1;
    api_res.tags  = calloc(api_res.n_tag, sizeof(neu_parse_read_res_tag_t));

    for (size_t i = 1; i < array->length; i++) {
        iv = (neu_int_val_t *) neu_fixed_array_get(array, i);

        api_res.tags[i - 1].tag_id = iv->key;

        switch (neu_dvalue_get_value_type(iv->val)) {
        case NEU_DTYPE_BIT: {
            uint8_t bit = 0;

            neu_dvalue_get_bit(iv->val, &bit);
            api_res.tags[i - 1].t             = NEU_JSON_BIT;
            api_res.tags[i - 1].value.val_bit = bit;
            break;
        }
        case NEU_DTYPE_UINT16: {
            uint16_t u16 = 0;

            neu_dvalue_get_uint16(iv->val, &u16);
            api_res.tags[i - 1].t             = NEU_JSON_INT;
            api_res.tags[i - 1].value.val_int = u16;
            break;
        }
        case NEU_DTYPE_INT16: {
            int16_t i16 = 0;

            neu_dvalue_get_int16(iv->val, &i16);
            api_res.tags[i - 1].t             = NEU_JSON_INT;
            api_res.tags[i - 1].value.val_int = i16;
            break;
        }
        case NEU_DTYPE_INT32: {
            int32_t i32 = 0;

            neu_dvalue_get_int32(iv->val, &i32);
            api_res.tags[i - 1].t             = NEU_JSON_INT;
            api_res.tags[i - 1].value.val_int = i32;
            break;
        }
        case NEU_DTYPE_UINT32: {
            uint32_t u32 = 0;

            neu_dvalue_get_uint32(iv->val, &u32);
            api_res.tags[i - 1].t             = NEU_JSON_INT;
            api_res.tags[i - 1].value.val_int = u32;
            break;
        }
        case NEU_DTYPE_FLOAT: {
            float f32 = 0;

            neu_dvalue_get_float(iv->val, &f32);
            api_res.tags[i - 1].t               = NEU_JSON_FLOAT;
            api_res.tags[i - 1].value.val_float = f32;
            break;
        }
        default:
            break;
        }
    }

    neu_json_encode_by_fn(&api_res, neu_parse_encode_read, &result);
    http_ok(ctx->aio, result);
    free(ctx);
    free(api_res.tags);
    free(result);
}

void handle_write_resp(void *cmd_resp)
{
    neu_request_t * req = (neu_request_t *) cmd_resp;
    struct cmd_ctx *ctx = write_find_ctx(req->req_id);

    log_info("write resp id: %d, ctx: %p", req->req_id, ctx);
    if (ctx != NULL) {
        http_ok(ctx->aio, "{\"status\": \"OK\"}");
        free(ctx);
    }
    assert(ctx != NULL);
}