/**
 *
 * Copyright (c) 2010, Zed A. Shaw and Mongrel2 Project Contributors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the Mongrel2 Project, Zed A. Shaw, nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config/config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "adt/tst.h"
#include "config/db.h"
#include "dir.h"
#include "dbg.h"
#include "mime.h"
#include "proxy.h"
#include "server.h"
#include "setting.h"

#define arity(N) check(cols == (N), "Wrong number of cols: expected %d got %d", cols, (N))
#define SQL(Q, ...) sqlite3_mprintf((Q), ##__VA_ARGS__)
#define SQL_FREE(Q) if(Q) sqlite3_free(Q)

static tst_t *LOADED = NULL;

typedef struct BackendValue {
    void *value;
    bstring key;
    BackendType type;
    int active;
} BackendValue;

static inline bstring cols_to_key(const char *type, int cols, char **data)
{
    bstring key = bfromcstr("");
    int i = 0;

    bformata(key,"%s:",type);

    for(i = 0; i < cols; i++) {
        bformata(key, "%s:", data[i]);
    }

    return key;
}

static inline BackendValue *find_by_type(const char *type, const char *id)
{
    bstring key = bformat("%s:%s:", type, id);
    void *result = tst_search_prefix(LOADED, bdata(key), blength(key));
    bdestroy(key);
    return result;
}


static inline int store_in_loaded(bstring key, void *value, BackendType type) {
    BackendValue *elem = calloc(sizeof(BackendValue), 1);
    check_mem(elem);

    elem->value = value;
    elem->key = key;
    elem->type = type;
    elem->active = 0;

    LOADED = tst_insert(LOADED, bdata(key), blength(key), elem);

    return 0;

error:
    return -1;
}


static int Config_load_handler_options_cb(void *param, int cols, char **data, char **names)
{
    arity(3);
    Handler *handler = (Handler *)param;

    if(data[1][0] == '1') {
        handler->raw = 1;
    } else if(data[1][0] == '0') {
        handler->raw = 0;
    } else {
        log_warn("Handler with id=%s has a weird raw_payload setting of %s, assuming you want raw. It should be 0 or 1.",
                data[0], data[1]);
        handler->raw = 1;
    }

    log_info("Using handler protocol: '%s'", data[2]);

    if(data[2] != NULL && data[2][0] == 't') {
        handler->protocol = HANDLER_PROTO_TNET;
    } else {
        handler->protocol = HANDLER_PROTO_JSON;
    }

    return 0;

error:
    return -1;
}


static int Config_load_handler_cb(void *param, int cols, char **data, char **names)
{
    int rc = 0;
    char *sql = NULL;
    bstring key = cols_to_key("handler", cols, data);
    arity(5);

    check_mem(key);
    debug("VALIDATING KEY for reload: %s", bdata(key));

    BackendValue *backend = tst_search(LOADED, bdata(key), blength(key));

    if(backend) {
        debug("Found original handler, keeping it running.");
        check(backend->type == BACKEND_HANDLER, "Didn't get a Handler type backend for key: %s", bdata(key));
        Handler *handler = backend->value;
        handler->running = 1;
    } else {
        debug("No handler found, making a new one for: %s", bdata(key));
        Handler *handler = Handler_create(data[1], data[2], data[3], data[4]);
        check(handler != NULL, "Loaded handler %s with send_spec=%s send_ident=%s recv_spec=%s recv_ident=%s", data[0], data[1], data[2], data[3], data[4]);

        log_info("Loaded handler %s with send_spec=%s send_ident=%s recv_spec=%s recv_ident=%s", data[0], data[1], data[2], data[3], data[4]);

        sql = sqlite3_mprintf("SELECT id, raw_payload, protocol FROM handler WHERE id=%q", data[0]);
        check_mem(sql);

        rc = DB_exec(sql, Config_load_handler_options_cb, handler);

        if(rc != 0) {
            log_warn("Couldn't get the Handler.raw_payload setting, you might need to rebuild your db.");
        }

        rc = store_in_loaded(key, handler, BACKEND_HANDLER);
        check(rc == 0, "Failed to store handler %s in backend store.", bdata(key));

        sqlite3_free(sql);
    }

    return 0;

error:
    if(key) bdestroy(key);
    if(sql) sqlite3_free(sql);
    return -1;
}

static int Config_load_handlers()
{
    const char *HANDLER_QUERY = "SELECT id, send_spec, send_ident, recv_spec, recv_ident FROM handler";

    int rc = DB_exec(HANDLER_QUERY, Config_load_handler_cb, NULL);
    check(rc == 0, "Failed to load handlers");

    return 0;

error:
    return -1;
}


static int Config_load_proxy_cb(void *param, int cols, char **data, char **names)
{
    bstring key = cols_to_key("proxy", cols, data);

    arity(3);

    BackendValue *backend = tst_search(LOADED, bdata(key), blength(key));

    if(backend) {
        Proxy *proxy = backend->value;
        proxy->running = 1;
    } else {
        Proxy *proxy = Proxy_create(bfromcstr(data[1]), atoi(data[2]));
        check(proxy != NULL, "Failed to create proxy %s with address=%s port=%s", data[0], data[1], data[2]);

        log_info("Loaded proxy %s with address=%s port=%s", data[0], data[1], data[2]);

        check(store_in_loaded(key, proxy, BACKEND_PROXY) == 0, "Failed to store proxy.");
    }

    return 0;

error:
    if(key) bdestroy(key);
    return -1;
}

static int Config_load_proxies()
{
    const char *PROXY_QUERY = "SELECT id, addr, port FROM proxy";

    int rc = DB_exec(PROXY_QUERY, Config_load_proxy_cb, NULL);
    check(rc == 0, "Failed to load proxies");

    return 0;

error:
    return -1;
}

static int Config_load_dir_cb(void *param, int cols, char **data, char **names)
{
    bstring key = cols_to_key("dir", cols, data);
    arity(5);

    BackendValue *backend = tst_search(LOADED, bdata(key), blength(key));

    if(backend) {
        Dir *dir = backend->value;
        dir->running = 1;
    } else {
        int cache_ttl = data[4] ? strtol(data[4], NULL, 10) : 0;
        Dir* dir = Dir_create(data[1], data[2], data[3], cache_ttl);
        check(dir != NULL, "Failed to create dir %s with base=%s index=%s def_ctype=%s cache_ttl=%d",
              data[0], data[1], data[2], data[3], cache_ttl);

        log_info("Loaded dir %s with base=%s index=%s def_ctype=%s cache_ttl=%d",
                 data[0], data[1], data[2], data[3], cache_ttl);

        check(store_in_loaded(key, dir, BACKEND_DIR) == 0, "Failed to store Dir in loaded.");
    }

    return 0;

error:
    if(key) bdestroy(key);
    return -1;
}

static int Config_load_dirs()
{
    const char *DIR_QUERY = "SELECT id, base, index_file, default_ctype, cache_ttl FROM directory";

    int rc = DB_exec(DIR_QUERY, Config_load_dir_cb, NULL);
    check(rc == 0, "Failed to load directories");

    return 0;

error:
    return -1;
}

static int Config_load_route_cb(void *param, int cols, char **data, char **names)
{
    arity(4);

    Host *host = (Host*)param;
    debug("ROUTE BEING LOADED into HOST %p: %s:%s for route %s:%s", host, data[3], data[2], data[0], data[1]);

    check(host, "Expected host as param");
    check(data[3] != NULL, "Route type is NULL but shouldn't be for route id=%s", data[0]);

    BackendValue *backend = find_by_type(data[3], data[2]);
    check(backend != NULL, "Failed to find %s:%s for route %s:%s", data[3], data[2], data[0], data[1]);

    backend->active = 1;  // now this backend is actually active
    Host_add_backend(host, data[1], strlen(data[1]), backend->type, backend->value);

    return 0;

error:
    return -1;
}

static int Config_load_host_cb(void *param, int cols, char **data, char **names)
{
    char *query = NULL;
    arity(4);

    Server *server = (Server*)param;
    check(server, "Expected server as param");

    Host *host = Host_create(data[1], data[2]);
    check(host != NULL, "Failed to create host %s with %s", data[0], data[1]);

    const char *ROUTE_QUERY = "SELECT route.id, route.path, route.target_id, route.target_type "
        "FROM route, host WHERE host_id=%s AND "
        "host.server_id=%s AND host.id = route.host_id";
    query = SQL(ROUTE_QUERY, data[0], data[3]);

    int rc = DB_exec(query, Config_load_route_cb, host);
    check(rc == 0, "Failed to load routes for host %s:%s", data[0], data[1]);

    log_info("Adding host %s:%s to server at pattern %s", data[0], data[1], data[2]);

    Server_add_host(server, bfromcstr(data[2]), host);

    if(biseq(host->name, server->default_hostname)) {
        check(server->default_host == NULL, "You have more than one host matching the default host: %s, the second one is: %s:%s:%s",
                bdata(server->default_hostname),
                data[0], data[1], data[2]);

        log_info("Setting default host to host %s:%s:%s", data[0], data[1], data[2]);
        Server_set_default_host(server, host);
    }

    SQL_FREE(query);
    return 0;

error:
    SQL_FREE(query);
    return -1;
}

static int Config_load_server_cb(void* param, int cols, char **data, char **names)
{
	Server **server = NULL;
    char *query = NULL;
    arity(9);

    server = (Server **)param;
    if(*server != NULL)
    {
        log_info("More than one server object matches given uuid, using last found");
        Server_destroy(*server);
    }

    *server = Server_create(
            data[1], // uuid
            data[2], // default_host
            data[3], // bind_addr
            data[4], // port
            data[5], // chroot
            data[6], // access_log
            data[7], // error_log
            data[8] // pid_file
            );
    check(*server, "Failed to create server %s:%s on port %s", data[0], data[2], data[4]);


    const char *HOST_QUERY = "SELECT id, name, matching, server_id FROM host WHERE server_id = %s";
    query = SQL(HOST_QUERY, data[0]);
    check(query, "Failed to craft query string");

    int rc = DB_exec(query, Config_load_host_cb, *server);
    check(rc == 0, "Failed to find hosts for server %s:%s on port %s", data[0], data[1], data[4]);

    log_info("Loaded server %s:%s on port %s with default host %s", data[0], data[1], data[4], data[2]);

    SQL_FREE(query);

    return 0;

error:
	if (server) Server_destroy(*server);
    SQL_FREE(query);
    return -1;

}

Server *Config_load_server(const char *uuid)
{
    int rc = 0;
    char *query = NULL;

    rc = Config_load_handlers();
    check(rc == 0, "You have an error in your handlers, aborting startup.");

    rc = Config_load_proxies();
    check(rc == 0, "You have an error in your proxies, aborting startup.");

    rc = Config_load_dirs();
    check(rc == 0, "You have an error in your directories, aborting startup.");

    const char *SERVER_QUERY = "SELECT id, uuid, default_host, bind_addr, port, chroot, access_log, error_log, pid_file FROM server WHERE uuid=%Q";
    query = SQL(SERVER_QUERY, uuid);

    Server *server = NULL;
    rc = DB_exec(query, Config_load_server_cb, &server);
    check(rc == 0, "Failed to select server with uuid %s", uuid);

    SQL_FREE(query);

    return server;

error:
    SQL_FREE(query);
    return NULL;
}

static int Config_load_mimetypes_cb(void *param, int cols, char **data, char **names)
{
    arity(3);

    int rc = MIME_add_type(data[1], data[2]);
    check(rc == 0, "Failed to create mimetype %s:%s from id %s", data[1], data[2], data[0]);

    return 0;

error:
    return -1;
}

int Config_load_mimetypes()
{
    const char *MIME_QUERY = "SELECT id, extension, mimetype FROM mimetype";

    int rc = DB_exec(MIME_QUERY, Config_load_mimetypes_cb, NULL);
    check(rc == 0, "Failed to load mimetypes");

    return 0;

error:
    return -1;
}

static int Config_load_settings_cb(void *param, int cols, char **data, char **names)
{
    arity(3);

    int rc = Setting_add(data[1], data[2]);
    check(rc == 0, "Failed to create setting %s:%s from id %s", data[1], data[2], data[0]);

    return 0;

error:
    return -1;
}

int Config_load_settings()
{
    const char *SETTINGS_QUERY = "SELECT id, key, value FROM setting";

    int rc = DB_exec(SETTINGS_QUERY, Config_load_settings_cb, NULL);
    check(rc == 0, "Failed to load settings");

    return 0;

error:
    return -1;
}

int Config_init_db(const char *path)
{
    return DB_init(path);
}

void Config_close_db()
{
    DB_close();
}



static void handlers_receive_start(void *value, void *data)
{
    BackendValue *backend = (BackendValue *)value;

    debug("SCANNING BACKEND: %s has active %d and type %d",
            bdata(backend->key), backend->active, backend->type);

    if(backend->type == BACKEND_HANDLER && backend->active) {
        debug("STARTING handler: %s", bdata(backend->key));
        Handler *handler = backend->value;

        if(!handler->running) {
            // TODO: need three states, running, suspended, not running
            debug("LOADING BACKEND %s", bdata(handler->send_spec));
            taskcreate(Handler_task, handler, HANDLER_STACK);
            handler->running = 1;
        }
    } else {
        debug("SKIPPING DISABLED HANDLER: %s", bdata(backend->key));
    }
}

void Config_start_handlers()
{
    debug("LOADING ALL HANDLERS...");
    tst_traverse(LOADED, handlers_receive_start, NULL);
}


static void shutdown_cb(void *value, void *data)
{
    BackendValue *backend = (BackendValue *)value;
    assert(backend->value != NULL && "Backend had a NULL value!");

    if(!backend->active) {
        // just skip ones that are no longer active
        return;
    }

    if(backend->type == BACKEND_HANDLER) {
        debug("Stopping handler: %s", bdata(backend->key));
        Handler *handler = backend->value;
        handler->running = 0;
    } else if(backend->type == BACKEND_PROXY) {
        debug("Stopping proxy: %s", bdata(backend->key));
        Proxy *proxy = backend->value;
        proxy->running = 0;
    } else if(backend->type == BACKEND_DIR) {
        debug("Stopping dir: %s", bdata(backend->key));
        Dir *dir = backend->value;
        dir->running = 0;
    } else {
        sentinel("Invalid backend type: %d", backend->type);
    }

    backend->active = 0; // now the backend is forced down

    return;

error:
    return;
}

void Config_stop_all()
{
    tst_traverse(LOADED, shutdown_cb, NULL);
}

