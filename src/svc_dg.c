
/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

#include <config.h>

/*
 * svc_dg.c, Server side for connectionless RPC.
 *
 * Does some caching in the hopes of achieving execute-at-most-once semantics.
 */
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc_dg.h>
#include <rpc/svc_auth.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netconfig.h>
#include <err.h>

#include "rpc_com.h"
#include "svc_internal.h"
#include "clnt_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include <rpc/svc_rqst.h>
#include <misc/city.h>
#include <rpc/rpc_cksum.h>

extern tirpc_pkg_params __pkg_params;
extern struct svc_params __svc_params[1];

#define su_data(xprt) ((struct svc_dg_data *)(xprt->xp_p2))	/* XXX */
#define rpc_buffer(xprt) ((xprt)->xp_p1)

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static void svc_dg_ops(SVCXPRT *);
static enum xprt_stat svc_dg_stat(SVCXPRT *);
static bool svc_dg_recv(SVCXPRT *, struct svc_req *);

static bool svc_dg_reply(SVCXPRT *, struct svc_req *, struct rpc_msg *);
static bool svc_dg_getargs(SVCXPRT *, struct svc_req *, xdrproc_t, void *,
			   void *);
static void svc_dg_lock(SVCXPRT *, uint32_t, const char *, int);
static void svc_dg_unlock(SVCXPRT *, uint32_t, const char *, int);
static bool svc_dg_freeargs(SVCXPRT *, xdrproc_t, void *);
static bool svc_dg_ref(SVCXPRT *, u_int, const char *,
		       const int);
static void svc_dg_release(SVCXPRT *, u_int, const char *,
			   const int);
static void svc_dg_destroy(SVCXPRT *);
static bool svc_dg_control(SVCXPRT *, const u_int, void *);
static int svc_dg_cache_get(SVCXPRT *, struct rpc_msg *, char **, size_t *);
static void svc_dg_cache_set(SVCXPRT *, size_t);
int svc_dg_enablecache(SVCXPRT *, u_int);
static void svc_dg_enable_pktinfo(int, const struct __rpc_sockinfo *);
static int svc_dg_valid_pktinfo(struct msghdr *);

/*
 * Usage:
 * xprt = svc_dg_ncreate(sock, sendsize, recvsize);
 * Does other connectionless specific initializations.
 * Once *xprt is initialized, it is registered.
 * see (svc.h, xprt_register). If recvsize or sendsize are 0 suitable
 * system defaults are chosen.
 * The routines returns NULL if a problem occurred.
 */
static const char svc_dg_str[] = "svc_dg_ncreate: %s";
static const char svc_dg_err1[] = "could not get transport information";
static const char svc_dg_err2[] = " transport does not support data transfer";
static const char __no_mem_str[] = "out of memory";

SVCXPRT *
svc_dg_ncreate(int fd, u_int sendsize, u_int recvsize)
{
	SVCXPRT *xprt;
	struct svc_dg_data *su = NULL;
	struct __rpc_sockinfo si;
	struct sockaddr_storage ss;
	socklen_t slen;
	uint32_t oflags;

	if (!__rpc_fd2sockinfo(fd, &si)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, svc_dg_str, svc_dg_err1);
		return (NULL);
	}
	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
	recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);
	if ((sendsize == 0) || (recvsize == 0)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, svc_dg_str, svc_dg_err2);
		return (NULL);
	}

	xprt = mem_alloc(sizeof(SVCXPRT));
	if (xprt == NULL)
		goto freedata;
	memset(xprt, 0, sizeof(SVCXPRT));

	/* Init SVCXPRT locks, etc */
	mutex_init(&xprt->xp_lock, NULL);
	mutex_init(&xprt->xp_auth_lock, NULL);

	su = mem_alloc(sizeof(*su));
	if (su == NULL)
		goto freedata;
	su->su_iosz = ((MAX(sendsize, recvsize) + 3) / 4) * 4;
	rpc_buffer(xprt) = mem_alloc(su->su_iosz);
	if (!rpc_buffer(xprt))
		goto freedata;
	xdrmem_create(&(su->su_xdrs), rpc_buffer(xprt), su->su_iosz,
		      XDR_DECODE);

	su->su_cache = NULL;
	xprt->xp_flags = SVC_XPRT_FLAG_NONE;
	xprt->xp_refcnt = 1;
	xprt->xp_fd = fd;
	xprt->xp_p2 = su;
	svc_dg_ops(xprt);
	xprt->xp_rtaddr.maxlen = sizeof(struct sockaddr_storage);

	slen = sizeof(ss);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0)
		goto freedata;

	__rpc_set_netbuf(&xprt->xp_ltaddr, &ss, slen);

	switch (ss.ss_family) {
	case AF_INET:
		xprt->xp_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		break;
#ifdef INET6
	case AF_INET6:
		xprt->xp_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		break;
#endif
	case AF_LOCAL:
		/* no port */
		break;
	default:
		break;
	}

	/* Enable reception of IP*_PKTINFO control msgs */
	svc_dg_enable_pktinfo(fd, &si);

	/* Make reachable */
	xprt->xp_p5 = rpc_dplx_lookup_rec(
		xprt->xp_fd, RPC_DPLX_FLAG_NONE, &oflags); /* ref+1 */
	svc_rqst_init_xprt(xprt);

	/* Conditional xprt_register */
	if (!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
		xprt_register(xprt);

	return (xprt);
 freedata:
	__warnx(TIRPC_DEBUG_FLAG_SVC_DG, svc_dg_str, __no_mem_str);
	if (xprt) {
		if (su)
			(void)mem_free(su, sizeof(*su));
		svc_rqst_finalize_xprt(xprt, SVC_RQST_FLAG_NONE);
		(void)mem_free(xprt, sizeof(SVCXPRT));
	}
	return (NULL);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_dg_stat(SVCXPRT *xprt)
{
	return (XPRT_IDLE);
}

static bool
svc_dg_recv(SVCXPRT *xprt, struct svc_req *req)
{
	struct svc_dg_data *su = su_data(xprt);
	XDR *xdrs = &(su->su_xdrs);
	char *reply;
	struct sockaddr_storage ss;
	struct sockaddr *sp = (struct sockaddr *)&ss;
	struct msghdr *mesgp;
	struct iovec iov;
	size_t replylen;
	ssize_t rlen;
	unsigned int alen;

	memset(&ss, 0xff, sizeof(struct sockaddr_storage));

	/* Magic marker value to see if we didn't get the header. */

	req->rq_msg = alloc_rpc_msg();

 again:
	iov.iov_base = rpc_buffer(xprt);
	iov.iov_len = su->su_iosz;
	mesgp = &su->su_msghdr;
	memset(mesgp, 0, sizeof(*mesgp));
	mesgp->msg_iov = &iov;
	mesgp->msg_iovlen = 1;
	mesgp->msg_name = (struct sockaddr *)(void *)&ss;
	sp->sa_family = (sa_family_t) 0xffff;
	mesgp->msg_namelen = sizeof(struct sockaddr_storage);
	mesgp->msg_control = su->su_cmsg;
	mesgp->msg_controllen = sizeof(su->su_cmsg);

	rlen = recvmsg(xprt->xp_fd, mesgp, 0);

	if (sp->sa_family == (sa_family_t) 0xffff)
		return false;

	if (rlen == -1 && errno == EINTR)
		goto again;
	if (rlen == -1 || (rlen < (ssize_t) (4 * sizeof(u_int32_t))))
		return (false);

	__rpc_set_netbuf(&xprt->xp_rtaddr, &ss, mesgp->msg_namelen);

	/* Check whether there's an IP_PKTINFO or IP6_PKTINFO control message.
	 * If yes, preserve it for svc_dg_reply; otherwise just zap any cmsgs */
	if (!svc_dg_valid_pktinfo(mesgp)) {
		mesgp->msg_control = NULL;
		mesgp->msg_controllen = 0;
	}

	__xprt_set_raddr(xprt, &ss);

	xdrs->x_op = XDR_DECODE;
	XDR_SETPOS(xdrs, 0);
	if (!xdr_callmsg(xdrs, req->rq_msg))
		return (false);

	req->rq_xprt = xprt;
	req->rq_prog = req->rq_msg->rm_call.cb_prog;
	req->rq_vers = req->rq_msg->rm_call.cb_vers;
	req->rq_proc = req->rq_msg->rm_call.cb_proc;
	req->rq_xid = req->rq_msg->rm_xid;
	req->rq_clntcred =
	    req->rq_msg->rm_call.cb_cred.oa_base + (2 * MAX_AUTH_BYTES);

	/* save remote address */
	alen = xprt->xp_rtaddr.len;
	req->rq_rtaddr.buf = mem_zalloc(alen);
	if (req->rq_rtaddr.buf != NULL) {
		req->rq_rtaddr.maxlen = req->rq_rtaddr.len = alen;
		memcpy(req->rq_rtaddr.buf, xprt->xp_rtaddr.buf, alen);
	} else
		return (false);

	/* the checksum */
	req->rq_cksum =
#if 1
	    CityHash64WithSeed(iov.iov_base, MIN(256, iov.iov_len), 103);
#else
	    calculate_crc32c(0, iov.iov_base, MIN(256, iov.iov_len));
#endif

	/* XXX su->su_xid !MT-SAFE */
	su->su_xid = req->rq_msg->rm_xid;
	if (su->su_cache != NULL) {
		if (svc_dg_cache_get(xprt, req->rq_msg, &reply, &replylen)) {
			iov.iov_base = reply;
			iov.iov_len = replylen;
			(void)sendmsg(xprt->xp_fd, mesgp, 0);
			return (false);
		}
	}
	return (true);
}

static bool
svc_dg_reply(SVCXPRT *xprt, struct svc_req *req, struct rpc_msg *msg)
{
	struct svc_dg_data *su = su_data(xprt);
	XDR *xdrs = &(su->su_xdrs);
	bool stat = false;
	size_t slen;

	xdrproc_t xdr_results;
	caddr_t xdr_location;
	bool has_args;

	if (msg->rm_reply.rp_stat == MSG_ACCEPTED
	    && msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
		has_args = true;
		xdr_results = msg->acpted_rply.ar_results.proc;
		xdr_location = msg->acpted_rply.ar_results.where;
		msg->acpted_rply.ar_results.proc = (xdrproc_t) xdr_void;
		msg->acpted_rply.ar_results.where = NULL;
	} else {
		xdr_results = NULL;
		xdr_location = NULL;
		has_args = false;
	}

	xdrs->x_op = XDR_ENCODE;
	XDR_SETPOS(xdrs, 0);

	if (xdr_replymsg(xdrs, msg) && req->rq_rtaddr.len
	    && (!has_args
		||
		(SVCAUTH_WRAP
		 (req->rq_auth, req, xdrs, xdr_results, xdr_location)))) {
		struct msghdr *msg = &su->su_msghdr;
		struct iovec iov;

		iov.iov_base = rpc_buffer(xprt);
		iov.iov_len = slen = XDR_GETPOS(xdrs);
		msg->msg_iov = &iov;
		msg->msg_iovlen = 1;
		msg->msg_name = (struct sockaddr *)(void *)req->rq_rtaddr.buf;
		msg->msg_namelen = xprt->xp_rtaddr.len;
		/* cmsg already set in svc_dg_recv */

		if (sendmsg(xprt->xp_fd, msg, 0) == (ssize_t) slen) {
			stat = true;
			if (su->su_cache)
				svc_dg_cache_set(xprt, slen);
		}
	}
	return (stat);
}

static bool
svc_dg_getargs(SVCXPRT *xprt, struct svc_req *req,
	       xdrproc_t xdr_args, void *args_ptr, void *u_data)
{
	struct svc_dg_data *su = su_data(xprt);
	XDR *xdrs = &(su->su_xdrs);

	/* threads u_data for advanced decoders */
	xdrs->x_public = u_data;

	if (!SVCAUTH_UNWRAP
	    (req->rq_auth, req, &(su_data(xprt)->su_xdrs), xdr_args,
	     args_ptr)) {
		(void)svc_freeargs(xprt, xdr_args, args_ptr);
		return false;
	}

	return (true);
}

static void
svc_dg_lock(SVCXPRT *xprt, uint32_t flags, const char *file,
	    int line)
{
	rpc_dplx_rlxi(xprt, file, line);
	rpc_dplx_slxi(xprt, file, line);
}

static void
svc_dg_unlock(SVCXPRT *xprt, uint32_t flags, const char *file,
	      int line)
{
	rpc_dplx_rux(xprt);
	rpc_dplx_sux(xprt);
}

static bool
svc_dg_freeargs(SVCXPRT *xprt, xdrproc_t xdr_args, void *args_ptr)
{
	XDR xdrs = {
		.x_public = NULL,
		.x_lib = {NULL, NULL}
	};
	xdrmem_create(&xdrs, args_ptr, ~0, XDR_FREE);
	return (*xdr_args) (&xdrs, args_ptr);
}

static void
svc_dg_dodestroy(SVCXPRT *xprt)
{
	struct svc_dg_data *su = su_data(xprt);

	(void)svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);
	if (xprt->xp_fd != -1)
		(void)close(xprt->xp_fd);
	XDR_DESTROY(&(su->su_xdrs));
	(void)mem_free(rpc_buffer(xprt), su->su_iosz);
	(void)mem_free(su, sizeof(*su));
	if (xprt->xp_rtaddr.buf)
		(void)mem_free(xprt->xp_rtaddr.buf, xprt->xp_rtaddr.maxlen);
	if (xprt->xp_ltaddr.buf)
		(void)mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);
	if (xprt->xp_tp)
		(void)free(xprt->xp_tp);
	rpc_dplx_unref((struct rpc_dplx_rec *)xprt->xp_p5, RPC_DPLX_FLAG_NONE);
	/* call free hook */
	if (xprt->xp_ops2->xp_free_xprt)
		xprt->xp_ops2->xp_free_xprt(xprt);
	(void)mem_free(xprt, sizeof(SVCXPRT));
}

static bool
svc_dg_ref(SVCXPRT *xprt, u_int flags, const char *tag,
	   const int line)
{
	if (!(flags & SVC_REF_FLAG_LOCKED))
		mutex_lock(&xprt->xp_lock);

	++(xprt->xp_refcnt);
	mutex_unlock(&xprt->xp_lock);
	return (true);
}

static void
svc_dg_release(SVCXPRT *xprt, u_int flags, const char *tag,
	       const int line)
{
	uint32_t refcnt;

	if (!(flags & SVC_RELEASE_FLAG_LOCKED))
		mutex_lock(&xprt->xp_lock);

	refcnt = --(xprt->xp_refcnt);
	mutex_unlock(&xprt->xp_lock);

	if (refcnt == 0)
		svc_dg_dodestroy(xprt);
}

static void
svc_dg_destroy(SVCXPRT *xprt)
{
	uint32_t refcnt = 0;

	mutex_lock(&xprt->xp_lock);
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		mutex_unlock(&xprt->xp_lock);
		goto out;
	}
	xprt->xp_flags |= SVC_XPRT_FLAG_DESTROYED;
	mutex_unlock(&xprt->xp_lock);

	/* XXX prefer LOCKED? (would require lock order change) */
	(void)svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);

	/* clears xprt from the xprt table (eg, idle scans) */
	svc_rqst_finalize_xprt(xprt, SVC_RQST_FLAG_NONE);

	/* XXX prefer LOCKED? (would require lock order change) */
	if (refcnt == 0)
		svc_dg_dodestroy(xprt);

 out:
	return;
}

extern mutex_t ops_lock;

 /*ARGSUSED*/
static bool
svc_dg_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
		*(u_int *) in = xprt->xp_flags;
		break;
	case SVCSET_XP_FLAGS:
		xprt->xp_flags = *(u_int *) in;
		break;
	case SVCGET_XP_RECV:
		mutex_lock(&ops_lock);
		*(xp_recv_t *) in = xprt->xp_ops->xp_recv;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_RECV:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_recv = *(xp_recv_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_GETREQ:
		mutex_lock(&ops_lock);
		*(xp_getreq_t *) in = xprt->xp_ops2->xp_getreq;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_GETREQ:
		mutex_lock(&ops_lock);
		xprt->xp_ops2->xp_getreq = *(xp_getreq_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		*(xp_dispatch_t *) in = xprt->xp_ops2->xp_dispatch;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		xprt->xp_ops2->xp_dispatch = *(xp_dispatch_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_FREE_XPRT:
		mutex_lock(&ops_lock);
		*(xp_free_xprt_t *) in = xprt->xp_ops2->xp_free_xprt;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_XPRT:
		mutex_lock(&ops_lock);
		xprt->xp_ops2->xp_free_xprt = *(xp_free_xprt_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (false);
	}
	return (true);
}

static void
svc_dg_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	static struct xp_ops2 ops2;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */
	mutex_lock(&ops_lock);

	/* Fill in type of service */
	xprt->xp_type = XPRT_UDP;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_dg_recv;
		ops.xp_stat = svc_dg_stat;
		ops.xp_getargs = svc_dg_getargs;
		ops.xp_lock = svc_dg_lock;
		ops.xp_unlock = svc_dg_unlock;
		ops.xp_reply = svc_dg_reply;
		ops.xp_freeargs = svc_dg_freeargs;
		ops.xp_ref = svc_dg_ref;
		ops.xp_release = svc_dg_release;
		ops.xp_destroy = svc_dg_destroy;
		ops2.xp_control = svc_dg_control;
		ops2.xp_getreq = svc_getreq_default;
		ops2.xp_dispatch = svc_dispatch_default;
		ops2.xp_rdvs = NULL;	/* no default */
		ops2.xp_free_xprt = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	xprt->xp_ops2 = &ops2;
	mutex_unlock(&ops_lock);
}

/*
 * Enable use of the cache. Returns 1 on success, 0 on failure.
 * Note: there is no disable.
 */
static const char cache_enable_str[] = "svc_enablecache: %s %s";
static const char alloc_err[] = "could not allocate cache ";
static const char enable_err[] = "cache already enabled";

int
svc_dg_enablecache(SVCXPRT *transp, u_int size)
{
	struct svc_dg_data *su = su_data(transp);
	struct cl_cache *uc;

	mutex_lock(&dupreq_lock);
	if (su->su_cache != NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_enable_str, enable_err,
			" ");
		mutex_unlock(&dupreq_lock);
		return (0);
	}
	uc = ALLOC(struct cl_cache, 1);
	if (uc == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_enable_str, alloc_err,
			" ");
		mutex_unlock(&dupreq_lock);
		return (0);
	}
	uc->uc_size = size;
	uc->uc_nextvictim = 0;
	uc->uc_entries = ALLOC(cache_ptr, size * SPARSENESS);
	if (uc->uc_entries == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_enable_str, alloc_err,
			"data");
		FREE(uc, struct cl_cache, 1);
		mutex_unlock(&dupreq_lock);
		return (0);
	}
	MEMZERO(uc->uc_entries, cache_ptr, size * SPARSENESS);
	uc->uc_fifo = ALLOC(cache_ptr, size);
	if (uc->uc_fifo == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_enable_str, alloc_err,
			"fifo");
		FREE(uc->uc_entries, cache_ptr, size * SPARSENESS);
		FREE(uc, struct cl_cache, 1);
		mutex_unlock(&dupreq_lock);
		return (0);
	}
	MEMZERO(uc->uc_fifo, cache_ptr, size);
	su->su_cache = (char *)(void *)uc;
	mutex_unlock(&dupreq_lock);
	return (1);
}

/*
 * Set an entry in the cache.  It assumes that the uc entry is set from
 * the earlier call to svc_dg_cache_get() for the same procedure.  This will
 * always happen because svc_dg_cache_get() is calle by svc_dg_recv and
 * svc_dg_cache_set() is called by svc_dg_reply().  All this hoopla because
 * the right RPC parameters are not available at svc_dg_reply time.
 */

static const char cache_set_str[] = "cache_set: %s";
static const char cache_set_err1[] = "victim not found";
static const char cache_set_err2[] = "victim alloc failed";
static const char cache_set_err3[] = "could not allocate new rpc buffer";

static void
svc_dg_cache_set(SVCXPRT *xprt, size_t replylen)
{
	cache_ptr victim;
	cache_ptr *vicp;
	struct svc_dg_data *su = su_data(xprt);
	struct cl_cache *uc = (struct cl_cache *)su->su_cache;
	u_int loc;
	char *newbuf;
	struct netconfig *nconf;
	char *uaddr;

	mutex_lock(&dupreq_lock);
	/*
	 * Find space for the new entry, either by
	 * reusing an old entry, or by mallocing a new one
	 */
	victim = uc->uc_fifo[uc->uc_nextvictim];
	if (victim != NULL) {
		loc = CACHE_LOC(xprt, victim->cache_xid);
		for (vicp = &uc->uc_entries[loc];
		     *vicp != NULL && *vicp != victim; 
		     vicp = &(*vicp)->cache_next);
		if (*vicp == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_set_str,
				cache_set_err1);
			mutex_unlock(&dupreq_lock);
			return;
		}
		*vicp = victim->cache_next;	/* remove from cache */
		newbuf = victim->cache_reply;
	} else {
		victim = ALLOC(struct cache_node, 1);
		if (victim == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_set_str,
				cache_set_err2);
			mutex_unlock(&dupreq_lock);
			return;
		}
		newbuf = mem_alloc(su->su_iosz);
		if (newbuf == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_DG, cache_set_str,
				cache_set_err3);
			FREE(victim, struct cache_node, 1);
			mutex_unlock(&dupreq_lock);
			return;
		}
	}

	/*
	 * Store it away
	 */
	if (__pkg_params.debug_flags & TIRPC_DEBUG_FLAG_RPC_CACHE) {
		nconf = getnetconfigent(xprt->xp_netid);
		if (nconf) {
			uaddr = taddr2uaddr(nconf, &xprt->xp_rtaddr);
			freenetconfigent(nconf);
			__warnx(TIRPC_DEBUG_FLAG_SVC_DG,
				"cache set for xid= %x prog=%d vers=%d proc=%d "
				"for rmtaddr=%s\n", su->su_xid, uc->uc_prog,
				uc->uc_vers, uc->uc_proc, uaddr);
			mem_free(uaddr, 0);	/* XXX */
		}
	}			/* DEBUG_RPC_CACHE */
	victim->cache_replylen = replylen;
	victim->cache_reply = rpc_buffer(xprt);
	rpc_buffer(xprt) = newbuf;
	xdrmem_create(&(su->su_xdrs), rpc_buffer(xprt), su->su_iosz,
		      XDR_ENCODE);
	victim->cache_xid = su->su_xid;
	victim->cache_proc = uc->uc_proc;
	victim->cache_vers = uc->uc_vers;
	victim->cache_prog = uc->uc_prog;
	victim->cache_addr = xprt->xp_rtaddr;
	victim->cache_addr.buf = ALLOC(char, xprt->xp_rtaddr.len);
	(void)memcpy(victim->cache_addr.buf, xprt->xp_rtaddr.buf,
		     (size_t) xprt->xp_rtaddr.len);
	loc = CACHE_LOC(xprt, victim->cache_xid);
	victim->cache_next = uc->uc_entries[loc];
	uc->uc_entries[loc] = victim;
	uc->uc_fifo[uc->uc_nextvictim++] = victim;
	uc->uc_nextvictim %= uc->uc_size;
	mutex_unlock(&dupreq_lock);
}

/*
 * Try to get an entry from the cache
 * return 1 if found, 0 if not found and set the stage for svc_dg_cache_set()
 */
static int
svc_dg_cache_get(SVCXPRT *xprt, struct rpc_msg *msg, char **replyp,
		 size_t *replylenp)
{
	u_int loc;
	cache_ptr ent;
	struct svc_dg_data *su = su_data(xprt);
	struct cl_cache *uc = (struct cl_cache *)su->su_cache;
	struct netconfig *nconf;
	char *uaddr;

	mutex_lock(&dupreq_lock);
	loc = CACHE_LOC(xprt, su->su_xid);
	for (ent = uc->uc_entries[loc]; ent != NULL; ent = ent->cache_next) {
		if (ent->cache_xid == su->su_xid
		    && ent->cache_proc == msg->rm_call.cb_proc
		    && ent->cache_vers == msg->rm_call.cb_vers
		    && ent->cache_prog == msg->rm_call.cb_prog
		    && ent->cache_addr.len == xprt->xp_rtaddr.len
		    &&
		    (memcmp
		     (ent->cache_addr.buf, xprt->xp_rtaddr.buf,
		      xprt->xp_rtaddr.len) == 0)) {
			if (__pkg_params.
			    debug_flags & TIRPC_DEBUG_FLAG_RPC_CACHE) {
				nconf = getnetconfigent(xprt->xp_netid);
				if (nconf) {
					uaddr =
					    taddr2uaddr(nconf,
							&xprt->xp_rtaddr);
					freenetconfigent(nconf);
					__warnx(TIRPC_DEBUG_FLAG_SVC_DG,
						"cache entry found for xid=%x prog=%d "
						"vers=%d proc=%d for rmtaddr=%s\n",
						su->su_xid,
						msg->rm_call.cb_prog,
						msg->rm_call.cb_vers,
						msg->rm_call.cb_proc, uaddr);
					mem_free(uaddr, 0);
				}
			}	/* RPC_CACHE_DEBUG */
			*replyp = ent->cache_reply;
			*replylenp = ent->cache_replylen;
			mutex_unlock(&dupreq_lock);
			return (1);
		}
	}
	/*
	 * Failed to find entry
	 * Remember a few things so we can do a set later
	 */
	uc->uc_proc = msg->rm_call.cb_proc;
	uc->uc_vers = msg->rm_call.cb_vers;
	uc->uc_prog = msg->rm_call.cb_prog;
	mutex_unlock(&dupreq_lock);
	return (0);
}

/*
 * Enable reception of PKTINFO control messages
 */
void
svc_dg_enable_pktinfo(int fd, const struct __rpc_sockinfo *si)
{
	int val = 1;

	switch (si->si_af) {
	case AF_INET:
#ifdef SOL_IP
		(void)setsockopt(fd, SOL_IP, IP_PKTINFO, &val, sizeof(val));
#endif
		break;

	case AF_INET6:
#ifdef SOL_IPV6
		(void)setsockopt(fd, SOL_IPV6, IPV6_PKTINFO, &val, sizeof(val));
#endif
		break;
	}
}

/*
 * When given a control message received from the socket
 * layer, check whether it contains valid PKTINFO data matching
 * the address family of the peer address.
 */
int
svc_dg_valid_pktinfo(struct msghdr *msg)
{
	struct cmsghdr *cmsg;

	if (!msg->msg_name)
		return 0;

	if (msg->msg_flags & MSG_CTRUNC)
		return 0;

	cmsg = CMSG_FIRSTHDR(msg);
	if (cmsg == NULL || CMSG_NXTHDR(msg, cmsg) != NULL)
		return 0;

	switch (((struct sockaddr *)msg->msg_name)->sa_family) {
	case AF_INET:
#ifdef SOL_IP
		if (cmsg->cmsg_level != SOL_IP || cmsg->cmsg_type != IP_PKTINFO
		    || cmsg->cmsg_len < CMSG_LEN(sizeof(struct in_pktinfo))) {
			return 0;
		} else {
			struct in_pktinfo *pkti;

			pkti = (struct in_pktinfo *)CMSG_DATA(cmsg);
			pkti->ipi_ifindex = 0;
		}
#endif
		break;

	case AF_INET6:
#ifdef SOL_IPV6
		if (cmsg->cmsg_level != SOL_IPV6
		    || cmsg->cmsg_type != IPV6_PKTINFO
		    || cmsg->cmsg_len < CMSG_LEN(sizeof(struct in6_pktinfo))) {
			return 0;
		} else {
			struct in6_pktinfo *pkti;

			pkti = (struct in6_pktinfo *)CMSG_DATA(cmsg);
			pkti->ipi6_ifindex = 0;
		}
#endif
		break;

	default:
		return 0;
	}

	return 1;
}
