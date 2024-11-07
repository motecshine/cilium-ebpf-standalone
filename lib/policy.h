/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright Authors of Cilium */

#pragma once

#include <linux/icmp.h>

#include "drop.h"
#include "dbg.h"
#include "eps.h"
#include "maps.h"

static __always_inline int
__account_and_check(struct __ctx_buff *ctx __maybe_unused, struct policy_entry *policy,
		    __s8 *ext_err, __u16 *proxy_port)
{
#ifdef POLICY_ACCOUNTING
	/* FIXME: Use per cpu counters */
	__sync_fetch_and_add(&policy->packets, 1);
	__sync_fetch_and_add(&policy->bytes, ctx_full_len(ctx));
#endif

	if (unlikely(policy->deny))
		return DROP_POLICY_DENY;

	*proxy_port = policy->proxy_port;
	if (unlikely(policy->auth_type)) {
		if (ext_err)
			*ext_err = (__s8)policy->auth_type;
		return DROP_POLICY_AUTH_REQUIRED;
	}
	return CTX_ACT_OK;
}

static __always_inline int
__policy_can_access(const void *map, struct __ctx_buff *ctx, __u32 local_id,
		    __u32 remote_id, __u16 ethertype __maybe_unused, __u16 dport,
		    __u8 proto, int off __maybe_unused, int dir,
		    bool is_untracked_fragment, __u8 *match_type, __s8 *ext_err,
		    __u16 *proxy_port)
{
	struct policy_entry *policy;
	struct policy_entry *l4policy;
	struct policy_key key = {
		.lpm_key = { POLICY_FULL_PREFIX, {} }, /* always look up with unwildcarded data */
		.sec_label = remote_id,
		.egress = !dir,
		.pad = 0,
		.protocol = proto,
		.dport = dport,
	};

#if defined(ALLOW_ICMP_FRAG_NEEDED) || defined(ENABLE_ICMP_RULE)
	switch (ethertype) {
	case ETH_P_IP:
		if (proto == IPPROTO_ICMP) {
			struct icmphdr icmphdr __align_stack_8;

			if (ctx_load_bytes(ctx, off, &icmphdr, sizeof(icmphdr)) < 0)
				return DROP_INVALID;

# if defined(ALLOW_ICMP_FRAG_NEEDED)
			if (icmphdr.type == ICMP_DEST_UNREACH &&
			    icmphdr.code == ICMP_FRAG_NEEDED) {
				*proxy_port = 0;
				return CTX_ACT_OK;
			}
# endif

# if defined(ENABLE_ICMP_RULE)
			key.dport = bpf_u8_to_be16(icmphdr.type);
# endif
		}
		break;
	case ETH_P_IPV6:
# if defined(ENABLE_ICMP_RULE)
		if (proto == IPPROTO_ICMPV6) {
			__u8 icmp_type;

			if (ctx_load_bytes(ctx, off, &icmp_type, sizeof(icmp_type)) < 0)
				return DROP_INVALID;

			key.dport = bpf_u8_to_be16(icmp_type);
		}
# endif
		break;
	default:
		break;
	}
#endif /* ALLOW_ICMP_FRAG_NEEDED || ENABLE_ICMP_RULE */

	/* Policy match precedence: Entries with longer prefix lengths get precedence. If both
	 * entries have the same prefix length, then the one with non-wildcard L3 is chosen.
	 * This is an extension of the following to fully support arbitrarily masked ports:
	 *
	 * 1. id/proto/port  (L3/L4)
	 * 2. ANY/proto/port (L4-only)
	 * 3. id/proto/ANY   (L3-proto)
	 * 4. ANY/proto/ANY  (Proto-only)
	 * 5. id/ANY/ANY     (L3-only)
	 * 6. ANY/ANY/ANY    (All)
	 */

	/* Start with L3/L4 lookup.
	 *
	 * Note: Untracked fragments always have zero ports in the tuple so they can
	 * only match entries that have fully wildcarded ports.
	 */
	policy = map_lookup_elem(map, &key);

	/* This is a full L3/L4 match if port is not wildcarded,
	 * need to check for L4-only policy first if it is.
	 */
	if (likely(policy && policy->lpm_prefix_length == LPM_FULL_PREFIX_BITS)) {
		cilium_dbg3(ctx, DBG_L4_CREATE, remote_id, local_id,
			    dport << 16 | proto);
		*match_type = POLICY_MATCH_L3_L4;		/* 1. id/proto/port */
		goto check_policy;
	}

	/* L4-only lookup. */
	key.sec_label = 0;
	/*
	 * Untracked fragments always have zero ports in the tuple so they can
	 * only match entries that have fully wildcarded ports.
	 */
	l4policy = map_lookup_elem(map, &key);

	if (likely(l4policy &&
		   (!policy || l4policy->lpm_prefix_length > policy->lpm_prefix_length))) {
		__u8 p_len = l4policy->lpm_prefix_length;
		*match_type =
			p_len == 0 ? POLICY_MATCH_ALL :					/* 6. ANY/ANY/ANY */
		  p_len <= LPM_PROTO_PREFIX_BITS ? POLICY_MATCH_PROTO_ONLY :	/* 4. ANY/proto/ANY */
			POLICY_MATCH_L4_ONLY;						/* 2. ANY/proto/port */
		goto check_l4_policy;
	}

	if (likely(policy)) {
		__u8 p_len = policy->lpm_prefix_length;
		*match_type = p_len > 0 ? POLICY_MATCH_L3_PROTO :	/* 3. id/proto/ANY */
			POLICY_MATCH_L3_ONLY;				/* 5. id/ANY/ANY */
		goto check_policy;
	}

	if (is_untracked_fragment)
		return DROP_FRAG_NOSUPPORT;

	return DROP_POLICY;

check_policy:
	return __account_and_check(ctx, policy, ext_err, proxy_port);

check_l4_policy:
	return __account_and_check(ctx, l4policy, ext_err, proxy_port);
}

/**
 * Determine whether the policy allows this traffic on ingress.
 * @arg ctx		Packet to allow or deny
 * @arg map		Policy map
 * @arg src_id		Source security identity for this packet
 * @arg dst_id		Destination security identity for this packet
 * @arg ethertype	Ethertype of this packet
 * @arg dport		Destination port of this packet
 * @arg proto		L3 Protocol of this packet
 * @arg l4_off		Offset to L4 header of this packet
 * @arg is_untracked_fragment	True if packet is a TCP/UDP datagram fragment
 *				AND IPv4 fragment tracking is disabled
 * @arg match_type		Pointer to store layers used for policy match
 * @arg ext_err		Pointer to store extended error information if this packet isn't allowed
 *
 * Returns:
 *   - Positive integer indicating the proxy_port to handle this traffic
 *   - CTX_ACT_OK if the policy allows this traffic based only on labels/L3/L4
 *   - Negative error code if the packet should be dropped
 */
static __always_inline int
policy_can_ingress(struct __ctx_buff *ctx, const void *map, __u32 src_id, __u32 dst_id,
		   __u16 ethertype, __u16 dport, __u8 proto, int l4_off,
		   bool is_untracked_fragment, __u8 *match_type, __u8 *audited,
		   __s8 *ext_err, __u16 *proxy_port)
{
	int ret;

	ret = __policy_can_access(map, ctx, dst_id, src_id, ethertype, dport,
				  proto, l4_off, CT_INGRESS, is_untracked_fragment,
				  match_type, ext_err, proxy_port);
	if (ret >= CTX_ACT_OK)
		return ret;

	cilium_dbg(ctx, DBG_POLICY_DENIED, src_id, dst_id);

	*audited = 0;
#ifdef POLICY_AUDIT_MODE
	if (IS_ERR(ret)) {
		ret = CTX_ACT_OK;
		*audited = 1;
	}
#endif

	return ret;
}

static __always_inline int policy_can_ingress6(struct __ctx_buff *ctx, const void *map,
					       const struct ipv6_ct_tuple *tuple,
					       int l4_off,  __u32 src_id, __u32 dst_id,
					       __u8 *match_type, __u8 *audited,
					       __s8 *ext_err, __u16 *proxy_port)
{
	return policy_can_ingress(ctx, map, src_id, dst_id, ETH_P_IPV6, tuple->dport,
				 tuple->nexthdr, l4_off, false, match_type, audited,
				 ext_err, proxy_port);
}

static __always_inline int policy_can_ingress4(struct __ctx_buff *ctx,
		const void *map,
					       const struct ipv4_ct_tuple *tuple,
					       int l4_off, bool is_untracked_fragment,
					       __u32 src_id, __u32 dst_id,
					       __u8 *match_type, __u8 *audited,
					       __s8 *ext_err, __u16 *proxy_port)
{
	return policy_can_ingress(ctx, map, src_id, dst_id, ETH_P_IP, tuple->dport,
				 tuple->nexthdr, l4_off, is_untracked_fragment,
				 match_type, audited, ext_err, proxy_port);
}

#ifdef HAVE_ENCAP
static __always_inline bool is_encap(__u16 dport, __u8 proto)
{
	return proto == IPPROTO_UDP && dport == bpf_htons(TUNNEL_PORT);
}
#endif

static __always_inline int
policy_can_egress(struct __ctx_buff *ctx, const void *map, __u32 src_id, __u32 dst_id,
		  __u16 ethertype, __u16 dport, __u8 proto, int l4_off, __u8 *match_type,
		  __u8 *audited, __s8 *ext_err, __u16 *proxy_port)
{
	int ret;

#ifdef HAVE_ENCAP
	if (src_id != HOST_ID && is_encap(dport, proto))
		return DROP_ENCAP_PROHIBITED;
#endif
	ret = __policy_can_access(map, ctx, src_id, dst_id, ethertype, dport,
				  proto, l4_off, CT_EGRESS, false, match_type,
				  ext_err, proxy_port);
	if (ret >= 0)
		return ret;
	cilium_dbg(ctx, DBG_POLICY_DENIED, src_id, dst_id);
	*audited = 0;
#ifdef POLICY_AUDIT_MODE
	if (IS_ERR(ret)) {
		ret = CTX_ACT_OK;
		*audited = 1;
	}
#endif
	return ret;
}

static __always_inline int policy_can_egress6(struct __ctx_buff *ctx, const void *map,
					      const struct ipv6_ct_tuple *tuple,
					      int l4_off, __u32 src_id, __u32 dst_id,
					      __u8 *match_type, __u8 *audited, __s8 *ext_err,
					      __u16 *proxy_port)
{
	return policy_can_egress(ctx, map, src_id, dst_id, ETH_P_IPV6, tuple->dport,
				 tuple->nexthdr, l4_off, match_type, audited,
				 ext_err, proxy_port);
}

static __always_inline int policy_can_egress4(struct __ctx_buff *ctx, const void *map,
					      const struct ipv4_ct_tuple *tuple,
					      int l4_off, __u32 src_id, __u32 dst_id,
					      __u8 *match_type, __u8 *audited, __s8 *ext_err,
					      __u16 *proxy_port)
{
	return policy_can_egress(ctx, map, src_id, dst_id, ETH_P_IP, tuple->dport,
				 tuple->nexthdr, l4_off, match_type, audited,
				 ext_err, proxy_port);
}