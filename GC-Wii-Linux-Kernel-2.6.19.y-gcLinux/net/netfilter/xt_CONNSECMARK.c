/*
 * This module is used to copy security markings from packets
 * to connections, and restore security markings from connections
 * back to packets.  This would normally be performed in conjunction
 * with the SECMARK target and state match.
 *
 * Based somewhat on CONNMARK:
 *   Copyright (C) 2002,2004 MARA Systems AB <http://www.marasystems.com>
 *    by Henrik Nordstrom <hno@marasystems.com>
 *
 * (C) 2006 Red Hat, Inc., James Morris <jmorris@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_CONNSECMARK.h>
#include <net/netfilter/nf_conntrack_compat.h>

#define PFX "CONNSECMARK: "

MODULE_LICENSE("GPL");
MODULE_AUTHOR("James Morris <jmorris@redhat.com>");
MODULE_DESCRIPTION("ip[6]tables CONNSECMARK module");
MODULE_ALIAS("ipt_CONNSECMARK");
MODULE_ALIAS("ip6t_CONNSECMARK");

/*
 * If the packet has a security mark and the connection does not, copy
 * the security mark from the packet to the connection.
 */
static void secmark_save(struct sk_buff *skb)
{
	if (skb->secmark) {
		u32 *connsecmark;
		enum ip_conntrack_info ctinfo;

		connsecmark = nf_ct_get_secmark(skb, &ctinfo);
		if (connsecmark && !*connsecmark)
			if (*connsecmark != skb->secmark)
				*connsecmark = skb->secmark;
	}
}

/*
 * If packet has no security mark, and the connection does, restore the
 * security mark from the connection to the packet.
 */
static void secmark_restore(struct sk_buff *skb)
{
	if (!skb->secmark) {
		u32 *connsecmark;
		enum ip_conntrack_info ctinfo;

		connsecmark = nf_ct_get_secmark(skb, &ctinfo);
		if (connsecmark && *connsecmark)
			if (skb->secmark != *connsecmark)
				skb->secmark = *connsecmark;
	}
}

static unsigned int target(struct sk_buff **pskb, const struct net_device *in,
			   const struct net_device *out, unsigned int hooknum,
			   const struct xt_target *target,
			   const void *targinfo)
{
	struct sk_buff *skb = *pskb;
	const struct xt_connsecmark_target_info *info = targinfo;

	switch (info->mode) {
	case CONNSECMARK_SAVE:
		secmark_save(skb);
		break;

	case CONNSECMARK_RESTORE:
		secmark_restore(skb);
		break;

	default:
		BUG();
	}

	return XT_CONTINUE;
}

static int checkentry(const char *tablename, const void *entry,
		      const struct xt_target *target, void *targinfo,
		      unsigned int hook_mask)
{
	struct xt_connsecmark_target_info *info = targinfo;

	switch (info->mode) {
	case CONNSECMARK_SAVE:
	case CONNSECMARK_RESTORE:
		break;

	default:
		printk(KERN_INFO PFX "invalid mode: %hu\n", info->mode);
		return 0;
	}

	return 1;
}

static struct xt_target xt_connsecmark_target[] = {
	{
		.name		= "CONNSECMARK",
		.family		= AF_INET,
		.checkentry	= checkentry,
		.target		= target,
		.targetsize	= sizeof(struct xt_connsecmark_target_info),
		.table		= "mangle",
		.me		= THIS_MODULE,
	},
	{
		.name		= "CONNSECMARK",
		.family		= AF_INET6,
		.checkentry	= checkentry,
		.target		= target,
		.targetsize	= sizeof(struct xt_connsecmark_target_info),
		.table		= "mangle",
		.me		= THIS_MODULE,
	},
};

static int __init xt_connsecmark_init(void)
{
	need_conntrack();
	return xt_register_targets(xt_connsecmark_target,
				   ARRAY_SIZE(xt_connsecmark_target));
}

static void __exit xt_connsecmark_fini(void)
{
	xt_unregister_targets(xt_connsecmark_target,
			      ARRAY_SIZE(xt_connsecmark_target));
}

module_init(xt_connsecmark_init);
module_exit(xt_connsecmark_fini);
