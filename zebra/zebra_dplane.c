/*
 * Zebra dataplane layer.
 * Copyright (c) 2018 Volta Networks, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/libfrr.h"
#include "lib/debug.h"
#include "lib/frratomic.h"
#include "lib/frr_pthread.h"
#include "lib/memory.h"
#include "lib/queue.h"
#include "lib/zebra.h"
#include "zebra/zebra_memory.h"
#include "zebra/zserv.h"
#include "zebra/zebra_dplane.h"
#include "zebra/rt.h"
#include "zebra/debug.h"

/* Memory type for context blocks */
DEFINE_MTYPE(ZEBRA, DP_CTX, "Zebra DPlane Ctx")
DEFINE_MTYPE(ZEBRA, DP_PROV, "Zebra DPlane Provider")

#ifndef AOK
#  define AOK 0
#endif

/* Validation value for context blocks */
const uint32_t DPLANE_CTX_MAGIC = 0xb97a557f;

/* Validation check macro for context blocks */
/* #define DPLANE_DEBUG 1 */

#ifdef DPLANE_DEBUG

#  define DPLANE_CTX_VALID(p)						\
		assert((p) && ((p)->zd_magic == DPLANE_CTX_MAGIC))

#else

#  define DPLANE_CTX_VALID(p)

#endif	/* DPLANE_DEBUG */

/*
 * The context block used to exchange info about route updates across
 * the boundary between the zebra main context (and pthread) and the
 * dataplane layer (and pthread).
 */
struct zebra_dplane_ctx_s {

	/* Operation code */
	enum dplane_op_e zd_op;

	/* Status on return */
	enum zebra_dplane_result zd_status;

	/* TODO -- internal/sub-operation status? */
	enum zebra_dplane_status zd_remote_status;
	enum zebra_dplane_status zd_kernel_status;

	/* Dest and (optional) source prefixes */
	struct prefix zd_dest;
	struct prefix zd_src;

	bool zd_is_update;

	uint32_t zd_seq;
	uint32_t zd_old_seq;
	vrf_id_t zd_vrf_id;
	uint32_t zd_table_id;

	int zd_type;
	int zd_old_type;

	afi_t zd_afi;
	safi_t zd_safi;

	route_tag_t zd_tag;
	route_tag_t zd_old_tag;
	uint32_t zd_metric;
	uint32_t zd_old_metric;
	uint16_t zd_instance;
	uint16_t zd_old_instance;

	uint8_t zd_distance;
	uint8_t zd_old_distance;

	uint32_t zd_mtu;
	uint32_t zd_nexthop_mtu;

	/* Namespace info */
	struct zebra_dplane_info zd_ns_info;

	/* Nexthops */
	struct nexthop_group zd_ng;

	/* "Previous" nexthops, used only in route update case without netlink */
	struct nexthop_group zd_old_ng;

	/* TODO -- use fixed array of nexthops, to avoid mallocs? */

	/* Embedded list linkage */
	TAILQ_ENTRY(zebra_dplane_ctx_s) zd_q_entries;

	/* Magic validation value */
	uint32_t zd_magic;
};

/*
 * Registration block for one dataplane provider.
 */
struct zebra_dplane_provider_s {
	/* Name */
	char dp_name[DPLANE_PROVIDER_NAMELEN + 1];

	/* Priority, for ordering among providers */
	uint8_t dp_priority;

	/* Id value */
	uint32_t dp_id;

	dplane_provider_process_fp dp_fp;

	dplane_provider_fini_fp dp_fini;

	_Atomic uint64_t dp_in_counter;
	_Atomic uint64_t dp_error_counter;

	/* Embedded list linkage */
	TAILQ_ENTRY(zebra_dplane_provider_s) dp_q_providers;

};

/*
 * Globals
 */
static struct zebra_dplane_globals_s {
	/* Mutex to control access to dataplane components */
	pthread_mutex_t dg_mutex;

	/* Results callback registered by zebra 'core' */
	dplane_results_fp dg_results_cb;

	/* Sentinel for shutdown */
	volatile bool dg_run;

	/* Route-update context queue inbound to the dataplane */
	TAILQ_HEAD(zdg_ctx_q, zebra_dplane_ctx_s) dg_route_ctx_q;

	/* Ordered list of providers */
	TAILQ_HEAD(zdg_prov_q, zebra_dplane_provider_s) dg_providers_q;

	/* Counter used to assign internal ids to providers */
	uint32_t dg_provider_id;

	_Atomic uint64_t dg_routes_in;
	_Atomic uint32_t dg_routes_queued;
	_Atomic uint64_t dg_route_errors;

	/* Event-delivery context 'master' for the dplane */
	struct thread_master *dg_master;

	/* Event/'thread' pointer for queued updates */
	struct thread *dg_t_update;

} zdplane_g;

/*
 * Lock and unlock for interactions with the zebra 'core'
 */
#define DPLANE_LOCK() pthread_mutex_lock(&zdplane_g.dg_mutex)

#define DPLANE_UNLOCK() pthread_mutex_unlock(&zdplane_g.dg_mutex)

/* Prototypes */
static int dplane_route_process(struct thread *event);

/*
 * Public APIs
 */

/*
 * Allocate a dataplane update context
 */
static dplane_ctx_h dplane_ctx_alloc(void)
{
	struct zebra_dplane_ctx_s *p;

	/* TODO -- just alloc'ing memory, but would like to maintain
	 * a pool
	 */
	p = XCALLOC(MTYPE_DP_CTX, sizeof(struct zebra_dplane_ctx_s));
	if (p)
		p->zd_magic = DPLANE_CTX_MAGIC;

	return p;
}

/*
 * Free a dataplane results context.
 */
static void dplane_ctx_free(dplane_ctx_h *pctx)
{
	if (pctx) {
		DPLANE_CTX_VALID(*pctx);

		/* TODO -- just freeing memory, but would like to maintain
		 * a pool
		 */

		/* Free embedded nexthops */
		if ((*pctx)->zd_ng.nexthop) {
			/* This deals with recursive nexthops too */
			nexthops_free((*pctx)->zd_ng.nexthop);
		}

		if ((*pctx)->zd_old_ng.nexthop) {
			nexthops_free((*pctx)->zd_old_ng.nexthop);
		}

		/* Clear validation value */
		(*pctx)->zd_magic = 0;

		XFREE(MTYPE_DP_CTX, *pctx);
		*pctx = NULL;
	}
}

/*
 * Return a context block to the dplane module after processing
 */
void dplane_ctx_fini(dplane_ctx_h *pctx)
{
	/* TODO -- enqueue for next provider; for now, just free */
	dplane_ctx_free(pctx);
}

/* Enqueue a context block */
void dplane_ctx_enqueue_tail(struct dplane_ctx_q_s *q, dplane_ctx_h ctx)
{
	TAILQ_INSERT_TAIL(q, ctx, zd_q_entries);
}

/* Dequeue a context block from the head of a list */
void dplane_ctx_dequeue(struct dplane_ctx_q_s *q, dplane_ctx_h *ctxp)
{
	dplane_ctx_h ctx = TAILQ_FIRST(q);

	if (ctx)
		TAILQ_REMOVE(q, ctx, zd_q_entries);

	*ctxp = ctx;
}

/*
 * Accessors for information from the context object
 */
enum zebra_dplane_result dplane_ctx_get_status(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_status;
}

enum dplane_op_e dplane_ctx_get_op(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_op;
}

const char *dplane_op2str(enum dplane_op_e op)
{
	const char *ret = "UNKNOWN";

	switch (op) {
	case DPLANE_OP_NONE:
		ret = "NONE";
		break;

	/* Route update */
	case DPLANE_OP_ROUTE_INSTALL:
		ret = "ROUTE_INSTALL";
		break;
	case DPLANE_OP_ROUTE_UPDATE:
		ret = "ROUTE_UPDATE";
		break;
	case DPLANE_OP_ROUTE_DELETE:
		ret = "ROUTE_DELETE";
		break;

	};

	return ret;
}

const struct prefix *dplane_ctx_get_dest(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->zd_dest);
}

/* Source prefix is a little special - return NULL for "no src prefix" */
const struct prefix *dplane_ctx_get_src(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	if (ctx->zd_src.prefixlen == 0 &&
	    IN6_IS_ADDR_UNSPECIFIED(&(ctx->zd_src.u.prefix6))) {
		return NULL;
	} else {
		return &(ctx->zd_src);
	}
}

bool dplane_ctx_is_update(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_is_update;
}

uint32_t dplane_ctx_get_seq(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_seq;
}

uint32_t dplane_ctx_get_old_seq(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_seq;
}

vrf_id_t dplane_ctx_get_vrf(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_vrf_id;
}

int dplane_ctx_get_type(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_type;
}

int dplane_ctx_get_old_type(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_type;
}

afi_t dplane_ctx_get_afi(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_afi;
}

safi_t dplane_ctx_get_safi(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_safi;
}

uint32_t dplane_ctx_get_table(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_table_id;
}

route_tag_t dplane_ctx_get_tag(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_tag;
}

route_tag_t dplane_ctx_get_old_tag(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_tag;
}

uint16_t dplane_ctx_get_instance(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_instance;
}

uint16_t dplane_ctx_get_old_instance(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_instance;
}

uint32_t dplane_ctx_get_metric(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_metric;
}

uint32_t dplane_ctx_get_old_metric(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_metric;
}

uint32_t dplane_ctx_get_mtu(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_mtu;
}

uint32_t dplane_ctx_get_nh_mtu(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_nexthop_mtu;
}

uint8_t dplane_ctx_get_distance(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_distance;
}

uint8_t dplane_ctx_get_old_distance(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_distance;
}

const struct nexthop_group *dplane_ctx_get_ng(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->zd_ng);
}

const struct nexthop_group *dplane_ctx_get_old_ng(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->zd_old_ng);
}

const struct zebra_dplane_info *dplane_ctx_get_ns(const dplane_ctx_h ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->zd_ns_info);
}

/*
 * End of dplane context accessors
 */

/*
 * Initialize a context block for a route update from zebra data structs.
 */
static int dplane_ctx_route_init(dplane_ctx_h ctx,
				 enum dplane_op_e op,
				 struct route_node *rn,
				 struct route_entry *re)
{
	int ret = EINVAL;
	const struct route_table *table = NULL;
	const rib_table_info_t *info;
	const struct prefix *p, *src_p;
	struct zebra_ns *zns;
	struct zebra_vrf *zvrf;

	if (!ctx || !rn || !re)
		goto done;

	ctx->zd_op = op;

	ctx->zd_type = re->type;
	ctx->zd_old_type = re->type;

	/* Prefixes: dest, and optional source */
	srcdest_rnode_prefixes(rn, &p, &src_p);

	prefix_copy(&(ctx->zd_dest), p);

	if (src_p)
		prefix_copy(&(ctx->zd_src), src_p);
	else
		memset(&(ctx->zd_src), 0, sizeof(ctx->zd_src));

	ctx->zd_table_id = re->table;

	ctx->zd_metric = re->metric;
	ctx->zd_old_metric = re->metric;
	ctx->zd_vrf_id = re->vrf_id;
	ctx->zd_mtu = re->mtu;
	ctx->zd_nexthop_mtu = re->nexthop_mtu;
	ctx->zd_instance = re->instance;
	ctx->zd_tag = re->tag;
	ctx->zd_old_tag = re->tag;
	ctx->zd_distance = re->distance;

	table = srcdest_rnode_table(rn);
	info = table->info;

	ctx->zd_afi = info->afi;
	ctx->zd_safi = info->safi;

	/* Extract ns info - can't use pointers to 'core' structs */
	zvrf = vrf_info_lookup(re->vrf_id);
	zns = zvrf->zns;

	zebra_dplane_info_from_zns(&(ctx->zd_ns_info), zns, true /*is_cmd*/);

#if defined(HAVE_NETLINK)
	/* Increment message counter after copying to context struct - may need
	 * two messages in some 'update' cases.
	 */
	if (op == DPLANE_OP_ROUTE_UPDATE)
		zns->netlink_cmd.seq += 2;
	else
		zns->netlink_cmd.seq++;
#endif /* NETLINK*/

	/* Copy nexthops; recursive info is included too */
	copy_nexthops(&(ctx->zd_ng.nexthop), re->ng.nexthop, NULL);

	/* TODO -- maybe use array of nexthops to avoid allocs? */

	/* Trying out the sequence number idea, so we can try to detect
	 * when a result is stale.
	 */
	re->dplane_sequence++;
	ctx->zd_seq = re->dplane_sequence;

	ret = AOK;

done:
	return ret;
}

/*
 * Enqueue a new route update,
 * and ensure an event is active for the dataplane thread.
 */
static int dplane_route_enqueue(dplane_ctx_h ctx)
{
	int ret = EINVAL;

	/* Enqueue for processing by the dataplane thread */
	DPLANE_LOCK();
	{
		TAILQ_INSERT_TAIL(&zdplane_g.dg_route_ctx_q, ctx, zd_q_entries);
	}
	DPLANE_UNLOCK();

	/* Ensure that an event for the dataplane thread is active */
	thread_add_event(zdplane_g.dg_master, dplane_route_process, NULL, 0,
			 &zdplane_g.dg_t_update);

	ret = AOK;

	return ret;
}

/*
 * Attempt to dequeue a route-update block
 */
static dplane_ctx_h dplane_route_dequeue(void)
{
	dplane_ctx_h ctx = NULL;

	DPLANE_LOCK();
	{
		ctx = TAILQ_FIRST(&zdplane_g.dg_route_ctx_q);
		if (ctx) {
			TAILQ_REMOVE(&zdplane_g.dg_route_ctx_q,
				     ctx, zd_q_entries);
		}
	}
	DPLANE_UNLOCK();

	return ctx;
}

/*
 * Utility that prepares a route update and enqueues it for processing
 */
static enum zebra_dplane_result
dplane_route_update_internal(struct route_node *rn,
			     struct route_entry *re,
			     struct route_entry *old_re,
			     enum dplane_op_e op)
{
	enum zebra_dplane_result result = ZEBRA_DPLANE_REQUEST_FAILURE;
	int ret = EINVAL;
	dplane_ctx_h ctx = NULL;

	/* Obtain context block */
	ctx = dplane_ctx_alloc();
	if (ctx == NULL) {
		ret = ENOMEM;
		goto done;
	}

	/* Init context with info from zebra data structs */
	ret = dplane_ctx_route_init(ctx, op, rn, re);
	if (ret == AOK) {
		/* Capture some extra info for update case
		 * where there's a different 'old' route.
		 */
		if ((op == DPLANE_OP_ROUTE_UPDATE) &&
		    old_re && (old_re != re)) {
			ctx->zd_is_update = true;

			old_re->dplane_sequence++;
			ctx->zd_old_seq = old_re->dplane_sequence;

			ctx->zd_old_tag = old_re->tag;
			ctx->zd_old_type = old_re->type;
			ctx->zd_old_instance = old_re->instance;
			ctx->zd_old_distance = old_re->distance;
			ctx->zd_old_metric = old_re->metric;

#ifndef HAVE_NETLINK
			/* Capture previous re's nexthops too for bsd, sigh */
			copy_nexthops(&(ctx->zd_old_ng.nexthop),
				      old_re->ng.nexthop, NULL);
#endif	/* !HAVE_NETLINK */
		}

		/* Enqueue context for processing */
		ret = dplane_route_enqueue(ctx);
	}

done:
	/* Update counters */
	atomic_fetch_add_explicit(&zdplane_g.dg_routes_in, 1,
				  memory_order_relaxed);

	if (ret == AOK) {
		atomic_fetch_add_explicit(&zdplane_g.dg_routes_queued, 1,
					  memory_order_relaxed);
		result = ZEBRA_DPLANE_REQUEST_QUEUED;
	} else if (ctx) {
		atomic_fetch_add_explicit(&zdplane_g.dg_route_errors, 1,
					  memory_order_relaxed);
		dplane_ctx_free(&ctx);
	}

	return result;
}

/*
 * Enqueue a route 'add' for the dataplane.
 */
enum zebra_dplane_result dplane_route_add(struct route_node *rn,
					  struct route_entry *re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, NULL,
					   DPLANE_OP_ROUTE_INSTALL);

done:
	return ret;
}

/*
 * Enqueue a route update for the dataplane.
 */
enum zebra_dplane_result dplane_route_update(struct route_node *rn,
					     struct route_entry *re,
					     struct route_entry *old_re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, old_re,
					   DPLANE_OP_ROUTE_UPDATE);
done:
	return ret;
}

/*
 * Enqueue a route removal for the dataplane.
 */
enum zebra_dplane_result dplane_route_delete(struct route_node *rn,
					     struct route_entry *re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, NULL,
					   DPLANE_OP_ROUTE_DELETE);

done:
	return ret;
}

/*
 * Event handler function for routing updates
 */
static int dplane_route_process(struct thread *event)
{
	enum zebra_dplane_result res;
	dplane_ctx_h ctx;

	while (1) {
		/* Check for shutdown */
		if (!zdplane_g.dg_run)
			break;

		/* TODO -- limit number of updates per cycle? */
		ctx = dplane_route_dequeue();
		if (ctx == NULL)
			break;

		/* Update counter */
		atomic_fetch_sub_explicit(&zdplane_g.dg_routes_queued, 1,
					  memory_order_relaxed);

		if (IS_ZEBRA_DEBUG_DPLANE_DETAIL) {
			char dest_str[PREFIX_STRLEN];

			prefix2str(dplane_ctx_get_dest(ctx),
				   dest_str, sizeof(dest_str));

			zlog_debug("%u:%s Dplane route update ctx %p op %s",
				   dplane_ctx_get_vrf(ctx), dest_str,
				   ctx, dplane_op2str(dplane_ctx_get_op(ctx)));
		}

		/* TODO -- support series of providers */

		/* Initially, just doing kernel-facing update here */
		res = kernel_route_update(ctx);

		if (res != ZEBRA_DPLANE_REQUEST_SUCCESS)
			atomic_fetch_add_explicit(&zdplane_g.dg_route_errors, 1,
						  memory_order_relaxed);

		ctx->zd_status = res;

		/* Enqueue result to zebra main context */
		(*zdplane_g.dg_results_cb)(ctx);

		ctx = NULL;
	}

	return 0;
}

/*
 * Handler for 'show dplane'
 */
int dplane_show_helper(struct vty *vty, bool detailed)
{
	uint64_t queued, errs, incoming;

	incoming = atomic_load_explicit(&zdplane_g.dg_routes_in,
					memory_order_relaxed);
	queued = atomic_load_explicit(&zdplane_g.dg_routes_queued,
				      memory_order_relaxed);
	errs = atomic_load_explicit(&zdplane_g.dg_route_errors,
				    memory_order_relaxed);

	vty_out(vty, "Route updates:            %"PRIu64"\n", incoming);
	vty_out(vty, "Route update errors:      %"PRIu64"\n", errs);
	vty_out(vty, "Route update queue depth: %"PRIu64"\n", queued);

	return CMD_SUCCESS;
}

/*
 * Handler for 'show dplane providers'
 */
int dplane_show_provs_helper(struct vty *vty, bool detailed)
{
	vty_out(vty, "Zebra dataplane providers:%s\n",
		(detailed ? " (detailed)" : ""));

	return CMD_SUCCESS;
}

/*
 * Provider registration
 */
int dplane_provider_register(const char *name,
			     enum dplane_provider_prio_e prio,
			     dplane_provider_process_fp fp,
			     dplane_provider_fini_fp fini_fp)
{
	int ret = 0;
	struct zebra_dplane_provider_s *p, *last;

	/* Validate */
	if (fp == NULL) {
		ret = EINVAL;
		goto done;
	}

	if (prio <= DPLANE_PRIO_NONE ||
	    prio > DPLANE_PRIO_LAST) {
		ret = EINVAL;
		goto done;
	}

	/* Allocate and init new provider struct */
	p = XCALLOC(MTYPE_DP_PROV, sizeof(struct zebra_dplane_provider_s));
	if (p == NULL) {
		ret = ENOMEM;
		goto done;
	}

	strncpy(p->dp_name, name, DPLANE_PROVIDER_NAMELEN);
	p->dp_name[DPLANE_PROVIDER_NAMELEN] = '\0'; /* Belt-and-suspenders */

	p->dp_priority = prio;
	p->dp_fp = fp;
	p->dp_fini = fini_fp;

	/* Lock the lock - the dplane pthread may be running */
	DPLANE_LOCK();

	p->dp_id = ++zdplane_g.dg_provider_id;

	/* Insert into list ordered by priority */
	TAILQ_FOREACH(last, &zdplane_g.dg_providers_q, dp_q_providers) {
		if (last->dp_priority > p->dp_priority)
			break;
	}

	if (last)
		TAILQ_INSERT_BEFORE(last, p, dp_q_providers);
	else
		TAILQ_INSERT_TAIL(&zdplane_g.dg_providers_q, p, dp_q_providers);

	/* And unlock */
	DPLANE_UNLOCK();

done:
	return ret;
}

/*
 * Zebra registers a results callback with the dataplane system
 */
int dplane_results_register(dplane_results_fp fp)
{
	zdplane_g.dg_results_cb = fp;
	return AOK;
}

/*
 * Initialize the dataplane module during startup, internal/private version
 */
static void zebra_dplane_init_internal(struct zebra_t *zebra)
{
	memset(&zdplane_g, 0, sizeof(zdplane_g));

	pthread_mutex_init(&zdplane_g.dg_mutex, NULL);

	TAILQ_INIT(&zdplane_g.dg_route_ctx_q);
	TAILQ_INIT(&zdplane_g.dg_providers_q);

	/* TODO -- register default kernel 'provider' during init */

	zdplane_g.dg_run = true;

	/* TODO -- start dataplane pthread. We're using the zebra
	 * core/main thread temporarily
	 */
	zdplane_g.dg_master = zebra->master;
}

/*
 * Shutdown, de-init api. This runs pretty late during shutdown,
 * because zebra tries to free/remove/uninstall all routes during shutdown.
 * NB: This runs in the main zebra thread context.
 */
void zebra_dplane_finish(void)
{
	/* Wait until all pending updates are processed */

	/* Stop dplane thread, if it's running */

	zdplane_g.dg_run = false;

	THREAD_OFF(zdplane_g.dg_t_update);

	/* Notify provider(s) of shutdown */

	/* Clean-up provider objects */

	/* Clean queue(s) */

}

/*
 * Initialize the dataplane module at startup; called by zebra rib_init()
 */
void zebra_dplane_init(void)
{
	zebra_dplane_init_internal(&zebrad);

	/* Finalize/cleanup code is called quite late during zebra shutdown;
	 * zebra expects to try to clean up all vrfs and all routes during
	 * shutdown, so the dplane must be available until very late.
	 */
}
