/*
 * Cryptographic API for algorithms (i.e., low-level API).
 *
 * Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>

#include "internal.h"

static LIST_HEAD(crypto_template_list);

void crypto_larval_error(const char *name, u32 type, u32 mask)
{
	struct crypto_alg *alg;

	down_read(&crypto_alg_sem);
	alg = __crypto_alg_lookup(name, type, mask);
	up_read(&crypto_alg_sem);

	if (alg) {
		if (crypto_is_larval(alg)) {
			struct crypto_larval *larval = (void *)alg;
			complete(&larval->completion);
		}
		crypto_mod_put(alg);
	}
}
EXPORT_SYMBOL_GPL(crypto_larval_error);

static inline int crypto_set_driver_name(struct crypto_alg *alg)
{
	static const char suffix[] = "-generic";
	char *driver_name = alg->cra_driver_name;
	int len;

	if (*driver_name)
		return 0;

	len = strlcpy(driver_name, alg->cra_name, CRYPTO_MAX_ALG_NAME);
	if (len + sizeof(suffix) > CRYPTO_MAX_ALG_NAME)
		return -ENAMETOOLONG;

	memcpy(driver_name + len, suffix, sizeof(suffix));
	return 0;
}

static int crypto_check_alg(struct crypto_alg *alg)
{
	if (alg->cra_alignmask & (alg->cra_alignmask + 1))
		return -EINVAL;

	if (alg->cra_alignmask & alg->cra_blocksize)
		return -EINVAL;

	if (alg->cra_blocksize > PAGE_SIZE / 8)
		return -EINVAL;

	if (alg->cra_priority < 0)
		return -EINVAL;

	return crypto_set_driver_name(alg);
}

static void crypto_destroy_instance(struct crypto_alg *alg)
{
	struct crypto_instance *inst = (void *)alg;
	struct crypto_template *tmpl = inst->tmpl;

	tmpl->free(inst);
	crypto_tmpl_put(tmpl);
}

static void crypto_remove_spawns(struct list_head *spawns,
				 struct list_head *list)
{
	struct crypto_spawn *spawn, *n;

	list_for_each_entry_safe(spawn, n, spawns, list) {
		struct crypto_instance *inst = spawn->inst;
		struct crypto_template *tmpl = inst->tmpl;

		list_del_init(&spawn->list);
		spawn->alg = NULL;

		if (crypto_is_dead(&inst->alg))
			continue;

		inst->alg.cra_flags |= CRYPTO_ALG_DEAD;
		if (!tmpl || !crypto_tmpl_get(tmpl))
			continue;

		crypto_notify(CRYPTO_MSG_ALG_UNREGISTER, &inst->alg);
		list_move(&inst->alg.cra_list, list);
		hlist_del(&inst->list);
		inst->alg.cra_destroy = crypto_destroy_instance;

		if (!list_empty(&inst->alg.cra_users)) {
			if (&n->list == spawns)
				n = list_entry(inst->alg.cra_users.next,
					       typeof(*n), list);
			__list_splice(&inst->alg.cra_users, spawns->prev);
		}
	}
}

static int __crypto_register_alg(struct crypto_alg *alg,
				 struct list_head *list)
{
	struct crypto_alg *q;
	int ret = -EAGAIN;

	if (crypto_is_dead(alg))
		goto out;

	INIT_LIST_HEAD(&alg->cra_users);

	ret = -EEXIST;

	atomic_set(&alg->cra_refcnt, 1);
	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		if (q == alg)
			goto out;

		if (crypto_is_moribund(q))
			continue;

		if (crypto_is_larval(q)) {
			struct crypto_larval *larval = (void *)q;

			if (strcmp(alg->cra_name, q->cra_name) &&
			    strcmp(alg->cra_driver_name, q->cra_name))
				continue;

			if (larval->adult)
				continue;
			if ((q->cra_flags ^ alg->cra_flags) & larval->mask)
				continue;
			if (!crypto_mod_get(alg))
				continue;

			larval->adult = alg;
			complete(&larval->completion);
			continue;
		}

		if (strcmp(alg->cra_name, q->cra_name))
			continue;

		if (strcmp(alg->cra_driver_name, q->cra_driver_name) &&
		    q->cra_priority > alg->cra_priority)
			continue;

		crypto_remove_spawns(&q->cra_users, list);
	}
	
	list_add(&alg->cra_list, &crypto_alg_list);

	crypto_notify(CRYPTO_MSG_ALG_REGISTER, alg);
	ret = 0;

out:	
	return ret;
}

static void crypto_remove_final(struct list_head *list)
{
	struct crypto_alg *alg;
	struct crypto_alg *n;

	list_for_each_entry_safe(alg, n, list, cra_list) {
		list_del_init(&alg->cra_list);
		crypto_alg_put(alg);
	}
}

int crypto_register_alg(struct crypto_alg *alg)
{
	LIST_HEAD(list);
	int err;

	err = crypto_check_alg(alg);
	if (err)
		return err;

	down_write(&crypto_alg_sem);
	err = __crypto_register_alg(alg, &list);
	up_write(&crypto_alg_sem);

	crypto_remove_final(&list);
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_alg);

static int crypto_remove_alg(struct crypto_alg *alg, struct list_head *list)
{
	if (unlikely(list_empty(&alg->cra_list)))
		return -ENOENT;

	alg->cra_flags |= CRYPTO_ALG_DEAD;

	crypto_notify(CRYPTO_MSG_ALG_UNREGISTER, alg);
	list_del_init(&alg->cra_list);
	crypto_remove_spawns(&alg->cra_users, list);

	return 0;
}

int crypto_unregister_alg(struct crypto_alg *alg)
{
	int ret;
	LIST_HEAD(list);
	
	down_write(&crypto_alg_sem);
	ret = crypto_remove_alg(alg, &list);
	up_write(&crypto_alg_sem);

	if (ret)
		return ret;

	BUG_ON(atomic_read(&alg->cra_refcnt) != 1);
	if (alg->cra_destroy)
		alg->cra_destroy(alg);

	crypto_remove_final(&list);
	return 0;
}
EXPORT_SYMBOL_GPL(crypto_unregister_alg);

int crypto_register_template(struct crypto_template *tmpl)
{
	struct crypto_template *q;
	int err = -EEXIST;

	down_write(&crypto_alg_sem);

	list_for_each_entry(q, &crypto_template_list, list) {
		if (q == tmpl)
			goto out;
	}

	list_add(&tmpl->list, &crypto_template_list);
	crypto_notify(CRYPTO_MSG_TMPL_REGISTER, tmpl);
	err = 0;
out:
	up_write(&crypto_alg_sem);
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_template);

void crypto_unregister_template(struct crypto_template *tmpl)
{
	struct crypto_instance *inst;
	struct hlist_node *p, *n;
	struct hlist_head *list;
	LIST_HEAD(users);

	down_write(&crypto_alg_sem);

	BUG_ON(list_empty(&tmpl->list));
	list_del_init(&tmpl->list);

	list = &tmpl->instances;
	hlist_for_each_entry(inst, p, list, list) {
		int err = crypto_remove_alg(&inst->alg, &users);
		BUG_ON(err);
	}

	crypto_notify(CRYPTO_MSG_TMPL_UNREGISTER, tmpl);

	up_write(&crypto_alg_sem);

	hlist_for_each_entry_safe(inst, p, n, list, list) {
		BUG_ON(atomic_read(&inst->alg.cra_refcnt) != 1);
		tmpl->free(inst);
	}
	crypto_remove_final(&users);
}
EXPORT_SYMBOL_GPL(crypto_unregister_template);

static struct crypto_template *__crypto_lookup_template(const char *name)
{
	struct crypto_template *q, *tmpl = NULL;

	down_read(&crypto_alg_sem);
	list_for_each_entry(q, &crypto_template_list, list) {
		if (strcmp(q->name, name))
			continue;
		if (unlikely(!crypto_tmpl_get(q)))
			continue;

		tmpl = q;
		break;
	}
	up_read(&crypto_alg_sem);

	return tmpl;
}

struct crypto_template *crypto_lookup_template(const char *name)
{
	return try_then_request_module(__crypto_lookup_template(name), name);
}
EXPORT_SYMBOL_GPL(crypto_lookup_template);

int crypto_register_instance(struct crypto_template *tmpl,
			     struct crypto_instance *inst)
{
	LIST_HEAD(list);
	int err = -EINVAL;

	if (inst->alg.cra_destroy)
		goto err;

	err = crypto_check_alg(&inst->alg);
	if (err)
		goto err;

	inst->alg.cra_module = tmpl->module;

	down_write(&crypto_alg_sem);

	err = __crypto_register_alg(&inst->alg, &list);
	if (err)
		goto unlock;

	hlist_add_head(&inst->list, &tmpl->instances);
	inst->tmpl = tmpl;

unlock:
	up_write(&crypto_alg_sem);

	crypto_remove_final(&list);

err:
	return err;
}
EXPORT_SYMBOL_GPL(crypto_register_instance);

int crypto_init_spawn(struct crypto_spawn *spawn, struct crypto_alg *alg,
		      struct crypto_instance *inst)
{
	int err = -EAGAIN;

	spawn->inst = inst;

	down_write(&crypto_alg_sem);
	if (!crypto_is_moribund(alg)) {
		list_add(&spawn->list, &alg->cra_users);
		spawn->alg = alg;
		err = 0;
	}
	up_write(&crypto_alg_sem);

	return err;
}
EXPORT_SYMBOL_GPL(crypto_init_spawn);

void crypto_drop_spawn(struct crypto_spawn *spawn)
{
	down_write(&crypto_alg_sem);
	list_del(&spawn->list);
	up_write(&crypto_alg_sem);
}
EXPORT_SYMBOL_GPL(crypto_drop_spawn);

struct crypto_tfm *crypto_spawn_tfm(struct crypto_spawn *spawn)
{
	struct crypto_alg *alg;
	struct crypto_alg *alg2;
	struct crypto_tfm *tfm;

	down_read(&crypto_alg_sem);
	alg = spawn->alg;
	alg2 = alg;
	if (alg2)
		alg2 = crypto_mod_get(alg2);
	up_read(&crypto_alg_sem);

	if (!alg2) {
		if (alg)
			crypto_shoot_alg(alg);
		return ERR_PTR(-EAGAIN);
	}

	tfm = __crypto_alloc_tfm(alg, 0);
	if (IS_ERR(tfm))
		crypto_mod_put(alg);

	return tfm;
}
EXPORT_SYMBOL_GPL(crypto_spawn_tfm);

int crypto_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&crypto_chain, nb);
}
EXPORT_SYMBOL_GPL(crypto_register_notifier);

int crypto_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&crypto_chain, nb);
}
EXPORT_SYMBOL_GPL(crypto_unregister_notifier);

struct crypto_alg *crypto_get_attr_alg(void *param, unsigned int len,
				       u32 type, u32 mask)
{
	struct rtattr *rta = param;
	struct crypto_attr_alg *alga;

	if (!RTA_OK(rta, len))
		return ERR_PTR(-EBADR);
	if (rta->rta_type != CRYPTOA_ALG || RTA_PAYLOAD(rta) < sizeof(*alga))
		return ERR_PTR(-EINVAL);

	alga = RTA_DATA(rta);
	alga->name[CRYPTO_MAX_ALG_NAME - 1] = 0;

	return crypto_alg_mod_lookup(alga->name, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_get_attr_alg);

struct crypto_instance *crypto_alloc_instance(const char *name,
					      struct crypto_alg *alg)
{
	struct crypto_instance *inst;
	struct crypto_spawn *spawn;
	int err;

	inst = kzalloc(sizeof(*inst) + sizeof(*spawn), GFP_KERNEL);
	if (!inst)
		return ERR_PTR(-ENOMEM);

	err = -ENAMETOOLONG;
	if (snprintf(inst->alg.cra_name, CRYPTO_MAX_ALG_NAME, "%s(%s)", name,
		     alg->cra_name) >= CRYPTO_MAX_ALG_NAME)
		goto err_free_inst;

	if (snprintf(inst->alg.cra_driver_name, CRYPTO_MAX_ALG_NAME, "%s(%s)",
		     name, alg->cra_driver_name) >= CRYPTO_MAX_ALG_NAME)
		goto err_free_inst;

	spawn = crypto_instance_ctx(inst);
	err = crypto_init_spawn(spawn, alg, inst);

	if (err)
		goto err_free_inst;

	return inst;

err_free_inst:
	kfree(inst);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(crypto_alloc_instance);

static int __init crypto_algapi_init(void)
{
	crypto_init_proc();
	return 0;
}

static void __exit crypto_algapi_exit(void)
{
	crypto_exit_proc();
}

module_init(crypto_algapi_init);
module_exit(crypto_algapi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cryptographic algorithms API");
