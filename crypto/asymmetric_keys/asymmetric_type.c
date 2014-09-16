/* Asymmetric public-key cryptography key type
 *
 * See Documentation/security/asymmetric-keys.txt
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */
#include <keys/asymmetric-subtype.h>
#include <keys/asymmetric-parser.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include "asymmetric_keys.h"

MODULE_LICENSE("GPL");

static LIST_HEAD(asymmetric_key_parsers);
static DECLARE_RWSEM(asymmetric_key_parsers_sem);

/**
 * asymmetric_key_generate_id: Construct an asymmetric key ID
 * @val_1: First binary blob
 * @len_1: Length of first binary blob
 * @val_2: Second binary blob
 * @len_2: Length of second binary blob
 *
 * Construct an asymmetric key ID from a pair of binary blobs.
 */
struct asymmetric_key_id *asymmetric_key_generate_id(const void *val_1,
						     size_t len_1,
						     const void *val_2,
						     size_t len_2)
{
	struct asymmetric_key_id *kid;

	kid = kmalloc(sizeof(struct asymmetric_key_id) + len_1 + len_2,
		      GFP_KERNEL);
	if (!kid)
		return ERR_PTR(-ENOMEM);
	kid->len = len_1 + len_2;
	memcpy(kid->data, val_1, len_1);
	memcpy(kid->data + len_1, val_2, len_2);
	return kid;
}
EXPORT_SYMBOL_GPL(asymmetric_key_generate_id);

/**
 * asymmetric_key_id_same - Return true if two asymmetric keys IDs are the same.
 * @kid_1, @kid_2: The key IDs to compare
 */
bool asymmetric_key_id_same(const struct asymmetric_key_id *kid1,
			    const struct asymmetric_key_id *kid2)
{
	if (!kid1 || !kid2)
		return false;
	if (kid1->len != kid2->len)
		return false;
	return memcmp(kid1->data, kid2->data, kid1->len) == 0;
}
EXPORT_SYMBOL_GPL(asymmetric_key_id_same);

/**
 * asymmetric_match_key_ids - Search asymmetric key IDs
 * @kids: The list of key IDs to check
 * @match_id: The key ID we're looking for
 */
bool asymmetric_match_key_ids(const struct asymmetric_key_ids *kids,
			      const struct asymmetric_key_id *match_id)
{
	if (!kids || !match_id)
		return false;
	if (asymmetric_key_id_same(kids->id[0], match_id))
		return true;
	if (asymmetric_key_id_same(kids->id[1], match_id))
		return true;
	return false;
}
EXPORT_SYMBOL_GPL(asymmetric_match_key_ids);

/**
 * asymmetric_key_hex_to_key_id - Convert a hex string into a key ID.
 * @id: The ID as a hex string.
 */
struct asymmetric_key_id *asymmetric_key_hex_to_key_id(const char *id)
{
	struct asymmetric_key_id *match_id;
	const char *p;
	ptrdiff_t hexlen;

	if (!*id)
		return ERR_PTR(-EINVAL);
	for (p = id; *p; p++)
		if (!isxdigit(*p))
			return ERR_PTR(-EINVAL);
	hexlen = p - id;
	if (hexlen & 1)
		return ERR_PTR(-EINVAL);

	match_id = kmalloc(sizeof(struct asymmetric_key_id) + hexlen / 2,
			   GFP_KERNEL);
	if (!match_id)
		return ERR_PTR(-ENOMEM);
	match_id->len = hexlen / 2;
	(void)hex2bin(match_id->data, id, hexlen / 2);
	return match_id;
}

/*
 * Match asymmetric key id with partial match
 * @id:		key id to match in a form "id:<id>"
 */
int asymmetric_keyid_match(const char *kid, const char *id)
{
	size_t idlen, kidlen;

	if (!kid || !id)
		return 0;

	/* make it possible to use id as in the request: "id:<id>" */
	if (strncmp(id, "id:", 3) == 0)
		id += 3;

	/* Anything after here requires a partial match on the ID string */
	idlen = strlen(id);
	kidlen = strlen(kid);
	if (idlen > kidlen)
		return 0;

	kid += kidlen - idlen;
	if (strcasecmp(id, kid) != 0)
		return 0;

	return 1;
}
EXPORT_SYMBOL_GPL(asymmetric_keyid_match);

/*
 * Match asymmetric keys on (part of) their name
 * We have some shorthand methods for matching keys.  We allow:
 *
 *	"<desc>"	- request a key by description
 *	"id:<id>"	- request a key matching the ID
 *	"<subtype>:<id>" - request a key of a subtype
 */
static bool asymmetric_key_cmp(const struct key *key,
			       const struct key_match_data *match_data)
{
	const struct asymmetric_key_subtype *subtype = asymmetric_key_subtype(key);
	const char *description = match_data->raw_data;
	const char *spec = description;
	const char *id;
	ptrdiff_t speclen;

	if (!subtype || !spec || !*spec)
		return 0;

	/* See if the full key description matches as is */
	if (key->description && strcmp(key->description, description) == 0)
		return 1;

	/* All tests from here on break the criterion description into a
	 * specifier, a colon and then an identifier.
	 */
	id = strchr(spec, ':');
	if (!id)
		return 0;

	speclen = id - spec;
	id++;

	if (speclen == 2 && memcmp(spec, "id", 2) == 0)
		return asymmetric_keyid_match(asymmetric_key_id(key), id);

	if (speclen == subtype->name_len &&
	    memcmp(spec, subtype->name, speclen) == 0)
		return 1;

	return 0;
}

/*
 * Preparse the match criterion.  If we don't set lookup_type and cmp,
 * the default will be an exact match on the key description.
 *
 * There are some specifiers for matching key IDs rather than by the key
 * description:
 *
 *	"id:<id>" - request a key by any available ID
 *
 * These have to be searched by iteration rather than by direct lookup because
 * the key is hashed according to its description.
 */
static int asymmetric_key_match_preparse(struct key_match_data *match_data)
{
	match_data->lookup_type = KEYRING_SEARCH_LOOKUP_ITERATE;
	match_data->cmp = asymmetric_key_cmp;
	return 0;
}

/*
 * Free the preparsed the match criterion.
 */
static void asymmetric_key_match_free(struct key_match_data *match_data)
{
}

/*
 * Describe the asymmetric key
 */
static void asymmetric_key_describe(const struct key *key, struct seq_file *m)
{
	const struct asymmetric_key_subtype *subtype = asymmetric_key_subtype(key);
	const char *kid = asymmetric_key_id(key);
	size_t n;

	seq_puts(m, key->description);

	if (subtype) {
		seq_puts(m, ": ");
		subtype->describe(key, m);

		if (kid) {
			seq_putc(m, ' ');
			n = strlen(kid);
			if (n <= 8)
				seq_puts(m, kid);
			else
				seq_puts(m, kid + n - 8);
		}

		seq_puts(m, " [");
		/* put something here to indicate the key's capabilities */
		seq_putc(m, ']');
	}
}

/*
 * Preparse a asymmetric payload to get format the contents appropriately for the
 * internal payload to cut down on the number of scans of the data performed.
 *
 * We also generate a proposed description from the contents of the key that
 * can be used to name the key if the user doesn't want to provide one.
 */
static int asymmetric_key_preparse(struct key_preparsed_payload *prep)
{
	struct asymmetric_key_parser *parser;
	int ret;

	pr_devel("==>%s()\n", __func__);

	if (prep->datalen == 0)
		return -EINVAL;

	down_read(&asymmetric_key_parsers_sem);

	ret = -EBADMSG;
	list_for_each_entry(parser, &asymmetric_key_parsers, link) {
		pr_debug("Trying parser '%s'\n", parser->name);

		ret = parser->parse(prep);
		if (ret != -EBADMSG) {
			pr_debug("Parser recognised the format (ret %d)\n",
				 ret);
			break;
		}
	}

	up_read(&asymmetric_key_parsers_sem);
	pr_devel("<==%s() = %d\n", __func__, ret);
	return ret;
}

/*
 * Clean up the preparse data
 */
static void asymmetric_key_free_preparse(struct key_preparsed_payload *prep)
{
	struct asymmetric_key_subtype *subtype = prep->type_data[0];

	pr_devel("==>%s()\n", __func__);

	if (subtype) {
		subtype->destroy(prep->payload[0]);
		module_put(subtype->owner);
	}
	kfree(prep->type_data[1]);
	kfree(prep->description);
}

/*
 * dispose of the data dangling from the corpse of a asymmetric key
 */
static void asymmetric_key_destroy(struct key *key)
{
	struct asymmetric_key_subtype *subtype = asymmetric_key_subtype(key);
	if (subtype) {
		subtype->destroy(key->payload.data);
		module_put(subtype->owner);
		key->type_data.p[0] = NULL;
	}
	kfree(key->type_data.p[1]);
	key->type_data.p[1] = NULL;
}

struct key_type key_type_asymmetric = {
	.name		= "asymmetric",
	.preparse	= asymmetric_key_preparse,
	.free_preparse	= asymmetric_key_free_preparse,
	.instantiate	= generic_key_instantiate,
	.match_preparse	= asymmetric_key_match_preparse,
	.match_free	= asymmetric_key_match_free,
	.destroy	= asymmetric_key_destroy,
	.describe	= asymmetric_key_describe,
};
EXPORT_SYMBOL_GPL(key_type_asymmetric);

/**
 * register_asymmetric_key_parser - Register a asymmetric key blob parser
 * @parser: The parser to register
 */
int register_asymmetric_key_parser(struct asymmetric_key_parser *parser)
{
	struct asymmetric_key_parser *cursor;
	int ret;

	down_write(&asymmetric_key_parsers_sem);

	list_for_each_entry(cursor, &asymmetric_key_parsers, link) {
		if (strcmp(cursor->name, parser->name) == 0) {
			pr_err("Asymmetric key parser '%s' already registered\n",
			       parser->name);
			ret = -EEXIST;
			goto out;
		}
	}

	list_add_tail(&parser->link, &asymmetric_key_parsers);

	pr_notice("Asymmetric key parser '%s' registered\n", parser->name);
	ret = 0;

out:
	up_write(&asymmetric_key_parsers_sem);
	return ret;
}
EXPORT_SYMBOL_GPL(register_asymmetric_key_parser);

/**
 * unregister_asymmetric_key_parser - Unregister a asymmetric key blob parser
 * @parser: The parser to unregister
 */
void unregister_asymmetric_key_parser(struct asymmetric_key_parser *parser)
{
	down_write(&asymmetric_key_parsers_sem);
	list_del(&parser->link);
	up_write(&asymmetric_key_parsers_sem);

	pr_notice("Asymmetric key parser '%s' unregistered\n", parser->name);
}
EXPORT_SYMBOL_GPL(unregister_asymmetric_key_parser);

/*
 * Module stuff
 */
static int __init asymmetric_key_init(void)
{
	return register_key_type(&key_type_asymmetric);
}

static void __exit asymmetric_key_cleanup(void)
{
	unregister_key_type(&key_type_asymmetric);
}

module_init(asymmetric_key_init);
module_exit(asymmetric_key_cleanup);
