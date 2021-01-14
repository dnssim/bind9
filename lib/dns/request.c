/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

#include <inttypes.h>
#include <stdbool.h>

#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/task.h>
#include <isc/util.h>

#include <dns/acl.h>
#include <dns/compress.h>
#include <dns/dispatch.h>
#include <dns/events.h>
#include <dns/log.h>
#include <dns/message.h>
#include <dns/rdata.h>
#include <dns/rdatastruct.h>
#include <dns/request.h>
#include <dns/result.h>
#include <dns/tsig.h>

#define REQUESTMGR_MAGIC      ISC_MAGIC('R', 'q', 'u', 'M')
#define VALID_REQUESTMGR(mgr) ISC_MAGIC_VALID(mgr, REQUESTMGR_MAGIC)

#define REQUEST_MAGIC	       ISC_MAGIC('R', 'q', 'u', '!')
#define VALID_REQUEST(request) ISC_MAGIC_VALID(request, REQUEST_MAGIC)

typedef ISC_LIST(dns_request_t) dns_requestlist_t;

#define DNS_REQUEST_NLOCKS 7

struct dns_requestmgr {
	unsigned int magic;
	isc_mutex_t lock;
	isc_mem_t *mctx;

	/* locked */
	int32_t eref;
	int32_t iref;
	isc_taskmgr_t *taskmgr;
	dns_dispatchmgr_t *dispatchmgr;
	dns_dispatch_t *dispatchv4;
	dns_dispatch_t *dispatchv6;
	bool exiting;
	isc_eventlist_t whenshutdown;
	unsigned int hash;
	isc_mutex_t locks[DNS_REQUEST_NLOCKS];
	dns_requestlist_t requests;
};

struct dns_request {
	unsigned int magic;
	unsigned int hash;
	isc_mem_t *mctx;
	int32_t flags;
	ISC_LINK(dns_request_t) link;
	isc_buffer_t *query;
	isc_buffer_t *answer;
	dns_requestevent_t *event;
	dns_dispatch_t *dispatch;
	dns_dispentry_t *dispentry;
	dns_requestmgr_t *requestmgr;
	isc_buffer_t *tsig;
	dns_tsigkey_t *tsigkey;
	isc_socketevent_t sendevent;
	isc_event_t ctlevent;
	bool canceling; /* ctlevent outstanding */
	isc_sockaddr_t destaddr;
	unsigned int udpcount;
	isc_dscp_t dscp;
};

#define DNS_REQUEST_F_CONNECTING 0x0001
#define DNS_REQUEST_F_SENDING	 0x0002
#define DNS_REQUEST_F_CANCELED                                                \
	0x0004				 /*%< ctlevent received, or otherwise \
					  * synchronously canceled */
#define DNS_REQUEST_F_TIMEDOUT	  0x0008 /*%< canceled due to a timeout */
#define DNS_REQUEST_F_TCP	  0x0010 /*%< This request used TCP */
#define DNS_REQUEST_CANCELED(r)	  (((r)->flags & DNS_REQUEST_F_CANCELED) != 0)
#define DNS_REQUEST_CONNECTING(r) (((r)->flags & DNS_REQUEST_F_CONNECTING) != 0)
#define DNS_REQUEST_SENDING(r)	  (((r)->flags & DNS_REQUEST_F_SENDING) != 0)
#define DNS_REQUEST_TIMEDOUT(r)	  (((r)->flags & DNS_REQUEST_F_TIMEDOUT) != 0)

/***
 *** Forward
 ***/

static void
mgr_destroy(dns_requestmgr_t *requestmgr);
static void
mgr_shutdown(dns_requestmgr_t *requestmgr);
static unsigned int
mgr_gethash(dns_requestmgr_t *requestmgr);
static void
send_shutdown_events(dns_requestmgr_t *requestmgr);

static isc_result_t
req_render(dns_message_t *message, isc_buffer_t **buffer, unsigned int options,
	   isc_mem_t *mctx);
static void
req_response(isc_task_t *task, isc_event_t *event);
static void
req_timeout(isc_task_t *task, isc_event_t *event);
static void
req_senddone(isc_nmhandle_t *handle, isc_result_t eresult, void *arg);
static void
req_sendevent(dns_request_t *request, isc_result_t result);
static void
req_connected(isc_nmhandle_t *handle, isc_result_t eresult, void *arg);
static void
req_cancel(dns_request_t *request);
static void
req_destroy(dns_request_t *request);
static void
req_log(int level, const char *fmt, ...) ISC_FORMAT_PRINTF(2, 3);
static void
do_cancel(isc_task_t *task, isc_event_t *event);

/***
 *** Public
 ***/

isc_result_t
dns_requestmgr_create(isc_mem_t *mctx, isc_taskmgr_t *taskmgr,
		      dns_dispatchmgr_t *dispatchmgr,
		      dns_dispatch_t *dispatchv4, dns_dispatch_t *dispatchv6,
		      dns_requestmgr_t **requestmgrp) {
	dns_requestmgr_t *requestmgr;
	int i;
	unsigned int dispattr;

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_create");

	REQUIRE(requestmgrp != NULL && *requestmgrp == NULL);
	REQUIRE(taskmgr != NULL);
	REQUIRE(dispatchmgr != NULL);

	if (dispatchv4 != NULL) {
		dispattr = dns_dispatch_getattributes(dispatchv4);
		REQUIRE((dispattr & DNS_DISPATCHATTR_UDP) != 0);
	}
	if (dispatchv6 != NULL) {
		dispattr = dns_dispatch_getattributes(dispatchv6);
		REQUIRE((dispattr & DNS_DISPATCHATTR_UDP) != 0);
	}

	requestmgr = isc_mem_get(mctx, sizeof(*requestmgr));

	isc_mutex_init(&requestmgr->lock);

	for (i = 0; i < DNS_REQUEST_NLOCKS; i++) {
		isc_mutex_init(&requestmgr->locks[i]);
	}
	requestmgr->taskmgr = taskmgr;
	requestmgr->dispatchmgr = dispatchmgr;
	requestmgr->dispatchv4 = NULL;
	if (dispatchv4 != NULL) {
		dns_dispatch_attach(dispatchv4, &requestmgr->dispatchv4);
	}
	requestmgr->dispatchv6 = NULL;
	if (dispatchv6 != NULL) {
		dns_dispatch_attach(dispatchv6, &requestmgr->dispatchv6);
	}
	requestmgr->mctx = NULL;
	isc_mem_attach(mctx, &requestmgr->mctx);
	requestmgr->eref = 1; /* implicit attach */
	requestmgr->iref = 0;
	ISC_LIST_INIT(requestmgr->whenshutdown);
	ISC_LIST_INIT(requestmgr->requests);
	requestmgr->exiting = false;
	requestmgr->hash = 0;
	requestmgr->magic = REQUESTMGR_MAGIC;

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_create: %p", requestmgr);

	*requestmgrp = requestmgr;
	return (ISC_R_SUCCESS);
}

void
dns_requestmgr_whenshutdown(dns_requestmgr_t *requestmgr, isc_task_t *task,
			    isc_event_t **eventp) {
	isc_task_t *tclone;
	isc_event_t *event;

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_whenshutdown");

	REQUIRE(VALID_REQUESTMGR(requestmgr));
	REQUIRE(eventp != NULL);

	event = *eventp;
	*eventp = NULL;

	LOCK(&requestmgr->lock);

	if (requestmgr->exiting) {
		/*
		 * We're already shutdown.  Send the event.
		 */
		event->ev_sender = requestmgr;
		isc_task_send(task, &event);
	} else {
		tclone = NULL;
		isc_task_attach(task, &tclone);
		event->ev_sender = tclone;
		ISC_LIST_APPEND(requestmgr->whenshutdown, event, ev_link);
	}
	UNLOCK(&requestmgr->lock);
}

void
dns_requestmgr_shutdown(dns_requestmgr_t *requestmgr) {
	REQUIRE(VALID_REQUESTMGR(requestmgr));

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_shutdown: %p", requestmgr);

	LOCK(&requestmgr->lock);
	mgr_shutdown(requestmgr);
	UNLOCK(&requestmgr->lock);
}

static void
mgr_shutdown(dns_requestmgr_t *requestmgr) {
	dns_request_t *request;

	/*
	 * Caller holds lock.
	 */
	if (!requestmgr->exiting) {
		requestmgr->exiting = true;
		for (request = ISC_LIST_HEAD(requestmgr->requests);
		     request != NULL; request = ISC_LIST_NEXT(request, link))
		{
			dns_request_cancel(request);
		}
		if (requestmgr->iref == 0) {
			INSIST(ISC_LIST_EMPTY(requestmgr->requests));
			send_shutdown_events(requestmgr);
		}
	}
}

static void
requestmgr_attach(dns_requestmgr_t *source, dns_requestmgr_t **targetp) {
	/*
	 * Locked by caller.
	 */

	REQUIRE(VALID_REQUESTMGR(source));
	REQUIRE(targetp != NULL && *targetp == NULL);

	REQUIRE(!source->exiting);

	source->iref++;
	*targetp = source;

	req_log(ISC_LOG_DEBUG(3), "requestmgr_attach: %p: eref %d iref %d",
		source, source->eref, source->iref);
}

static void
requestmgr_detach(dns_requestmgr_t **requestmgrp) {
	dns_requestmgr_t *requestmgr;
	bool need_destroy = false;

	REQUIRE(requestmgrp != NULL);
	requestmgr = *requestmgrp;
	*requestmgrp = NULL;
	REQUIRE(VALID_REQUESTMGR(requestmgr));

	LOCK(&requestmgr->lock);
	INSIST(requestmgr->iref > 0);
	requestmgr->iref--;

	req_log(ISC_LOG_DEBUG(3), "requestmgr_detach: %p: eref %d iref %d",
		requestmgr, requestmgr->eref, requestmgr->iref);

	if (requestmgr->iref == 0 && requestmgr->exiting) {
		INSIST(ISC_LIST_HEAD(requestmgr->requests) == NULL);
		send_shutdown_events(requestmgr);
		if (requestmgr->eref == 0) {
			need_destroy = true;
		}
	}
	UNLOCK(&requestmgr->lock);

	if (need_destroy) {
		mgr_destroy(requestmgr);
	}
}

void
dns_requestmgr_attach(dns_requestmgr_t *source, dns_requestmgr_t **targetp) {
	REQUIRE(VALID_REQUESTMGR(source));
	REQUIRE(targetp != NULL && *targetp == NULL);
	REQUIRE(!source->exiting);

	LOCK(&source->lock);
	source->eref++;
	*targetp = source;
	UNLOCK(&source->lock);

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_attach: %p: eref %d iref %d",
		source, source->eref, source->iref);
}

void
dns_requestmgr_detach(dns_requestmgr_t **requestmgrp) {
	dns_requestmgr_t *requestmgr;
	bool need_destroy = false;

	REQUIRE(requestmgrp != NULL);
	requestmgr = *requestmgrp;
	*requestmgrp = NULL;
	REQUIRE(VALID_REQUESTMGR(requestmgr));

	LOCK(&requestmgr->lock);
	INSIST(requestmgr->eref > 0);
	requestmgr->eref--;

	req_log(ISC_LOG_DEBUG(3), "dns_requestmgr_detach: %p: eref %d iref %d",
		requestmgr, requestmgr->eref, requestmgr->iref);

	if (requestmgr->eref == 0 && requestmgr->iref == 0) {
		INSIST(requestmgr->exiting &&
		       ISC_LIST_HEAD(requestmgr->requests) == NULL);
		need_destroy = true;
	}
	UNLOCK(&requestmgr->lock);

	if (need_destroy) {
		mgr_destroy(requestmgr);
	}
}

static void
send_shutdown_events(dns_requestmgr_t *requestmgr) {
	isc_event_t *event, *next_event;
	isc_task_t *etask;

	req_log(ISC_LOG_DEBUG(3), "send_shutdown_events: %p", requestmgr);

	/*
	 * Caller must be holding the manager lock.
	 */
	for (event = ISC_LIST_HEAD(requestmgr->whenshutdown); event != NULL;
	     event = next_event)
	{
		next_event = ISC_LIST_NEXT(event, ev_link);
		ISC_LIST_UNLINK(requestmgr->whenshutdown, event, ev_link);
		etask = event->ev_sender;
		event->ev_sender = requestmgr;
		isc_task_sendanddetach(&etask, &event);
	}
}

static void
mgr_destroy(dns_requestmgr_t *requestmgr) {
	int i;

	req_log(ISC_LOG_DEBUG(3), "mgr_destroy");

	REQUIRE(requestmgr->eref == 0);
	REQUIRE(requestmgr->iref == 0);

	isc_mutex_destroy(&requestmgr->lock);
	for (i = 0; i < DNS_REQUEST_NLOCKS; i++) {
		isc_mutex_destroy(&requestmgr->locks[i]);
	}
	if (requestmgr->dispatchv4 != NULL) {
		dns_dispatch_detach(&requestmgr->dispatchv4);
	}
	if (requestmgr->dispatchv6 != NULL) {
		dns_dispatch_detach(&requestmgr->dispatchv6);
	}
	requestmgr->magic = 0;
	isc_mem_putanddetach(&requestmgr->mctx, requestmgr,
			     sizeof(*requestmgr));
}

static unsigned int
mgr_gethash(dns_requestmgr_t *requestmgr) {
	req_log(ISC_LOG_DEBUG(3), "mgr_gethash");
	/*
	 * Locked by caller.
	 */
	requestmgr->hash++;
	return (requestmgr->hash % DNS_REQUEST_NLOCKS);
}

static inline void
req_send(dns_request_t *request) {
	isc_region_t r;

	req_log(ISC_LOG_DEBUG(3), "req_send: request %p", request);

	REQUIRE(VALID_REQUEST(request));

	isc_buffer_usedregion(request->query, &r);

	request->flags |= DNS_REQUEST_F_SENDING;
	dns_dispatch_send(request->dispentry, &r, request->dscp);
}

static isc_result_t
new_request(isc_mem_t *mctx, dns_request_t **requestp) {
	dns_request_t *request;

	request = isc_mem_get(mctx, sizeof(*request));
	*request = (dns_request_t){ .dscp = -1 };
	ISC_LINK_INIT(request, link);
	ISC_EVENT_INIT(&request->ctlevent, sizeof(request->ctlevent), 0, NULL,
		       DNS_EVENT_REQUESTCONTROL, do_cancel, request, NULL, NULL,
		       NULL);

	isc_mem_attach(mctx, &request->mctx);

	request->magic = REQUEST_MAGIC;
	*requestp = request;
	return (ISC_R_SUCCESS);
}

static bool
isblackholed(dns_dispatchmgr_t *dispatchmgr, const isc_sockaddr_t *destaddr) {
	dns_acl_t *blackhole;
	isc_netaddr_t netaddr;
	int match;
	bool drop = false;
	char netaddrstr[ISC_NETADDR_FORMATSIZE];

	blackhole = dns_dispatchmgr_getblackhole(dispatchmgr);
	if (blackhole != NULL) {
		isc_netaddr_fromsockaddr(&netaddr, destaddr);
		if (dns_acl_match(&netaddr, NULL, blackhole, NULL, &match,
				  NULL) == ISC_R_SUCCESS &&
		    match > 0)
		{
			drop = true;
		}
	}
	if (drop) {
		isc_netaddr_format(&netaddr, netaddrstr, sizeof(netaddrstr));
		req_log(ISC_LOG_DEBUG(10), "blackholed address %s", netaddrstr);
	}
	return (drop);
}

static isc_result_t
tcp_dispatch(bool newtcp, dns_requestmgr_t *requestmgr,
	     const isc_sockaddr_t *srcaddr, const isc_sockaddr_t *destaddr,
	     isc_dscp_t dscp, bool *connected, dns_dispatch_t **dispatchp) {
	isc_result_t result;

	if (!newtcp) {
		result = dns_dispatch_gettcp(requestmgr->dispatchmgr, destaddr,
					     srcaddr, connected, dispatchp);
		if (result == ISC_R_SUCCESS) {
			char peer[ISC_SOCKADDR_FORMATSIZE];

			isc_sockaddr_format(destaddr, peer, sizeof(peer));
			req_log(ISC_LOG_DEBUG(1),
				"attached to %s TCP "
				"connection to %s",
				*connected ? "existing" : "pending", peer);
			return (result);
		}
	}

	result = dns_dispatch_createtcp(requestmgr->dispatchmgr,
					requestmgr->taskmgr, srcaddr, destaddr,
					0, dscp, dispatchp);
	return (result);
}

static isc_result_t
udp_dispatch(dns_requestmgr_t *requestmgr, const isc_sockaddr_t *srcaddr,
	     const isc_sockaddr_t *destaddr, dns_dispatch_t **dispatchp) {
	dns_dispatch_t *disp = NULL;

	if (srcaddr == NULL) {
		switch (isc_sockaddr_pf(destaddr)) {
		case PF_INET:
			disp = requestmgr->dispatchv4;
			break;

		case PF_INET6:
			disp = requestmgr->dispatchv6;
			break;

		default:
			return (ISC_R_NOTIMPLEMENTED);
		}
		if (disp == NULL) {
			return (ISC_R_FAMILYNOSUPPORT);
		}
		dns_dispatch_attach(disp, dispatchp);
		return (ISC_R_SUCCESS);
	}

	return (dns_dispatch_createudp(requestmgr->dispatchmgr,
				       requestmgr->taskmgr, srcaddr, 0,
				       dispatchp));
}

static isc_result_t
get_dispatch(bool tcp, bool newtcp, dns_requestmgr_t *requestmgr,
	     const isc_sockaddr_t *srcaddr, const isc_sockaddr_t *destaddr,
	     isc_dscp_t dscp, bool *connected, dns_dispatch_t **dispatchp) {
	isc_result_t result;

	if (tcp) {
		result = tcp_dispatch(newtcp, requestmgr, srcaddr, destaddr,
				      dscp, connected, dispatchp);
	} else {
		result = udp_dispatch(requestmgr, srcaddr, destaddr, dispatchp);
	}
	return (result);
}

isc_result_t
dns_request_createraw(dns_requestmgr_t *requestmgr, isc_buffer_t *msgbuf,
		      const isc_sockaddr_t *srcaddr,
		      const isc_sockaddr_t *destaddr, isc_dscp_t dscp,
		      unsigned int options, unsigned int timeout,
		      unsigned int udptimeout, unsigned int udpretries,
		      isc_task_t *task, isc_taskaction_t action, void *arg,
		      dns_request_t **requestp) {
	dns_request_t *request = NULL;
	isc_task_t *tclone = NULL;
	isc_result_t result;
	isc_mem_t *mctx;
	dns_messageid_t id;
	bool tcp = false;
	bool newtcp = false;
	isc_region_t r;
	bool connected = false;
	unsigned int dispopt = 0;

	REQUIRE(VALID_REQUESTMGR(requestmgr));
	REQUIRE(msgbuf != NULL);
	REQUIRE(destaddr != NULL);
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);
	REQUIRE(requestp != NULL && *requestp == NULL);
	REQUIRE(timeout > 0);
	if (srcaddr != NULL) {
		REQUIRE(isc_sockaddr_pf(srcaddr) == isc_sockaddr_pf(destaddr));
	}

	mctx = requestmgr->mctx;

	req_log(ISC_LOG_DEBUG(3), "dns_request_createraw");

	if (isblackholed(requestmgr->dispatchmgr, destaddr)) {
		return (DNS_R_BLACKHOLED);
	}

	request = NULL;
	result = new_request(mctx, &request);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}

	if (udptimeout == 0 && udpretries != 0) {
		udptimeout = timeout / (udpretries + 1);
		if (udptimeout == 0) {
			udptimeout = 1;
		}
	}

	timeout *= 1000;
	udptimeout *= 1000;

	request->udpcount = udpretries;
	request->dscp = dscp;

	request->event = (dns_requestevent_t *)isc_event_allocate(
		mctx, task, DNS_EVENT_REQUESTDONE, action, arg,
		sizeof(dns_requestevent_t));
	isc_task_attach(task, &tclone);
	request->event->ev_sender = task;
	request->event->request = request;
	request->event->result = ISC_R_FAILURE;

	isc_buffer_usedregion(msgbuf, &r);
	if (r.length < DNS_MESSAGE_HEADERLEN || r.length > 65535) {
		result = DNS_R_FORMERR;
		goto cleanup;
	}

	if ((options & DNS_REQUESTOPT_TCP) != 0 || r.length > 512) {
		tcp = true;
	}

again:
	result = get_dispatch(tcp, newtcp, requestmgr, srcaddr, destaddr, dscp,
			      &connected, &request->dispatch);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	if ((options & DNS_REQUESTOPT_FIXEDID) != 0) {
		id = (r.base[0] << 8) | r.base[1];
		dispopt |= DNS_DISPATCHOPT_FIXEDID;
	}

	result = dns_dispatch_addresponse(
		request->dispatch, dispopt, tcp ? timeout : udptimeout,
		destaddr, task, req_connected, req_senddone, req_response,
		req_timeout, request, &id, &request->dispentry);
	if (result != ISC_R_SUCCESS) {
		if ((options & DNS_REQUESTOPT_FIXEDID) != 0 && !newtcp) {
			newtcp = true;
			connected = false;
			dns_dispatch_detach(&request->dispatch);
			goto again;
		}
		goto cleanup;
	}

	isc_buffer_allocate(mctx, &request->query, r.length + (tcp ? 2 : 0));
	result = isc_buffer_copyregion(request->query, &r);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	/* Add message ID. */
	isc_buffer_usedregion(request->query, &r);
	r.base[0] = (id >> 8) & 0xff;
	r.base[1] = id & 0xff;

	LOCK(&requestmgr->lock);
	if (requestmgr->exiting) {
		UNLOCK(&requestmgr->lock);
		result = ISC_R_SHUTTINGDOWN;
		goto cleanup;
	}
	requestmgr_attach(requestmgr, &request->requestmgr);
	request->hash = mgr_gethash(requestmgr);
	ISC_LIST_APPEND(requestmgr->requests, request, link);
	UNLOCK(&requestmgr->lock);

	request->destaddr = *destaddr;
	if (!tcp || !connected) {
		result = dns_dispatch_connect(request->dispentry);
		if (result != ISC_R_SUCCESS) {
			goto unlink;
		}
		request->flags |= DNS_REQUEST_F_CONNECTING | DNS_REQUEST_F_TCP;
	} else {
		req_send(request);
	}

	req_log(ISC_LOG_DEBUG(3), "dns_request_createraw: request %p", request);
	*requestp = request;
	return (ISC_R_SUCCESS);

unlink:
	LOCK(&requestmgr->lock);
	ISC_LIST_UNLINK(requestmgr->requests, request, link);
	UNLOCK(&requestmgr->lock);

cleanup:
	if (tclone != NULL) {
		isc_task_detach(&tclone);
	}
	req_destroy(request);
	req_log(ISC_LOG_DEBUG(3), "dns_request_createraw: failed %s",
		dns_result_totext(result));
	return (result);
}

isc_result_t
dns_request_create(dns_requestmgr_t *requestmgr, dns_message_t *message,
		   const isc_sockaddr_t *address, unsigned int options,
		   dns_tsigkey_t *key, unsigned int timeout, isc_task_t *task,
		   isc_taskaction_t action, void *arg,
		   dns_request_t **requestp) {
	return (dns_request_createvia(requestmgr, message, NULL, address, -1,
				      options, key, timeout, 0, 0, task, action,
				      arg, requestp));
}

isc_result_t
dns_request_createvia(dns_requestmgr_t *requestmgr, dns_message_t *message,
		      const isc_sockaddr_t *srcaddr,
		      const isc_sockaddr_t *destaddr, isc_dscp_t dscp,
		      unsigned int options, dns_tsigkey_t *key,
		      unsigned int timeout, unsigned int udptimeout,
		      unsigned int udpretries, isc_task_t *task,
		      isc_taskaction_t action, void *arg,
		      dns_request_t **requestp) {
	dns_request_t *request = NULL;
	isc_task_t *tclone = NULL;
	isc_result_t result;
	isc_mem_t *mctx;
	dns_messageid_t id;
	bool tcp;
	bool settsigkey = true;
	bool connected = false;

	REQUIRE(VALID_REQUESTMGR(requestmgr));
	REQUIRE(message != NULL);
	REQUIRE(destaddr != NULL);
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);
	REQUIRE(requestp != NULL && *requestp == NULL);
	REQUIRE(timeout > 0);

	mctx = requestmgr->mctx;

	req_log(ISC_LOG_DEBUG(3), "dns_request_createvia");

	if (srcaddr != NULL &&
	    isc_sockaddr_pf(srcaddr) != isc_sockaddr_pf(destaddr)) {
		return (ISC_R_FAMILYMISMATCH);
	}

	if (isblackholed(requestmgr->dispatchmgr, destaddr)) {
		return (DNS_R_BLACKHOLED);
	}

	result = new_request(mctx, &request);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}

	if (udptimeout == 0 && udpretries != 0) {
		udptimeout = timeout / (udpretries + 1);
		if (udptimeout == 0) {
			udptimeout = 1;
		}
	}

	timeout *= 1000;
	udptimeout *= 1000;

	request->udpcount = udpretries;
	request->dscp = dscp;

	request->event = (dns_requestevent_t *)isc_event_allocate(
		mctx, task, DNS_EVENT_REQUESTDONE, action, arg,
		sizeof(dns_requestevent_t));
	isc_task_attach(task, &tclone);
	request->event->ev_sender = task;
	request->event->request = request;
	request->event->result = ISC_R_FAILURE;
	if (key != NULL) {
		dns_tsigkey_attach(key, &request->tsigkey);
	}

use_tcp:
	tcp = ((options & DNS_REQUESTOPT_TCP) != 0);
	result = get_dispatch(tcp, false, requestmgr, srcaddr, destaddr, dscp,
			      &connected, &request->dispatch);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_dispatch_addresponse(
		request->dispatch, 0, tcp ? timeout : udptimeout, destaddr,
		task, req_connected, req_senddone, req_response, req_timeout,
		request, &id, &request->dispentry);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	message->id = id;
	if (settsigkey) {
		result = dns_message_settsigkey(message, request->tsigkey);
		if (result != ISC_R_SUCCESS) {
			goto cleanup;
		}
	}
	result = req_render(message, &request->query, options, mctx);
	if (result == DNS_R_USETCP && (options & DNS_REQUESTOPT_TCP) == 0) {
		/*
		 * Try again using TCP.
		 */
		dns_message_renderreset(message);
		dns_dispatch_removeresponse(&request->dispentry, NULL);
		dns_dispatch_detach(&request->dispatch);
		options |= DNS_REQUESTOPT_TCP;
		settsigkey = false;
		goto use_tcp;
	}
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	result = dns_message_getquerytsig(message, mctx, &request->tsig);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	LOCK(&requestmgr->lock);
	if (requestmgr->exiting) {
		UNLOCK(&requestmgr->lock);
		result = ISC_R_SHUTTINGDOWN;
		goto cleanup;
	}
	requestmgr_attach(requestmgr, &request->requestmgr);
	request->hash = mgr_gethash(requestmgr);
	ISC_LIST_APPEND(requestmgr->requests, request, link);
	UNLOCK(&requestmgr->lock);

	request->destaddr = *destaddr;
	if (!tcp || !connected) {
		result = dns_dispatch_connect(request->dispentry);
		if (result != ISC_R_SUCCESS) {
			goto unlink;
		}
		request->flags |= DNS_REQUEST_F_CONNECTING | DNS_REQUEST_F_TCP;
	} else {
		req_send(request);
	}

	req_log(ISC_LOG_DEBUG(3), "dns_request_createvia: request %p", request);
	*requestp = request;
	return (ISC_R_SUCCESS);

unlink:
	LOCK(&requestmgr->lock);
	ISC_LIST_UNLINK(requestmgr->requests, request, link);
	UNLOCK(&requestmgr->lock);

cleanup:
	if (tclone != NULL) {
		isc_task_detach(&tclone);
	}
	req_destroy(request);
	req_log(ISC_LOG_DEBUG(3), "dns_request_createvia: failed %s",
		dns_result_totext(result));
	return (result);
}

static isc_result_t
req_render(dns_message_t *message, isc_buffer_t **bufferp, unsigned int options,
	   isc_mem_t *mctx) {
	isc_buffer_t *buf1 = NULL;
	isc_buffer_t *buf2 = NULL;
	isc_result_t result;
	isc_region_t r;
	dns_compress_t cctx;
	bool cleanup_cctx = false;

	REQUIRE(bufferp != NULL && *bufferp == NULL);

	req_log(ISC_LOG_DEBUG(3), "request_render");

	/*
	 * Create buffer able to hold largest possible message.
	 */
	isc_buffer_allocate(mctx, &buf1, 65535);

	result = dns_compress_init(&cctx, -1, mctx);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	cleanup_cctx = true;

	if ((options & DNS_REQUESTOPT_CASE) != 0) {
		dns_compress_setsensitive(&cctx, true);
	}

	/*
	 * Render message.
	 */
	result = dns_message_renderbegin(message, &cctx, buf1);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	result = dns_message_rendersection(message, DNS_SECTION_QUESTION, 0);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	result = dns_message_rendersection(message, DNS_SECTION_ANSWER, 0);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	result = dns_message_rendersection(message, DNS_SECTION_AUTHORITY, 0);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	result = dns_message_rendersection(message, DNS_SECTION_ADDITIONAL, 0);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	result = dns_message_renderend(message);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	dns_compress_invalidate(&cctx);
	cleanup_cctx = false;

	/*
	 * Copy rendered message to exact sized buffer.
	 */
	isc_buffer_usedregion(buf1, &r);
	if ((options & DNS_REQUESTOPT_TCP) == 0 && r.length > 512) {
		result = DNS_R_USETCP;
		goto cleanup;
	}
	isc_buffer_allocate(mctx, &buf2, r.length);
	result = isc_buffer_copyregion(buf2, &r);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}

	/*
	 * Cleanup and return.
	 */
	isc_buffer_free(&buf1);
	*bufferp = buf2;
	return (ISC_R_SUCCESS);

cleanup:
	dns_message_renderreset(message);
	if (buf1 != NULL) {
		isc_buffer_free(&buf1);
	}
	if (buf2 != NULL) {
		isc_buffer_free(&buf2);
	}
	if (cleanup_cctx) {
		dns_compress_invalidate(&cctx);
	}
	return (result);
}

/*
 * If this request is no longer waiting for events,
 * send the completion event.  This will ultimately
 * cause the request to be destroyed.
 *
 * Requires:
 *	'request' is locked by the caller.
 */
static void
send_if_done(dns_request_t *request, isc_result_t result) {
	if (request->event != NULL && !request->canceling) {
		req_sendevent(request, result);
	}
}

/*
 * Handle the control event.
 */
static void
do_cancel(isc_task_t *task, isc_event_t *event) {
	dns_request_t *request = event->ev_arg;
	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_REQUESTCONTROL);
	LOCK(&request->requestmgr->locks[request->hash]);
	request->canceling = false;
	if (!DNS_REQUEST_CANCELED(request)) {
		req_cancel(request);
	}
	send_if_done(request, ISC_R_CANCELED);
	UNLOCK(&request->requestmgr->locks[request->hash]);
}

void
dns_request_cancel(dns_request_t *request) {
	REQUIRE(VALID_REQUEST(request));

	req_log(ISC_LOG_DEBUG(3), "dns_request_cancel: request %p", request);

	REQUIRE(VALID_REQUEST(request));

	LOCK(&request->requestmgr->locks[request->hash]);
	if (!request->canceling && !DNS_REQUEST_CANCELED(request)) {
		isc_event_t *ev = &request->ctlevent;
		isc_task_send(request->event->ev_sender, &ev);
		request->canceling = true;
	}
	UNLOCK(&request->requestmgr->locks[request->hash]);
}

isc_result_t
dns_request_getresponse(dns_request_t *request, dns_message_t *message,
			unsigned int options) {
	isc_result_t result;

	REQUIRE(VALID_REQUEST(request));
	REQUIRE(request->answer != NULL);

	req_log(ISC_LOG_DEBUG(3), "dns_request_getresponse: request %p",
		request);

	result = dns_message_setquerytsig(message, request->tsig);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	result = dns_message_settsigkey(message, request->tsigkey);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	result = dns_message_parse(message, request->answer, options);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	if (request->tsigkey != NULL) {
		result = dns_tsig_verify(request->answer, message, NULL, NULL);
	}
	return (result);
}

isc_buffer_t *
dns_request_getanswer(dns_request_t *request) {
	REQUIRE(VALID_REQUEST(request));

	return (request->answer);
}

bool
dns_request_usedtcp(dns_request_t *request) {
	REQUIRE(VALID_REQUEST(request));

	return ((request->flags & DNS_REQUEST_F_TCP) != 0);
}

void
dns_request_destroy(dns_request_t **requestp) {
	dns_request_t *request;

	REQUIRE(requestp != NULL && VALID_REQUEST(*requestp));

	request = *requestp;
	*requestp = NULL;

	req_log(ISC_LOG_DEBUG(3), "dns_request_destroy: request %p", request);

	LOCK(&request->requestmgr->lock);
	LOCK(&request->requestmgr->locks[request->hash]);
	ISC_LIST_UNLINK(request->requestmgr->requests, request, link);
	INSIST(!DNS_REQUEST_CONNECTING(request));
	INSIST(!DNS_REQUEST_SENDING(request));
	UNLOCK(&request->requestmgr->locks[request->hash]);
	UNLOCK(&request->requestmgr->lock);

	/*
	 * These should have been cleaned up by req_cancel() before
	 * the completion event was sent.
	 */
	INSIST(!ISC_LINK_LINKED(request, link));
	INSIST(request->dispentry == NULL);
	INSIST(request->dispatch == NULL);

	req_destroy(request);
}

/***
 *** Private: request.
 ***/
static void
req_connected(isc_nmhandle_t *handle, isc_result_t eresult, void *arg) {
	dns_request_t *request = (dns_request_t *)arg;

	UNUSED(handle);

	REQUIRE(VALID_REQUEST(request));
	REQUIRE(DNS_REQUEST_CONNECTING(request));

	req_log(ISC_LOG_DEBUG(3), "req_connected: request %p", request);

	LOCK(&request->requestmgr->locks[request->hash]);
	request->flags &= ~DNS_REQUEST_F_CONNECTING;

	if (DNS_REQUEST_CANCELED(request)) {
		/*
		 * Send delayed event.
		 */
		if (DNS_REQUEST_TIMEDOUT(request)) {
			send_if_done(request, ISC_R_TIMEDOUT);
		} else {
			send_if_done(request, ISC_R_CANCELED);
		}
	} else {
		if (eresult == ISC_R_SUCCESS) {
			req_send(request);
		} else {
			req_cancel(request);
			send_if_done(request, ISC_R_CANCELED);
		}
	}
	UNLOCK(&request->requestmgr->locks[request->hash]);
}

static void
req_senddone(isc_nmhandle_t *handle, isc_result_t eresult, void *arg) {
	dns_request_t *request = (dns_request_t *)arg;

	REQUIRE(VALID_REQUEST(request));
	REQUIRE(DNS_REQUEST_SENDING(request));

	UNUSED(handle);

	req_log(ISC_LOG_DEBUG(3), "req_senddone: request %p", request);

	LOCK(&request->requestmgr->locks[request->hash]);
	request->flags &= ~DNS_REQUEST_F_SENDING;

	if (DNS_REQUEST_CANCELED(request)) {
		if (DNS_REQUEST_TIMEDOUT(request)) {
			send_if_done(request, ISC_R_TIMEDOUT);
		} else {
			send_if_done(request, ISC_R_CANCELED);
		}
	} else if (eresult != ISC_R_SUCCESS) {
		req_cancel(request);
		send_if_done(request, ISC_R_CANCELED);
	}

	UNLOCK(&request->requestmgr->locks[request->hash]);
}

static void
req_response(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	dns_request_t *request = event->ev_arg;
	dns_dispatchevent_t *devent = (dns_dispatchevent_t *)event;
	isc_region_t r;

	REQUIRE(VALID_REQUEST(request));
	REQUIRE(event->ev_type == DNS_EVENT_DISPATCH);

	UNUSED(task);

	req_log(ISC_LOG_DEBUG(3), "req_response: request %p: %s", request,
		dns_result_totext(devent->result));

	LOCK(&request->requestmgr->locks[request->hash]);
	result = devent->result;
	if (result != ISC_R_SUCCESS) {
		goto done;
	}

	/*
	 * Copy buffer to request.
	 */
	isc_buffer_usedregion(&devent->buffer, &r);
	isc_buffer_allocate(request->mctx, &request->answer, r.length);
	result = isc_buffer_copyregion(request->answer, &r);
	if (result != ISC_R_SUCCESS) {
		isc_buffer_free(&request->answer);
	}
done:
	/*
	 * Cleanup.
	 */
	dns_dispatch_removeresponse(&request->dispentry, &devent);
	req_cancel(request);
	/*
	 * Send completion event.
	 */
	send_if_done(request, result);
	UNLOCK(&request->requestmgr->locks[request->hash]);
}

static void
req_timeout(isc_task_t *task, isc_event_t *event) {
	dns_request_t *request = event->ev_arg;

	REQUIRE(VALID_REQUEST(request));

	req_log(ISC_LOG_DEBUG(3), "req_timeout: request %p", request);

	UNUSED(task);
	LOCK(&request->requestmgr->locks[request->hash]);
	if (request->udpcount-- != 0) {
		if (!DNS_REQUEST_SENDING(request)) {
			req_send(request);
		}
	} else {
		request->flags |= DNS_REQUEST_F_TIMEDOUT;
		req_cancel(request);
		send_if_done(request, ISC_R_TIMEDOUT);
	}
	UNLOCK(&request->requestmgr->locks[request->hash]);
	isc_event_free(&event);
}

static void
req_sendevent(dns_request_t *request, isc_result_t result) {
	isc_task_t *task;

	REQUIRE(VALID_REQUEST(request));

	req_log(ISC_LOG_DEBUG(3), "req_sendevent: request %p", request);

	/*
	 * Lock held by caller.
	 */
	task = request->event->ev_sender;
	request->event->ev_sender = request;
	request->event->result = result;
	isc_task_sendanddetach(&task, (isc_event_t **)&request->event);
}

static void
req_destroy(dns_request_t *request) {
	REQUIRE(VALID_REQUEST(request));

	req_log(ISC_LOG_DEBUG(3), "req_destroy: request %p", request);

	request->magic = 0;
	if (request->query != NULL) {
		isc_buffer_free(&request->query);
	}
	if (request->answer != NULL) {
		isc_buffer_free(&request->answer);
	}
	if (request->event != NULL) {
		isc_event_free((isc_event_t **)&request->event);
	}
	if (request->dispentry != NULL) {
		dns_dispatch_removeresponse(&request->dispentry, NULL);
	}
	if (request->dispatch != NULL) {
		dns_dispatch_detach(&request->dispatch);
	}
	if (request->tsig != NULL) {
		isc_buffer_free(&request->tsig);
	}
	if (request->tsigkey != NULL) {
		dns_tsigkey_detach(&request->tsigkey);
	}
	if (request->requestmgr != NULL) {
		requestmgr_detach(&request->requestmgr);
	}
	isc_mem_putanddetach(&request->mctx, request, sizeof(*request));
}

/*
 * Stop the current request.  Must be called from the request's task.
 */
static void
req_cancel(dns_request_t *request) {
	REQUIRE(VALID_REQUEST(request));

	req_log(ISC_LOG_DEBUG(3), "req_cancel: request %p", request);

	/*
	 * Lock held by caller.
	 */
	request->flags |= DNS_REQUEST_F_CANCELED;

	if (request->dispentry != NULL) {
		dns_dispatch_cancel(NULL, request->dispentry,
				    DNS_REQUEST_SENDING(request),
				    DNS_REQUEST_CONNECTING(request));
		dns_dispatch_removeresponse(&request->dispentry, NULL);
	}
	dns_dispatch_detach(&request->dispatch);
}

static void
req_log(int level, const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	isc_log_vwrite(dns_lctx, DNS_LOGCATEGORY_GENERAL, DNS_LOGMODULE_REQUEST,
		       level, fmt, ap);
	va_end(ap);
}
