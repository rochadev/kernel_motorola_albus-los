/*
 * AMD Cryptographic Coprocessor (CCP) SHA crypto API support
 *
 * Copyright (C) 2013 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <linux/crypto.h>
#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/sha.h>
#include <crypto/scatterwalk.h>

#include "ccp-crypto.h"


struct ccp_sha_result {
	struct completion completion;
	int err;
};

static void ccp_sync_hash_complete(struct crypto_async_request *req, int err)
{
	struct ccp_sha_result *result = req->data;

	if (err == -EINPROGRESS)
		return;

	result->err = err;
	complete(&result->completion);
}

static int ccp_sync_hash(struct crypto_ahash *tfm, u8 *buf,
			 struct scatterlist *sg, unsigned int len)
{
	struct ccp_sha_result result;
	struct ahash_request *req;
	int ret;

	init_completion(&result.completion);

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   ccp_sync_hash_complete, &result);
	ahash_request_set_crypt(req, sg, buf, len);

	ret = crypto_ahash_digest(req);
	if ((ret == -EINPROGRESS) || (ret == -EBUSY)) {
		ret = wait_for_completion_interruptible(&result.completion);
		if (!ret)
			ret = result.err;
	}

	ahash_request_free(req);

	return ret;
}

static int ccp_sha_finish_hmac(struct crypto_async_request *async_req)
{
	struct ahash_request *req = ahash_request_cast(async_req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ccp_ctx *ctx = crypto_ahash_ctx(tfm);
	struct scatterlist sg[2];
	unsigned int block_size =
		crypto_tfm_alg_blocksize(crypto_ahash_tfm(tfm));
	unsigned int digest_size = crypto_ahash_digestsize(tfm);

	sg_init_table(sg, ARRAY_SIZE(sg));
	sg_set_buf(&sg[0], ctx->u.sha.opad, block_size);
	sg_set_buf(&sg[1], req->result, digest_size);

	return ccp_sync_hash(ctx->u.sha.hmac_tfm, req->result, sg,
			     block_size + digest_size);
}

static int ccp_sha_complete(struct crypto_async_request *async_req, int ret)
{
	struct ahash_request *req = ahash_request_cast(async_req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ccp_ctx *ctx = crypto_ahash_ctx(tfm);
	struct ccp_sha_req_ctx *rctx = ahash_request_ctx(req);
	unsigned int digest_size = crypto_ahash_digestsize(tfm);

	if (ret)
		goto e_free;

	if (rctx->hash_rem) {
		/* Save remaining data to buffer */
		scatterwalk_map_and_copy(rctx->buf, rctx->cmd.u.sha.src,
					 rctx->hash_cnt, rctx->hash_rem, 0);
		rctx->buf_count = rctx->hash_rem;
	} else
		rctx->buf_count = 0;

	memcpy(req->result, rctx->ctx, digest_size);

	/* If we're doing an HMAC, we need to perform that on the final op */
	if (rctx->final && ctx->u.sha.key_len)
		ret = ccp_sha_finish_hmac(async_req);

e_free:
	sg_free_table(&rctx->data_sg);

	return ret;
}

static int ccp_do_sha_update(struct ahash_request *req, unsigned int nbytes,
			     unsigned int final)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ccp_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct scatterlist *sg;
	unsigned int block_size =
		crypto_tfm_alg_blocksize(crypto_ahash_tfm(tfm));
	unsigned int len, sg_count;
	gfp_t gfp;
	int ret;

	if (!final && ((nbytes + rctx->buf_count) <= block_size)) {
		scatterwalk_map_and_copy(rctx->buf + rctx->buf_count, req->src,
					 0, nbytes, 0);
		rctx->buf_count += nbytes;

		return 0;
	}

	len = rctx->buf_count + nbytes;

	rctx->final = final;
	rctx->hash_cnt = final ? len : len & ~(block_size - 1);
	rctx->hash_rem = final ?   0 : len &  (block_size - 1);
	if (!final && (rctx->hash_cnt == len)) {
		/* CCP can't do zero length final, so keep some data around */
		rctx->hash_cnt -= block_size;
		rctx->hash_rem = block_size;
	}

	/* Initialize the context scatterlist */
	sg_init_one(&rctx->ctx_sg, rctx->ctx, sizeof(rctx->ctx));

	sg = NULL;
	if (rctx->buf_count && nbytes) {
		/* Build the data scatterlist table - allocate enough entries
		 * for both data pieces (buffer and input data)
		 */
		gfp = req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP ?
			GFP_KERNEL : GFP_ATOMIC;
		sg_count = sg_nents(req->src) + 1;
		ret = sg_alloc_table(&rctx->data_sg, sg_count, gfp);
		if (ret)
			return ret;

		sg_init_one(&rctx->buf_sg, rctx->buf, rctx->buf_count);
		sg = ccp_crypto_sg_table_add(&rctx->data_sg, &rctx->buf_sg);
		sg = ccp_crypto_sg_table_add(&rctx->data_sg, req->src);
		sg_mark_end(sg);

		sg = rctx->data_sg.sgl;
	} else if (rctx->buf_count) {
		sg_init_one(&rctx->buf_sg, rctx->buf, rctx->buf_count);

		sg = &rctx->buf_sg;
	} else if (nbytes) {
		sg = req->src;
	}

	rctx->msg_bits += (rctx->hash_cnt << 3);	/* Total in bits */

	memset(&rctx->cmd, 0, sizeof(rctx->cmd));
	INIT_LIST_HEAD(&rctx->cmd.entry);
	rctx->cmd.engine = CCP_ENGINE_SHA;
	rctx->cmd.u.sha.type = rctx->type;
	rctx->cmd.u.sha.ctx = &rctx->ctx_sg;
	rctx->cmd.u.sha.ctx_len = sizeof(rctx->ctx);
	rctx->cmd.u.sha.src = sg;
	rctx->cmd.u.sha.src_len = rctx->hash_cnt;
	rctx->cmd.u.sha.final = rctx->final;
	rctx->cmd.u.sha.msg_bits = rctx->msg_bits;

	rctx->first = 0;

	ret = ccp_crypto_enqueue_request(&req->base, &rctx->cmd);

	return ret;
}

static int ccp_sha_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ccp_ctx *ctx = crypto_ahash_ctx(tfm);
	struct ccp_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct ccp_crypto_ahash_alg *alg =
		ccp_crypto_ahash_alg(crypto_ahash_tfm(tfm));
	unsigned int block_size =
		crypto_tfm_alg_blocksize(crypto_ahash_tfm(tfm));

	memset(rctx, 0, sizeof(*rctx));

	memcpy(rctx->ctx, alg->init, sizeof(rctx->ctx));
	rctx->type = alg->type;
	rctx->first = 1;

	if (ctx->u.sha.key_len) {
		/* Buffer the HMAC key for first update */
		memcpy(rctx->buf, ctx->u.sha.ipad, block_size);
		rctx->buf_count = block_size;
	}

	return 0;
}

static int ccp_sha_update(struct ahash_request *req)
{
	return ccp_do_sha_update(req, req->nbytes, 0);
}

static int ccp_sha_final(struct ahash_request *req)
{
	return ccp_do_sha_update(req, 0, 1);
}

static int ccp_sha_finup(struct ahash_request *req)
{
	return ccp_do_sha_update(req, req->nbytes, 1);
}

static int ccp_sha_digest(struct ahash_request *req)
{
	ccp_sha_init(req);

	return ccp_do_sha_update(req, req->nbytes, 1);
}

static int ccp_sha_setkey(struct crypto_ahash *tfm, const u8 *key,
			  unsigned int key_len)
{
	struct ccp_ctx *ctx = crypto_tfm_ctx(crypto_ahash_tfm(tfm));
	struct scatterlist sg;
	unsigned int block_size =
		crypto_tfm_alg_blocksize(crypto_ahash_tfm(tfm));
	unsigned int digest_size = crypto_ahash_digestsize(tfm);
	int i, ret;

	/* Set to zero until complete */
	ctx->u.sha.key_len = 0;

	/* Clear key area to provide zero padding for keys smaller
	 * than the block size
	 */
	memset(ctx->u.sha.key, 0, sizeof(ctx->u.sha.key));

	if (key_len > block_size) {
		/* Must hash the input key */
		sg_init_one(&sg, key, key_len);
		ret = ccp_sync_hash(tfm, ctx->u.sha.key, &sg, key_len);
		if (ret) {
			crypto_ahash_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
			return -EINVAL;
		}

		key_len = digest_size;
	} else
		memcpy(ctx->u.sha.key, key, key_len);

	for (i = 0; i < block_size; i++) {
		ctx->u.sha.ipad[i] = ctx->u.sha.key[i] ^ 0x36;
		ctx->u.sha.opad[i] = ctx->u.sha.key[i] ^ 0x5c;
	}

	ctx->u.sha.key_len = key_len;

	return 0;
}

static int ccp_sha_cra_init(struct crypto_tfm *tfm)
{
	struct ccp_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_ahash *ahash = __crypto_ahash_cast(tfm);

	ctx->complete = ccp_sha_complete;
	ctx->u.sha.key_len = 0;

	crypto_ahash_set_reqsize(ahash, sizeof(struct ccp_sha_req_ctx));

	return 0;
}

static void ccp_sha_cra_exit(struct crypto_tfm *tfm)
{
}

static int ccp_hmac_sha_cra_init(struct crypto_tfm *tfm)
{
	struct ccp_ctx *ctx = crypto_tfm_ctx(tfm);
	struct ccp_crypto_ahash_alg *alg = ccp_crypto_ahash_alg(tfm);
	struct crypto_ahash *hmac_tfm;

	hmac_tfm = crypto_alloc_ahash(alg->child_alg,
				      CRYPTO_ALG_TYPE_AHASH, 0);
	if (IS_ERR(hmac_tfm)) {
		pr_warn("could not load driver %s need for HMAC support\n",
			alg->child_alg);
		return PTR_ERR(hmac_tfm);
	}

	ctx->u.sha.hmac_tfm = hmac_tfm;

	return ccp_sha_cra_init(tfm);
}

static void ccp_hmac_sha_cra_exit(struct crypto_tfm *tfm)
{
	struct ccp_ctx *ctx = crypto_tfm_ctx(tfm);

	if (ctx->u.sha.hmac_tfm)
		crypto_free_ahash(ctx->u.sha.hmac_tfm);

	ccp_sha_cra_exit(tfm);
}

static const __be32 sha1_init[CCP_SHA_CTXSIZE / sizeof(__be32)] = {
	cpu_to_be32(SHA1_H0), cpu_to_be32(SHA1_H1),
	cpu_to_be32(SHA1_H2), cpu_to_be32(SHA1_H3),
	cpu_to_be32(SHA1_H4), 0, 0, 0,
};

static const __be32 sha224_init[CCP_SHA_CTXSIZE / sizeof(__be32)] = {
	cpu_to_be32(SHA224_H0), cpu_to_be32(SHA224_H1),
	cpu_to_be32(SHA224_H2), cpu_to_be32(SHA224_H3),
	cpu_to_be32(SHA224_H4), cpu_to_be32(SHA224_H5),
	cpu_to_be32(SHA224_H6), cpu_to_be32(SHA224_H7),
};

static const __be32 sha256_init[CCP_SHA_CTXSIZE / sizeof(__be32)] = {
	cpu_to_be32(SHA256_H0), cpu_to_be32(SHA256_H1),
	cpu_to_be32(SHA256_H2), cpu_to_be32(SHA256_H3),
	cpu_to_be32(SHA256_H4), cpu_to_be32(SHA256_H5),
	cpu_to_be32(SHA256_H6), cpu_to_be32(SHA256_H7),
};

struct ccp_sha_def {
	const char *name;
	const char *drv_name;
	const __be32 *init;
	enum ccp_sha_type type;
	u32 digest_size;
	u32 block_size;
};

static struct ccp_sha_def sha_algs[] = {
	{
		.name		= "sha1",
		.drv_name	= "sha1-ccp",
		.init		= sha1_init,
		.type		= CCP_SHA_TYPE_1,
		.digest_size	= SHA1_DIGEST_SIZE,
		.block_size	= SHA1_BLOCK_SIZE,
	},
	{
		.name		= "sha224",
		.drv_name	= "sha224-ccp",
		.init		= sha224_init,
		.type		= CCP_SHA_TYPE_224,
		.digest_size	= SHA224_DIGEST_SIZE,
		.block_size	= SHA224_BLOCK_SIZE,
	},
	{
		.name		= "sha256",
		.drv_name	= "sha256-ccp",
		.init		= sha256_init,
		.type		= CCP_SHA_TYPE_256,
		.digest_size	= SHA256_DIGEST_SIZE,
		.block_size	= SHA256_BLOCK_SIZE,
	},
};

static int ccp_register_hmac_alg(struct list_head *head,
				 const struct ccp_sha_def *def,
				 const struct ccp_crypto_ahash_alg *base_alg)
{
	struct ccp_crypto_ahash_alg *ccp_alg;
	struct ahash_alg *alg;
	struct hash_alg_common *halg;
	struct crypto_alg *base;
	int ret;

	ccp_alg = kzalloc(sizeof(*ccp_alg), GFP_KERNEL);
	if (!ccp_alg)
		return -ENOMEM;

	/* Copy the base algorithm and only change what's necessary */
	*ccp_alg = *base_alg;
	INIT_LIST_HEAD(&ccp_alg->entry);

	strncpy(ccp_alg->child_alg, def->name, CRYPTO_MAX_ALG_NAME);

	alg = &ccp_alg->alg;
	alg->setkey = ccp_sha_setkey;

	halg = &alg->halg;

	base = &halg->base;
	snprintf(base->cra_name, CRYPTO_MAX_ALG_NAME, "hmac(%s)", def->name);
	snprintf(base->cra_driver_name, CRYPTO_MAX_ALG_NAME, "hmac-%s",
		 def->drv_name);
	base->cra_init = ccp_hmac_sha_cra_init;
	base->cra_exit = ccp_hmac_sha_cra_exit;

	ret = crypto_register_ahash(alg);
	if (ret) {
		pr_err("%s ahash algorithm registration error (%d)\n",
			base->cra_name, ret);
		kfree(ccp_alg);
		return ret;
	}

	list_add(&ccp_alg->entry, head);

	return ret;
}

static int ccp_register_sha_alg(struct list_head *head,
				const struct ccp_sha_def *def)
{
	struct ccp_crypto_ahash_alg *ccp_alg;
	struct ahash_alg *alg;
	struct hash_alg_common *halg;
	struct crypto_alg *base;
	int ret;

	ccp_alg = kzalloc(sizeof(*ccp_alg), GFP_KERNEL);
	if (!ccp_alg)
		return -ENOMEM;

	INIT_LIST_HEAD(&ccp_alg->entry);

	ccp_alg->init = def->init;
	ccp_alg->type = def->type;

	alg = &ccp_alg->alg;
	alg->init = ccp_sha_init;
	alg->update = ccp_sha_update;
	alg->final = ccp_sha_final;
	alg->finup = ccp_sha_finup;
	alg->digest = ccp_sha_digest;

	halg = &alg->halg;
	halg->digestsize = def->digest_size;

	base = &halg->base;
	snprintf(base->cra_name, CRYPTO_MAX_ALG_NAME, "%s", def->name);
	snprintf(base->cra_driver_name, CRYPTO_MAX_ALG_NAME, "%s",
		 def->drv_name);
	base->cra_flags = CRYPTO_ALG_TYPE_AHASH | CRYPTO_ALG_ASYNC |
			  CRYPTO_ALG_KERN_DRIVER_ONLY |
			  CRYPTO_ALG_NEED_FALLBACK;
	base->cra_blocksize = def->block_size;
	base->cra_ctxsize = sizeof(struct ccp_ctx);
	base->cra_priority = CCP_CRA_PRIORITY;
	base->cra_type = &crypto_ahash_type;
	base->cra_init = ccp_sha_cra_init;
	base->cra_exit = ccp_sha_cra_exit;
	base->cra_module = THIS_MODULE;

	ret = crypto_register_ahash(alg);
	if (ret) {
		pr_err("%s ahash algorithm registration error (%d)\n",
			base->cra_name, ret);
		kfree(ccp_alg);
		return ret;
	}

	list_add(&ccp_alg->entry, head);

	ret = ccp_register_hmac_alg(head, def, ccp_alg);

	return ret;
}

int ccp_register_sha_algs(struct list_head *head)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(sha_algs); i++) {
		ret = ccp_register_sha_alg(head, &sha_algs[i]);
		if (ret)
			return ret;
	}

	return 0;
}
