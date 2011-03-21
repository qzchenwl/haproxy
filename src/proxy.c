/*
 * Proxy variables and functions.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <common/defaults.h>
#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/errors.h>
#include <common/memory.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/client.h>
#include <proto/backend.h>
#include <proto/fd.h>
#include <proto/hdr_idx.h>
#include <proto/log.h>
#include <proto/protocols.h>
#include <proto/proto_tcp.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/checks.h>
#include <proto/server.h>
#include <proto/task.h>
#include <proto/queue.h>


extern struct proxy defproxy;
int listeners;	/* # of proxy listeners, set by cfgparse, unset by maintain_proxies */
struct proxy *proxy  = NULL;	/* list of all existing proxies */
struct eb_root used_proxy_id = EB_ROOT;	/* list of proxy IDs in use */

/*
 * This function returns a string containing a name describing capabilities to
 * report comprehensible error messages. Specifically, it will return the words
 * "frontend", "backend", "ruleset" when appropriate, or "proxy" for all other
 * cases including the proxies declared in "listen" mode.
 */
const char *proxy_cap_str(int cap)
{
	if ((cap & PR_CAP_LISTEN) != PR_CAP_LISTEN) {
		if (cap & PR_CAP_FE)
			return "frontend";
		else if (cap & PR_CAP_BE)
			return "backend";
		else if (cap & PR_CAP_RS)
			return "ruleset";
	}
	return "proxy";
}

/*
 * This function returns a string containing the mode of the proxy in a format
 * suitable for error messages.
 */
const char *proxy_mode_str(int mode) {

	if (mode == PR_MODE_TCP)
		return "tcp";
	else if (mode == PR_MODE_HTTP)
		return "http";
	else if (mode == PR_MODE_HEALTH)
		return "health";
	else
		return "unknown";
}

/*
 * This function scans the list of backends and servers to retrieve the first
 * backend and the first server with the given names, and sets them in both
 * parameters. It returns zero if either is not found, or non-zero and sets
 * the ones it did not found to NULL. If a NULL pointer is passed for the
 * backend, only the pointer to the server will be updated.
 */
int get_backend_server(const char *bk_name, const char *sv_name,
		       struct proxy **bk, struct server **sv)
{
	struct proxy *p;
	struct server *s;
	int pid, sid;

	*sv = NULL;

	pid = 0;
	if (*bk_name == '#')
		pid = atoi(bk_name + 1);
	sid = 0;
	if (*sv_name == '#')
		sid = atoi(sv_name + 1);

	for (p = proxy; p; p = p->next)
		if ((p->cap & PR_CAP_BE) &&
		    ((pid && p->uuid == pid) ||
		     (!pid && strcmp(p->id, bk_name) == 0)))
			break;
	if (bk)
		*bk = p;
	if (!p)
		return 0;

	for (s = p->srv; s; s = s->next)
		if ((sid && s->puid == sid) ||
		    (!sid && strcmp(s->id, sv_name) == 0))
			break;
	*sv = s;
	if (!s)
		return 0;
	return 1;
}

/* This function parses a "timeout" statement in a proxy section. It returns
 * -1 if there is any error, 1 for a warning, otherwise zero. If it does not
 * return zero, it may write an error message into the <err> buffer, for at
 * most <errlen> bytes, trailing zero included. The trailing '\n' must not
 * be written. The function must be called with <args> pointing to the first
 * command line word, with <proxy> pointing to the proxy being parsed, and
 * <defpx> to the default proxy or NULL. As a special case for compatibility
 * with older configs, it also accepts "{cli|srv|con}timeout" in args[0].
 */
static int proxy_parse_timeout(char **args, int section, struct proxy *proxy,
			       struct proxy *defpx, char *err, int errlen)
{
	unsigned timeout;
	int retval, cap;
	const char *res, *name;
	int *tv = NULL;
	int *td = NULL;

	retval = 0;

	/* simply skip "timeout" but remain compatible with old form */
	if (strcmp(args[0], "timeout") == 0)
		args++;

	name = args[0];
	if (!strcmp(args[0], "client") || !strcmp(args[0], "clitimeout")) {
		name = "client";
		tv = &proxy->timeout.client;
		td = &defpx->timeout.client;
		cap = PR_CAP_FE;
	} else if (!strcmp(args[0], "tarpit")) {
		tv = &proxy->timeout.tarpit;
		td = &defpx->timeout.tarpit;
		cap = PR_CAP_FE | PR_CAP_BE;
	} else if (!strcmp(args[0], "http-keep-alive")) {
		tv = &proxy->timeout.httpka;
		td = &defpx->timeout.httpka;
		cap = PR_CAP_FE | PR_CAP_BE;
	} else if (!strcmp(args[0], "http-request")) {
		tv = &proxy->timeout.httpreq;
		td = &defpx->timeout.httpreq;
		cap = PR_CAP_FE | PR_CAP_BE;
	} else if (!strcmp(args[0], "server") || !strcmp(args[0], "srvtimeout")) {
		name = "server";
		tv = &proxy->timeout.server;
		td = &defpx->timeout.server;
		cap = PR_CAP_BE;
	} else if (!strcmp(args[0], "connect") || !strcmp(args[0], "contimeout")) {
		name = "connect";
		tv = &proxy->timeout.connect;
		td = &defpx->timeout.connect;
		cap = PR_CAP_BE;
	} else if (!strcmp(args[0], "check")) {
		tv = &proxy->timeout.check;
		td = &defpx->timeout.check;
		cap = PR_CAP_BE;
	} else if (!strcmp(args[0], "queue")) {
		tv = &proxy->timeout.queue;
		td = &defpx->timeout.queue;
		cap = PR_CAP_BE;
	} else {
		snprintf(err, errlen,
			 "timeout '%s': must be 'client', 'server', 'connect', 'check', "
			 "'queue', 'http-keep-alive', 'http-request' or 'tarpit'",
			 args[0]);
		return -1;
	}

	if (*args[1] == 0) {
		snprintf(err, errlen, "%s timeout expects an integer value (in milliseconds)", name);
		return -1;
	}

	res = parse_time_err(args[1], &timeout, TIME_UNIT_MS);
	if (res) {
		snprintf(err, errlen, "unexpected character '%c' in %s timeout", *res, name);
		return -1;
	}

	if (!(proxy->cap & cap)) {
		snprintf(err, errlen, "%s timeout will be ignored because %s '%s' has no %s capability",
			 name, proxy_type_str(proxy), proxy->id,
			 (cap & PR_CAP_BE) ? "backend" : "frontend");
		retval = 1;
	}
	else if (defpx && *tv != *td) {
		snprintf(err, errlen, "overwriting %s timeout which was already specified", name);
		retval = 1;
	}

	*tv = MS_TO_TICKS(timeout);
	return retval;
}

/* This function parses a "rate-limit" statement in a proxy section. It returns
 * -1 if there is any error, 1 for a warning, otherwise zero. If it does not
 * return zero, it may write an error message into the <err> buffer, for at
 * most <errlen> bytes, trailing zero included. The trailing '\n' must not
 * be written. The function must be called with <args> pointing to the first
 * command line word, with <proxy> pointing to the proxy being parsed, and
 * <defpx> to the default proxy or NULL.
 */
static int proxy_parse_rate_limit(char **args, int section, struct proxy *proxy,
			          struct proxy *defpx, char *err, int errlen)
{
	int retval, cap;
	char *res, *name;
	unsigned int *tv = NULL;
	unsigned int *td = NULL;
	unsigned int val;

	retval = 0;

	/* simply skip "rate-limit" */
	if (strcmp(args[0], "rate-limit") == 0)
		args++;

	name = args[0];
	if (!strcmp(args[0], "sessions")) {
		name = "sessions";
		tv = &proxy->fe_sps_lim;
		td = &defpx->fe_sps_lim;
		cap = PR_CAP_FE;
	} else {
		snprintf(err, errlen,
			 "%s '%s': must be 'sessions'",
			 "rate-limit", args[0]);
		return -1;
	}

	if (*args[1] == 0) {
		snprintf(err, errlen, "%s %s expects expects an integer value (in sessions/second)", "rate-limit", name);
		return -1;
	}

	val = strtoul(args[1], &res, 0);
	if (*res) {
		snprintf(err, errlen, "%s %s: unexpected character '%c' in integer value '%s'", "rate-limit", name, *res, args[1]);
		return -1;
	}

	if (!(proxy->cap & cap)) {
		snprintf(err, errlen, "%s %s will be ignored because %s '%s' has no %s capability",
			 "rate-limit", name, proxy_type_str(proxy), proxy->id,
			 (cap & PR_CAP_BE) ? "backend" : "frontend");
		retval = 1;
	}
	else if (defpx && *tv != *td) {
		snprintf(err, errlen, "overwriting %s %s which was already specified", "rate-limit", name);
		retval = 1;
	}

	*tv = val;
	return retval;
}

/*
 * This function finds a proxy with matching name, mode and with satisfying
 * capabilities. It also checks if there are more matching proxies with
 * requested name as this often leads into unexpected situations.
 */

struct proxy *findproxy_mode(const char *name, int mode, int cap) {

	struct proxy *curproxy, *target = NULL;

	for (curproxy = proxy; curproxy; curproxy = curproxy->next) {
		if ((curproxy->cap & cap)!=cap || strcmp(curproxy->id, name))
			continue;

		if (curproxy->mode != mode &&
		    !(curproxy->mode == PR_MODE_HTTP && mode == PR_MODE_TCP)) {
			Alert("Unable to use proxy '%s' with wrong mode, required: %s, has: %s.\n", 
				name, proxy_mode_str(mode), proxy_mode_str(curproxy->mode));
			Alert("You may want to use 'mode %s'.\n", proxy_mode_str(mode));
			return NULL;
		}

		if (!target) {
			target = curproxy;
			continue;
		}

		Alert("Refusing to use duplicated proxy '%s' with overlapping capabilities: %s/%s!\n",
			name, proxy_type_str(curproxy), proxy_type_str(target));

		return NULL;
	}

	return target;
}

struct proxy *findproxy(const char *name, int cap) {

	struct proxy *curproxy, *target = NULL;

	for (curproxy = proxy; curproxy; curproxy = curproxy->next) {
		if ((curproxy->cap & cap)!=cap || strcmp(curproxy->id, name))
			continue;

		if (!target) {
			target = curproxy;
			continue;
		}

		return NULL;
	}

	return target;
}

/*
 * This function finds a server with matching name within selected proxy.
 * It also checks if there are more matching servers with
 * requested name as this often leads into unexpected situations.
 */

struct server *findserver(const struct proxy *px, const char *name) {

	struct server *cursrv, *target = NULL;

	if (!px)
		return NULL;

	for (cursrv = px->srv; cursrv; cursrv = cursrv->next) {
		if (strcmp(cursrv->id, name))
			continue;

		if (!target) {
			target = cursrv;
			continue;
		}

		Alert("Refusing to use duplicated server '%s' fould in proxy: %s!\n",
			name, px->id);

		return NULL;
	}

	return target;
}

struct server *addserver(const char *pxid, const char *svid, const char *addr, const char *cookie)
{
    struct proxy *px;
    struct server *newsrv;
    struct sockaddr_in *sk;
    struct task *t;
    char *tmp;
    

    if ((px = findproxy(pxid, PR_CAP_BE)) == NULL) {
        Alert("add server %s failed. backend %s not found.\n", svid, pxid);
        return NULL;
    }
    if ((newsrv = findserver(px, svid))) {
        Alert("add server %s failed. a server named %s already exist.", svid, svid);
        return NULL;
    }
    if ((newsrv = calloc(1, sizeof(struct server))) == NULL) {
        Alert("add server %s failed. out of memory.\n", svid);
        return NULL;
    }
    
    tmp = strdup(addr);
    sk = str2sa(tmp);
    free(tmp);
    if (!sk) {
        Alert("add server %s failed. unkown host %s\n", svid, addr);
        free(newsrv);
        return NULL;
    }

    if ((newsrv->check_data = calloc(global.tune.chksize, sizeof(char))) == NULL) {
        Alert("add server %s failed. out of memory while allocating memory for check buffer\n", svid);
        free(newsrv);
        return NULL;
    }

    if ((t = task_new()) == NULL) {
        Alert("add server %s failed. create new task failed.\n", svid);
        free(newsrv->check_data);
        free(newsrv);
        return NULL;
    }

    newsrv->next = px->srv;
    px->srv = newsrv;

    newsrv->proxy = px;
    newsrv->conf.file = NULL;
    newsrv->conf.line = 0;
    LIST_INIT(&newsrv->pendconns);
    newsrv->state = SRV_MAINTAIN;
    newsrv->last_change = now.tv_sec;
    newsrv->id = strdup(svid);
    newsrv->addr = *sk;
    newsrv->addr.sin_port = sk->sin_port?sk->sin_port:htons(80);
    newsrv->cookie = strdup(cookie);
    newsrv->cklen = strlen(cookie);
    newsrv->check_port  = ntohs(newsrv->addr.sin_port);

    newsrv->inter       = px->defsrv.inter;
    newsrv->fastinter   = px->defsrv.fastinter;
    newsrv->downinter   = px->defsrv.downinter;
    newsrv->rise        = px->defsrv.rise;
    newsrv->fall        = px->defsrv.fall;
    newsrv->maxqueue    = px->defsrv.maxqueue;
    newsrv->minconn     = px->defsrv.minconn;
    newsrv->maxconn     = px->maxconn;
    newsrv->slowstart   = px->defsrv.slowstart;
    newsrv->onerror     = px->defsrv.onerror;
    newsrv->consecutive_errors_limit
        = px->defsrv.consecutive_errors_limit;
    newsrv->uweight = newsrv->iweight = px->defsrv.iweight;

    newsrv->curfd = -1;
    newsrv->health = newsrv->rise;
    newsrv->prev_eweight = newsrv->eweight = newsrv->uweight * BE_WEIGHT_SCALE;
    newsrv->prev_state = newsrv->state;

    newsrv->check_status = HCHK_STATUS_INI;
    newsrv->state |= SRV_CHECKED;
    newsrv->check = t;
    newsrv->check_start = now;

    t->process = process_chk;
    t->context = newsrv;
    t->expire = tick_add(now_ms, srv_getinter(newsrv));
    task_queue(t);

    set_server_up(newsrv);

    return newsrv;
}

int delserver(const char *pxid, const char *svid)
{
    struct proxy *px;
    struct server *oldsrv, *presrv;
    struct task *t;
    if ((px = findproxy(pxid, PR_CAP_BE)) == NULL) {
        Alert("del server %s failed. backend %s not found.\n", svid, pxid);
        return 1;
    }
    if ((oldsrv = findserver(px, svid)) == NULL) {
        Alert("del server %s failed. server not found.\n", svid);
        return 1;
    }
    t = oldsrv->check;

    oldsrv->state |= SRV_MAINTAIN;
    set_server_down(oldsrv);

    task_delete(t);
    task_free(t);

    if (px->srv == oldsrv)
        px->srv = oldsrv->next;
    else {
        presrv = px->srv;
        while (presrv->next != oldsrv)
            presrv = presrv->next;
        presrv->next = oldsrv->next;
    }
    if (oldsrv->cookie)
        free(oldsrv->cookie);
    if (oldsrv->id)
        free(oldsrv->id);
    if (oldsrv->check_data)
        free(oldsrv->check_data);
    free(oldsrv);
    return 0;
}

int addbackend(const char *id)
{
    int cap;
    unsigned int next_pxid = 1;
    const char *err;
    struct proxy *curproxy = NULL;
    cap = PR_CAP_BE | PR_CAP_RS;
    if (!*id) {
        Warning("backend needs and <id>\n");
        return 1;
    }
    err = invalid_char(id);
    if (err) {
        Warning("charater '%c' is not permitted in backend name '%s'.\n", *err, id);
        return 1;
    }
    for (curproxy = proxy; curproxy != NULL; curproxy = curproxy->next) {
			/*
			 * If there are two proxies with the same name only following
			 * combinations are allowed:
			 *
			 *			listen backend frontend ruleset
			 *	listen             -      -       -        -
			 *	backend            -      -       OK       -
			 *	frontend           -      OK      -        -
			 *	ruleset            -      -       -        -
			 */
        if (!strcmp(curproxy->id, id) && \
                (cap != (PR_CAP_FE|PR_CAP_RS) || curproxy->cap != (PR_CAP_BE|PR_CAP_RS)) &&
                (cap != (PR_CAP_BE|PR_CAP_RS) || curproxy->cap != (PR_CAP_FE|PR_CAP_RS))) {
            Warning("'%s' has the same name as another '%s'\n", id, proxy_type_str(curproxy));
            return 1;
        }
    }
    
    if ((curproxy = (struct proxy *)calloc(1, sizeof(struct proxy))) == NULL) {
        Warning("allocate proxy: out of memory\n");
        return 1;
    }

    init_new_proxy(curproxy);

    /* DEFAULT SETTINGS */
	curproxy->mode = PR_MODE_TCP;
	curproxy->state = PR_STNEW;
	curproxy->maxconn = cfg_maxpconn;
	curproxy->conn_retries = CONN_RETRIES;
	curproxy->logfac1 = curproxy->logfac2 = -1; /* log disabled */

	curproxy->defsrv.inter = DEF_CHKINTR;
	curproxy->defsrv.fastinter = 0;
	curproxy->defsrv.downinter = 0;
	curproxy->defsrv.rise = DEF_RISETIME;
	curproxy->defsrv.fall = DEF_FALLTIME;
	curproxy->defsrv.check_port = 0;
	curproxy->defsrv.maxqueue = 0;
	curproxy->defsrv.minconn = 0;
	curproxy->defsrv.maxconn = 0;
	curproxy->defsrv.slowstart = 0;
	curproxy->defsrv.onerror = DEF_HANA_ONERR;
	curproxy->defsrv.consecutive_errors_limit = DEF_HANA_ERRLIMIT;
	curproxy->defsrv.uweight = curproxy->defsrv.iweight = 1;
    /* END DEFAULT SETTINGS */

    curproxy->next = proxy;
    proxy = curproxy;
    curproxy->conf.file = NULL;
    curproxy->conf.line = 0;
    curproxy->last_change = now.tv_sec;
    curproxy->id = strdup(id);
    curproxy->cap = cap;
    curproxy->defsrv.id = "default-server";

    /*
    // set default values
    memcpy(&curproxy->defsrv, &defproxy->defsrv, sizeof(curproxy->defsrv));
    
    curproxy->state = defproxy.state;
    curproxy->options = defproxy.options;
    curproxy->no_options = defproxy.no_options;
    curproxy->no_options2 = defproxy.no_options2;
    curproxy->bind_proc = defproxy.bind_proc;
    curproxy->lbprm.algo = defproxy.lbprm.algo;
    curproxy->except_net = defproxy.except_net;
    curproxy->except_mask = defproxy.except_mask;
    curproxy->except_to = defproxy.except_to;
    curproxy->except_mask_to = defproxy.except_mask_to;

    if (defproxy.fwdfor_hdr_len) {
        curproxy->fwdfor_hdr_len = defproxy.fwdfor_hdr_len;
        curproxy->fwdfor_hdr_name = defproxy.fwdfor_hdr_name;
    }

    curproxy->fullconn = defproxy.fullconn;
    curproxy->conn_retries = defproxy.conn_retries;
    
    if (defproxy.check_req) {
        curproxy->check_req = calloc(1, defproxy.check_len);
        memcpy(curproxy->check_req, defproxy.check_req, defproxy.check_len);
    }
    curproxy->check_len = defproxy.check_len;

    if (defproxy.cookie_name)
        curproxy->cookie_name = strdup(defproxy.cookie_name);
    curproxy->cookie_len = defproxy.cookie_len;
    if (defproxy.cookie_domain)
        curproxy->cookie_domain = strdup(defproxy.cookie_domain);

    if (defproxy.cookie_maxidle)
        curproxy->cookie_maxidle = defproxy.cookie_maxidle;

    if (defproxy.cookie_maxlife)
        curproxy->cookie_maxlife = defproxy.cookie_maxlife;

    if (defproxy.rdp_cookie_name)
        curproxy->rdp_cookie_name = strdup(defproxy.rdp_cookie_name);
    curproxy->rdp_cookie_len = defproxy.rdp_cookie_len;

    if (defproxy.url_param_name)
        curproxy->url_param_name = strdup(defproxy.url_param_name);
    curproxy->url_param_len = defproxy.url_param_len;

    if (defproxy.hh_name)
        curproxy->hh_name = strdup(defproxy.hh_name);
    curproxy->hh_len = defproxy.hh_len;
    curproxy->hh_match_domain = defproxy.hh_match_domain;

    if (defproxy.iface_name)
        curproxy->iface_name = strdup(defproxy.iface_name);
    curproxy->iface_len = defproxy.iface_len;

    curproxy->timeout.connect = defproxy.timeout.connect;
    curproxy->timeout.server = defproxy.timeout.server;
    curproxy->timeout.check = defproxy.timeout.check;
    curproxy->timeout.queue = defproxy.timeout.queue;
    curproxy->timeout.tarpit = defproxy.timeout.tarpit;
    curproxy->timeout.httpreq = defproxy.timeout.httpreq;
    curproxy->timeout.httpka = defproxy.timeout.httpka;
    curproxy->source_addr = defproxy.source_addr;

    //curproxy->mode = defproxy.mode;
    curproxy->logfac1 = defproxy.logfac1;
    curproxy->logsrv1 = defproxy.logsrv1;
    curproxy->loglev1 = defproxy.loglev1;
    curproxy->minlvl1 = defproxy.minlvl1;
    curproxy->logfac2 = defproxy.logfac2;
    curproxy->logsrv2 = defproxy.logsrv2;
    curproxy->loglev2 = defproxy.loglev2;
    curproxy->minlvl2 = defproxy.minlvl2;
    curproxy->grace = defproxy.grace;
    */

    curproxy->conf.used_listener_id = EB_ROOT;
    curproxy->conf.used_server_id = EB_ROOT;


    // mode http
    curproxy->mode = PR_MODE_HTTP;
    // cookie SERVERID insert indirect
    curproxy->options &= ~PR_O_COOK_ANY;
    curproxy->options2 &= ~PR_O2_COOK_PSV;
    curproxy->cookie_maxidle = curproxy->cookie_maxlife = 0;
    free(curproxy->cookie_domain); curproxy->cookie_domain = NULL;
    free(curproxy->cookie_name);
    curproxy->cookie_name = strdup("SERVERID"); // SERVERID
    curproxy->cookie_len = strlen(curproxy->cookie_name);
	curproxy->options |= PR_O_COOK_INS; // insert
    curproxy->options |= PR_O_COOK_IND; // indirect
    // balance roundrobin
    curproxy->lbprm.algo |= BE_LB_ALGO_RR;

    next_pxid = get_next_id(&used_proxy_id, next_pxid);
    curproxy->conf.id.key = curproxy->uuid = next_pxid;
    eb32_insert(&used_proxy_id, &curproxy->conf.id);

    curproxy->acl_requires |= ACL_USE_L7_ANY;

    if (curproxy->nb_req_cap)
        curproxy->req_cap_pool = create_pool("ptrcap",
                curproxy->nb_req_cap * sizeof(char *),
                MEM_F_SHARED);
    if (curproxy->nb_rsp_cap)
        curproxy->rsp_cap_pool = create_pool("ptrcap",
                curproxy->nb_rsp_cap * sizeof(char *),
                MEM_F_SHARED);
    curproxy->hdr_idx_pool = create_pool("hdr_idx",
            MAX_HTTP_HDR * sizeof(struct hdr_idx_elem),
            MEM_F_SHARED);
    if (!curproxy->fullconn)
        curproxy->fullconn = curproxy->maxconn;

    curproxy->lbprm.wmult = 1;
    curproxy->lbprm.wdiv = 1;
    curproxy->lbprm.algo &= ~(BE_LB_LKUP | BE_LB_PROP_DYN);
    switch (curproxy->lbprm.algo & BE_LB_KIND) {
        case BE_LB_KIND_RR:
            if ((curproxy->lbprm.algo & BE_LB_PARM) == BE_LB_RR_STATIC) {
                curproxy->lbprm.algo |= BE_LB_LKUP_MAP;
                init_server_map(curproxy);
            } else {
                curproxy->lbprm.algo |= BE_LB_LKUP_RRTREE | BE_LB_PROP_DYN;
                fwrr_init_server_groups(curproxy);
            }
            break;
        case BE_LB_KIND_LC:
            curproxy->lbprm.algo |= BE_LB_LKUP_LCTREE | BE_LB_PROP_DYN;
            fwlc_init_server_tree(curproxy);
            break;
        case BE_LB_KIND_HI:
            if ((curproxy->lbprm.algo &BE_LB_HASH_TYPE) == BE_LB_HASH_CONS) {
                curproxy->lbprm.algo |= BE_LB_LKUP_CHTREE | BE_LB_PROP_DYN;
                chash_init_server_tree(curproxy);
            } else {
                curproxy->lbprm.algo |= BE_LB_LKUP_MAP;
                init_server_map(curproxy);
            }
            break;
    }
    if (curproxy->mode == PR_MODE_HTTP) {
        curproxy->be_req_ana |= AN_REQ_WAIT_HTTP | AN_REQ_HTTP_INNER | AN_REQ_HTTP_PROCESS_BE;
        curproxy->be_rsp_ana |= AN_RES_WAIT_HTTP | AN_RES_HTTP_PROCESS_BE;
    }
    stktable_init(&curproxy->table);
    if (curproxy->options2 & PR_O2_RDPC_PRST)
        curproxy->be_req_ana |= AN_REQ_PRST_RDP_COOKIE;

    return 0;
}

int delbackend(struct proxy *px)
{
    struct proxy *curproxy;
    struct switching_rule *rule;


    for (curproxy = proxy; curproxy; curproxy = curproxy->next) {
        if (curproxy->defbe.be == px) {
            Warning("proxy '%s' has default proxy '%s'\n", curproxy->id, px->id);
            return 1;
        }

        list_for_each_entry(rule, &curproxy->switching_rules, list) {
            if (rule->be.backend == px) {
                Warning("proxy '%s' has use_backend '%s'\n", curproxy->id, px->id);
                return 1;
            }
        }
    }
    while (px->srv) {
        delserver(px->id, px->srv->id);
    }

    if (proxy == px)
        proxy = px->next;
    else {
        curproxy = proxy;
        while (curproxy->next != px)
            curproxy = curproxy->next;
        curproxy->next = px->next;
    }
    /* FIXME: free proxy memebers, id, lists...*/
    free(px);
    return 0;
}

int add_switch_entry(const char *frontend, const char *backend, const char *domain)
{
    struct proxy *fe, *be;
    if ((fe = findproxy(frontend, PR_CAP_FE)) == NULL) {
        Warning("cannot find frontend '%s'\n", frontend);
        return 1;
    }
    if ((be = findproxy(backend, PR_CAP_BE)) == NULL) {
        Warning("cannot find backend '%s'\n", backend);
        return 1;
    }
    hashtbl_insert(fe->switching_hashtbl, domain, be);
    return 0;
}

/* This function checks that the designated proxy has no http directives
 * enabled. It will output a warning if there are, and will fix some of them.
 * It returns the number of fatal errors encountered. This should be called
 * at the end of the configuration parsing if the proxy is not in http mode.
 * The <file> argument is used to construct the error message.
 */
int proxy_cfg_ensure_no_http(struct proxy *curproxy)
{
	if (curproxy->cookie_name != NULL) {
		Warning("config : cookie will be ignored for %s '%s' (needs 'mode http').\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	if (curproxy->rsp_exp != NULL) {
		Warning("config : server regular expressions will be ignored for %s '%s' (needs 'mode http').\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	if (curproxy->req_exp != NULL) {
		Warning("config : client regular expressions will be ignored for %s '%s' (needs 'mode http').\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	if (curproxy->monitor_uri != NULL) {
		Warning("config : monitor-uri will be ignored for %s '%s' (needs 'mode http').\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	if (curproxy->lbprm.algo & BE_LB_NEED_HTTP) {
		curproxy->lbprm.algo &= ~BE_LB_ALGO;
		curproxy->lbprm.algo |= BE_LB_ALGO_RR;
		Warning("config : Layer 7 hash not possible for %s '%s' (needs 'mode http'). Falling back to round robin.\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	if (curproxy->to_log & (LW_REQ | LW_RESP)) {
		curproxy->to_log &= ~(LW_REQ | LW_RESP);
		Warning("config : 'option httplog' not usable with %s '%s' (needs 'mode http'). Falling back to 'option tcplog'.\n",
			proxy_type_str(curproxy), curproxy->id);
	}
	return 0;
}

/*
 * This function creates all proxy sockets. It should be done very early,
 * typically before privileges are dropped. The sockets will be registered
 * but not added to any fd_set, in order not to loose them across the fork().
 * The proxies also start in IDLE state, meaning that it will be
 * maintain_proxies that will finally complete their loading.
 *
 * Its return value is composed from ERR_NONE, ERR_RETRYABLE and ERR_FATAL.
 * Retryable errors will only be printed if <verbose> is not zero.
 */
int start_proxies(int verbose)
{
	struct proxy *curproxy;
	struct listener *listener;
	int lerr, err = ERR_NONE;
	int pxerr;
	char msg[100];

	for (curproxy = proxy; curproxy != NULL; curproxy = curproxy->next) {
		if (curproxy->state != PR_STNEW)
			continue; /* already initialized */

		pxerr = 0;
		for (listener = curproxy->listen; listener != NULL; listener = listener->next) {
			if (listener->state != LI_ASSIGNED)
				continue; /* already started */

			lerr = tcp_bind_listener(listener, msg, sizeof(msg));

			/* errors are reported if <verbose> is set or if they are fatal */
			if (verbose || (lerr & (ERR_FATAL | ERR_ABORT))) {
				if (lerr & ERR_ALERT)
					Alert("Starting %s %s: %s\n",
					      proxy_type_str(curproxy), curproxy->id, msg);
				else if (lerr & ERR_WARN)
					Warning("Starting %s %s: %s\n",
						proxy_type_str(curproxy), curproxy->id, msg);
			}

			err |= lerr;
			if (lerr & (ERR_ABORT | ERR_FATAL)) {
				pxerr |= 1;
				break;
			}
			else if (lerr & ERR_CODE) {
				pxerr |= 1;
				continue;
			}
		}

		if (!pxerr) {
			curproxy->state = PR_STIDLE;
			send_log(curproxy, LOG_NOTICE, "Proxy %s started.\n", curproxy->id);
		}

		if (err & ERR_ABORT)
			break;
	}

	return err;
}


/*
 * this function enables proxies when there are enough free sessions,
 * or stops them when the table is full. It is designed to be called from the
 * select_loop(). It adjusts the date of next expiration event during stop
 * time if appropriate.
 */
void maintain_proxies(int *next)
{
	struct proxy *p;
	struct listener *l;
	unsigned int wait;

	p = proxy;

	/* if there are enough free sessions, we'll activate proxies */
	if (actconn < global.maxconn) {
		for (; p; p = p->next) {
			/* check the various reasons we may find to block the frontend */
			if (p->feconn >= p->maxconn)
				goto do_block;

			if (p->fe_sps_lim &&
			    (wait = next_event_delay(&p->fe_sess_per_sec, p->fe_sps_lim, 1))) {
				/* we're blocking because a limit was reached on the number of
				 * requests/s on the frontend. We want to re-check ASAP, which
				 * means in 1 ms before estimated expiration date, because the
				 * timer will have settled down. Note that we may already be in
				 * IDLE state here.
				 */
				*next = tick_first(*next, tick_add(now_ms, wait));
				goto do_block;
			}

			/* OK we have no reason to block, so let's unblock if we were blocking */
			if (p->state == PR_STIDLE) {
				for (l = p->listen; l != NULL; l = l->next)
					enable_listener(l);
				p->state = PR_STRUN;
			}
			continue;

		do_block:
			if (p->state == PR_STRUN) {
				for (l = p->listen; l != NULL; l = l->next)
					disable_listener(l);
				p->state = PR_STIDLE;
			}
		}
	}
	else {  /* block all proxies */
		while (p) {
			if (p->state == PR_STRUN) {
				for (l = p->listen; l != NULL; l = l->next)
					disable_listener(l);
				p->state = PR_STIDLE;
			}
			p = p->next;
		}
	}

	if (stopping) {
		p = proxy;
		while (p) {
			if (p->state != PR_STSTOPPED) {
				int t;
				t = tick_remain(now_ms, p->stop_time);
				if (t == 0) {
					Warning("Proxy %s stopped (FE: %lld conns, BE: %lld conns).\n",
						p->id, p->counters.cum_feconn, p->counters.cum_beconn);
					send_log(p, LOG_WARNING, "Proxy %s stopped (FE: %lld conns, BE: %lld conns).\n",
						 p->id, p->counters.cum_feconn, p->counters.cum_beconn);
					stop_proxy(p);
					/* try to free more memory */
					pool_gc2();
				}
				else {
					*next = tick_first(*next, p->stop_time);
				}
			}
			p = p->next;
		}
	}
	return;
}


/*
 * this function disables health-check servers so that the process will quickly be ignored
 * by load balancers. Note that if a proxy was already in the PAUSED state, then its grace
 * time will not be used since it would already not listen anymore to the socket.
 */
void soft_stop(void)
{
	struct proxy *p;

	stopping = 1;
	p = proxy;
	tv_update_date(0,1); /* else, the old time before select will be used */
	while (p) {
		if (p->state != PR_STSTOPPED) {
			Warning("Stopping %s %s in %d ms.\n", proxy_cap_str(p->cap), p->id, p->grace);
			send_log(p, LOG_WARNING, "Stopping %s %s in %d ms.\n", proxy_cap_str(p->cap), p->id, p->grace);
			p->stop_time = tick_add(now_ms, p->grace);
		}
		p = p->next;
	}
}


/*
 * Linux unbinds the listen socket after a SHUT_RD, and ignores SHUT_WR.
 * Solaris refuses either shutdown().
 * OpenBSD ignores SHUT_RD but closes upon SHUT_WR and refuses to rebind.
 * So a common validation path involves SHUT_WR && listen && SHUT_RD.
 * If disabling at least one listener returns an error, then the proxy
 * state is set to PR_STERROR because we don't know how to resume from this.
 */
void pause_proxy(struct proxy *p)
{
	struct listener *l;
	for (l = p->listen; l != NULL; l = l->next) {
		if (shutdown(l->fd, SHUT_WR) == 0 &&
		    listen(l->fd, p->backlog ? p->backlog : p->maxconn) == 0 &&
		    shutdown(l->fd, SHUT_RD) == 0) {
			EV_FD_CLR(l->fd, DIR_RD);
			if (p->state != PR_STERROR)
				p->state = PR_STPAUSED;
		}
		else
			p->state = PR_STERROR;
	}
}


/*
 * This function completely stops a proxy and releases its listeners. It has
 * to be called when going down in order to release the ports so that another
 * process may bind to them. It must also be called on disabled proxies at the
 * end of start-up. When all listeners are closed, the proxy is set to the
 * PR_STSTOPPED state.
 */
void stop_proxy(struct proxy *p)
{
	struct listener *l;

	for (l = p->listen; l != NULL; l = l->next) {
		unbind_listener(l);
		if (l->state >= LI_ASSIGNED) {
			delete_listener(l);
			listeners--;
		}
	}
	p->state = PR_STSTOPPED;
}

/*
 * This function temporarily disables listening so that another new instance
 * can start listening. It is designed to be called upon reception of a
 * SIGTTOU, after which either a SIGUSR1 can be sent to completely stop
 * the proxy, or a SIGTTIN can be sent to listen again.
 */
void pause_proxies(void)
{
	int err;
	struct proxy *p;

	err = 0;
	p = proxy;
	tv_update_date(0,1); /* else, the old time before select will be used */
	while (p) {
		if (p->cap & PR_CAP_FE &&
		    p->state != PR_STERROR &&
		    p->state != PR_STSTOPPED &&
		    p->state != PR_STPAUSED) {
			Warning("Pausing %s %s.\n", proxy_cap_str(p->cap), p->id);
			send_log(p, LOG_WARNING, "Pausing %s %s.\n", proxy_cap_str(p->cap), p->id);
			pause_proxy(p);
			if (p->state != PR_STPAUSED) {
				err |= 1;
				Warning("%s %s failed to enter pause mode.\n", proxy_cap_str(p->cap), p->id);
				send_log(p, LOG_WARNING, "%s %s failed to enter pause mode.\n", proxy_cap_str(p->cap), p->id);
			}
		}
		p = p->next;
	}
	if (err) {
		Warning("Some proxies refused to pause, performing soft stop now.\n");
		send_log(p, LOG_WARNING, "Some proxies refused to pause, performing soft stop now.\n");
		soft_stop();
	}
}


/*
 * This function reactivates listening. This can be used after a call to
 * sig_pause(), for example when a new instance has failed starting up.
 * It is designed to be called upon reception of a SIGTTIN.
 */
void listen_proxies(void)
{
	struct proxy *p;
	struct listener *l;

	p = proxy;
	tv_update_date(0,1); /* else, the old time before select will be used */
	while (p) {
		if (p->state == PR_STPAUSED) {
			Warning("Enabling %s %s.\n", proxy_cap_str(p->cap), p->id);
			send_log(p, LOG_WARNING, "Enabling %s %s.\n", proxy_cap_str(p->cap), p->id);

			for (l = p->listen; l != NULL; l = l->next) {
				if (listen(l->fd, p->backlog ? p->backlog : p->maxconn) == 0) {
					if (actconn < global.maxconn && p->feconn < p->maxconn) {
						EV_FD_SET(l->fd, DIR_RD);
						p->state = PR_STRUN;
					}
					else
						p->state = PR_STIDLE;
				} else {
					int port;

					if (l->addr.ss_family == AF_INET6)
						port = ntohs(((struct sockaddr_in6 *)(&l->addr))->sin6_port);
					else
						port = ntohs(((struct sockaddr_in *)(&l->addr))->sin_port);

					Warning("Port %d busy while trying to enable %s %s.\n",
						port, proxy_cap_str(p->cap), p->id);
					send_log(p, LOG_WARNING, "Port %d busy while trying to enable %s %s.\n",
						 port, proxy_cap_str(p->cap), p->id);
					/* Another port might have been enabled. Let's stop everything. */
					pause_proxy(p);
					break;
				}
			}
		}
		p = p->next;
	}
}

/* Set current session's backend to <be>. Nothing is done if the
 * session already had a backend assigned, which is indicated by
 * s->flags & SN_BE_ASSIGNED.
 * All flags, stats and counters which need be updated are updated.
 * Returns 1 if done, 0 in case of internal error, eg: lack of resource.
 */
int session_set_backend(struct session *s, struct proxy *be)
{
	if (s->flags & SN_BE_ASSIGNED)
		return 1;
	s->be = be;
	be->beconn++;
	if (be->beconn > be->counters.beconn_max)
		be->counters.beconn_max = be->beconn;
	proxy_inc_be_ctr(be);

	/* assign new parameters to the session from the new backend */
	s->rep->rto = s->req->wto = be->timeout.server;
	s->req->cto = be->timeout.connect;
	s->conn_retries = be->conn_retries;
	s->si[1].flags &= ~SI_FL_INDEP_STR;
	if (be->options2 & PR_O2_INDEPSTR)
		s->si[1].flags |= SI_FL_INDEP_STR;

	if (be->options2 & PR_O2_RSPBUG_OK)
		s->txn.rsp.err_pos = -1; /* let buggy responses pass */
	s->flags |= SN_BE_ASSIGNED;

	/* If the target backend requires HTTP processing, we have to allocate
	 * a struct hdr_idx for it if we did not have one.
	 */
	if (unlikely(!s->txn.hdr_idx.v && (be->acl_requires & ACL_USE_L7_ANY))) {
		if ((s->txn.hdr_idx.v = pool_alloc2(s->fe->hdr_idx_pool)) == NULL)
			return 0; /* not enough memory */

		/* and now initialize the HTTP transaction state */
		http_init_txn(s);

		s->txn.hdr_idx.size = MAX_HTTP_HDR;
		hdr_idx_init(&s->txn.hdr_idx);
	}

	/* We want to enable the backend-specific analysers except those which
	 * were already run as part of the frontend/listener. Note that it would
	 * be more reliable to store the list of analysers that have been run,
	 * but what we do here is OK for now.
	 */
	s->req->analysers |= be->be_req_ana & ~(s->listener->analysers);

	return 1;
}

static struct cfg_kw_list cfg_kws = {{ },{
	{ CFG_LISTEN, "timeout", proxy_parse_timeout },
	{ CFG_LISTEN, "clitimeout", proxy_parse_timeout },
	{ CFG_LISTEN, "contimeout", proxy_parse_timeout },
	{ CFG_LISTEN, "srvtimeout", proxy_parse_timeout },
	{ CFG_LISTEN, "rate-limit", proxy_parse_rate_limit },
	{ 0, NULL, NULL },
}};

__attribute__((constructor))
static void __proxy_module_init(void)
{
	cfg_register_keywords(&cfg_kws);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
