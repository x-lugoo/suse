/* Module signature checker
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <crypto/public_key.h>
#include <crypto/hash.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include <crypto/public_key.h>
#include "module-internal.h"

LIST_HEAD(module_hash_blacklist);

/*
 * Module signature information block.
 *
 * The constituents of the signature section are, in order:
 *
 *	- Signer's name
 *	- Key identifier
 *	- Signature data
 *	- Information block
 */
struct module_signature {
	u8	algo;		/* Public-key crypto algorithm [0|enum pkey_algo] */
	u8	hash;		/* Digest algorithm [0|enum hash_algo] */
	u8	id_type;	/* Key identifier type [PKEY_ID_PKCS7|PKEY_ID_X509] */
	u8	signer_len;	/* Length of signer's name (PKEY_ID_X509 only) */
	u8	key_id_len;	/* Length of key identifier (PKEY_ID_X509 only) */
	u8	__pad[3];
	__be32	sig_len;	/* Length of signature data */
};

static int old_mod_verify_sig(const void *mod, unsigned long *_modlen,
                       const struct module_signature *ms);

static int mod_verify_hash(const void *mod, unsigned long modlen,
			struct public_key_signature *pks);

static int new_check_blacklist(const void *mod, size_t wholelen,
				size_t modlen);

/*
 * Verify the signature on a module.
 */
int mod_verify_sig(const void *mod, unsigned long *_modlen)
{
	struct module_signature ms;
	size_t modlen = *_modlen, sig_len, wholelen;
	int ret;

	pr_devel("==>%s(,%zu)\n", __func__, modlen);

	if (modlen <= sizeof(ms))
		return -EBADMSG;

	wholelen = modlen + sizeof(MODULE_SIG_STRING) - 1;
	memcpy(&ms, mod + (modlen - sizeof(ms)), sizeof(ms));
	modlen -= sizeof(ms);
	*_modlen = modlen;

	switch (ms.id_type) {
	case PKEY_ID_PKCS7:
		break;
	case PKEY_ID_X509:
		return old_mod_verify_sig(mod, _modlen, &ms);
	default:
		pr_err("Module is not signed with expected signature type\n");
		return -ENOPKG;
	}

	sig_len = be32_to_cpu(ms.sig_len);
	if (sig_len >= modlen)
		return -EBADMSG;
	modlen -= sig_len;
	*_modlen = modlen;

	if (ms.algo != 0 ||
	    ms.hash != 0 ||
	    ms.signer_len != 0 ||
	    ms.key_id_len != 0 ||
	    ms.__pad[0] != 0 ||
	    ms.__pad[1] != 0 ||
	    ms.__pad[2] != 0) {
		pr_err("PKCS#7 signature info has unexpected non-zero params\n");
		return -EBADMSG;
	}

	ret = system_verify_data(mod, modlen, mod + modlen, sig_len,
				  VERIFYING_MODULE_SIGNATURE);
	pr_devel("system_verify_data() = %d\n", ret);

	/* check hash of module not in blacklist */
	if (!ret)
		ret = new_check_blacklist(mod, wholelen, modlen);

	return ret;
}

/*
 * pre-4.3 compat stuff
 */

/*
 * Digest the module contents.
 */
static struct public_key_signature *mod_make_digest(enum hash_algo hash,
						    const void *mod,
						    unsigned long modlen)
{
	struct public_key_signature *pks;
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	size_t digest_size, desc_size;
	int ret;

	pr_devel("==>%s()\n", __func__);
	
	/* Allocate the hashing algorithm we're going to need and find out how
	 * big the hash operational data will be.
	 */
	tfm = crypto_alloc_shash(hash_algo_name[hash], 0, 0);
	if (IS_ERR(tfm))
		return (PTR_ERR(tfm) == -ENOENT) ? ERR_PTR(-ENOPKG) : ERR_CAST(tfm);

	desc_size = crypto_shash_descsize(tfm) + sizeof(*desc);
	digest_size = crypto_shash_digestsize(tfm);

	/* We allocate the hash operational data storage on the end of our
	 * context data and the digest output buffer on the end of that.
	 */
	ret = -ENOMEM;
	pks = kzalloc(digest_size + sizeof(*pks) + desc_size, GFP_KERNEL);
	if (!pks)
		goto error_no_pks;

	pks->pkey_hash_algo	= hash;
	pks->digest		= (u8 *)pks + sizeof(*pks) + desc_size;
	pks->digest_size	= digest_size;

	desc = (void *)pks + sizeof(*pks);
	desc->tfm   = tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;

	ret = crypto_shash_init(desc);
	if (ret < 0)
		goto error;

	ret = crypto_shash_finup(desc, mod, modlen, pks->digest);
	if (ret < 0)
		goto error;

	crypto_free_shash(tfm);
	pr_devel("<==%s() = ok\n", __func__);
	return pks;

error:
	kfree(pks);
error_no_pks:
	crypto_free_shash(tfm);
	pr_devel("<==%s() = %d\n", __func__, ret);
	return ERR_PTR(ret);
}

/*
 * Extract an MPI array from the signature data.  This represents the actual
 * signature.  Each raw MPI is prefaced by a BE 2-byte value indicating the
 * size of the MPI in bytes.
 *
 * RSA signatures only have one MPI, so currently we only read one.
 */
static int mod_extract_mpi_array(struct public_key_signature *pks,
				 const void *data, size_t len)
{
	size_t nbytes;
	MPI mpi;

	if (len < 3)
		return -EBADMSG;
	nbytes = ((const u8 *)data)[0] << 8 | ((const u8 *)data)[1];
	data += 2;
	len -= 2;
	if (len != nbytes)
		return -EBADMSG;

	mpi = mpi_read_raw_data(data, nbytes);
	if (!mpi)
		return -ENOMEM;
	pks->mpi[0] = mpi;
	pks->nr_mpi = 1;
	return 0;
}

/*
 * Request an asymmetric key.
 */
static struct key *request_asymmetric_key(const char *signer, size_t signer_len,
					  const u8 *key_id, size_t key_id_len)
{
	key_ref_t key;
	size_t i;
	char *id, *q;

	pr_devel("==>%s(,%zu,,%zu)\n", __func__, signer_len, key_id_len);

	/* Construct an identifier. */
	id = kmalloc(signer_len + 2 + key_id_len * 2 + 1, GFP_KERNEL);
	if (!id)
		return ERR_PTR(-ENOKEY);

	memcpy(id, signer, signer_len);

	q = id + signer_len;
	*q++ = ':';
	*q++ = ' ';
	for (i = 0; i < key_id_len; i++) {
		*q++ = hex_asc[*key_id >> 4];
		*q++ = hex_asc[*key_id++ & 0x0f];
	}

	*q = 0;

	pr_debug("Look up: \"%s\"\n", id);

#ifdef CONFIG_SYSTEM_BLACKLIST_KEYRING
	key = keyring_search(make_key_ref(system_blacklist_keyring, 1),
				   &key_type_asymmetric, id);
	if (!IS_ERR(key)) {
		/* module is signed with a cert in the blacklist.  reject */
		pr_err("Module key '%s' is in blacklist\n", id);
		key_ref_put(key);
		kfree(id);
		return ERR_PTR(-EKEYREJECTED);
	}
#endif

	key = keyring_search(make_key_ref(system_trusted_keyring, 1),
			     &key_type_asymmetric, id);
	if (IS_ERR(key))
		pr_warn("Request for unknown module key '%s' err %ld\n",
			id, PTR_ERR(key));
	kfree(id);

	if (IS_ERR(key)) {
		switch (PTR_ERR(key)) {
			/* Hide some search errors */
		case -EACCES:
		case -ENOTDIR:
		case -EAGAIN:
			return ERR_PTR(-ENOKEY);
		default:
			return ERR_CAST(key);
		}
	}

	pr_devel("<==%s() = 0 [%x]\n", __func__, key_serial(key_ref_to_ptr(key)));
	return key_ref_to_ptr(key);
}

static int check_blacklist(const char *hash_algo_name, const void *hash)
{
	struct module_hash *module_hash;
	int ret = 0;

	list_for_each_entry(module_hash, &module_hash_blacklist, list) {
		if (strcmp(hash_algo_name, module_hash->hash_name))
			continue;

		if (!memcmp(hash, module_hash->hash_data, module_hash->size)) {
			ret = -EKEYREJECTED;
			pr_info("Module hash is in the module hash blacklist: "
				"%*phN\n", (int)module_hash->size,
				module_hash->hash_data);
			break;
		}
	}

	return ret;
}

static int mod_verify_hash(const void *mod, unsigned long modlen,
		struct public_key_signature *pks)
{
	const char *pks_hash_algo = hash_algo_name[pks->pkey_hash_algo];
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	size_t digest_size, desc_size;
	u8 *digest;
	int ret = 0;

	if (list_empty(&module_hash_blacklist))
		return 0;

	/* check digest of module is not in hash blacklist */
	ret = check_blacklist(pks_hash_algo, pks->digest);
	if (ret)
		goto error_return;

	/* check hash of whole module file */
	tfm = crypto_alloc_shash(pks_hash_algo, 0, 0);
	if (IS_ERR(tfm))
		goto error_return;

	desc_size = crypto_shash_descsize(tfm) + sizeof(*desc);
	digest_size = crypto_shash_digestsize(tfm);
	digest = kzalloc(digest_size + desc_size, GFP_KERNEL);
	if (!digest) {
		pr_err("digest memory buffer allocate fail\n");
		ret = -ENOMEM;
		goto error_digest;
	}
	desc = (void *)digest + digest_size;
	desc->tfm = tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	ret = crypto_shash_init(desc);
	if (ret < 0)
		goto error_shash;

	ret = crypto_shash_finup(desc, mod, modlen, digest);
	if (ret < 0)
		goto error_shash;

	pr_debug("%ld digest: %*phN\n", modlen, (int) digest_size, digest);

	/* check the hash of whole module file including signature */
	ret = check_blacklist(pks_hash_algo, digest);

error_shash:
	kfree(digest);
error_digest:
	crypto_free_shash(tfm);
error_return:
	return ret;
}

static int new_mod_verify_hash(const void *mod, size_t verifylen,
				struct module_hash *module_hash)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	size_t digest_size, desc_size;
	u8 *digest;
	int ret = 0;

	tfm = crypto_alloc_shash(module_hash->hash_name, 0, 0);
	if (IS_ERR(tfm))
		goto error_return;

	desc_size = crypto_shash_descsize(tfm) + sizeof(*desc);
	digest_size = crypto_shash_digestsize(tfm);
	digest = kzalloc(digest_size + desc_size, GFP_KERNEL);
	if (!digest) {
		pr_err("digest memory buffer allocate fail\n");
		ret = -ENOMEM;
		goto error_digest;
	}
	desc = (void *)digest + digest_size;
	desc->tfm = tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	ret = crypto_shash_init(desc);
	if (ret < 0)
		goto error_shash;

	ret = crypto_shash_finup(desc, mod, verifylen, digest);
	if (ret < 0)
		goto error_shash;

	pr_debug("%ld digest: %*phN\n", verifylen, (int) digest_size, digest);

	if (!memcmp(digest, module_hash->hash_data, module_hash->size)) {
		ret = -EKEYREJECTED;
		pr_info("Module hash is in the module hash blacklist: %*phN\n",
			(int)module_hash->size,
			module_hash->hash_data);
	}

error_shash:
	kfree(digest);
error_digest:
	crypto_free_shash(tfm);
error_return:
	return ret;
}

static int new_check_blacklist(const void *mod, size_t wholelen, size_t modlen)
{
	struct module_hash *module_hash;
	int ret = 0;

	if (list_empty(&module_hash_blacklist))
		return 0;

	list_for_each_entry(module_hash, &module_hash_blacklist, list) {
		/* compare hash of whole kernel module */
		ret = new_mod_verify_hash(mod, wholelen, module_hash);

		/* compare hash of kernel module without signature */
		if (!ret)
			ret = new_mod_verify_hash(mod, modlen, module_hash);
	}

	return ret;
}

/*
 * Verify the signature on a module
 */
static int old_mod_verify_sig(const void *mod, unsigned long *_modlen,
                       const struct module_signature *ms)
{
	struct public_key_signature *pks;
	struct key *key;
	const void *sig;
	size_t modlen = *_modlen, sig_len, wholelen;
	int ret;

	pr_devel("==>%s(,%zu)\n", __func__, modlen);

	wholelen = modlen + sizeof(struct module_signature) +
		   sizeof(MODULE_SIG_STRING) - 1;
	sig_len = be32_to_cpu(ms->sig_len);
	if (sig_len >= modlen)
		return -EBADMSG;
	modlen -= sig_len;
	if ((size_t)ms->signer_len + ms->key_id_len >= modlen)
		return -EBADMSG;
	modlen -= (size_t)ms->signer_len + ms->key_id_len;

	*_modlen = modlen;
	sig = mod + modlen;

	/* For the moment, only support RSA and X.509 identifiers */
	if (ms->algo != PKEY_ALGO_RSA ||
	    ms->id_type != PKEY_ID_X509)
		return -ENOPKG;

	if (ms->hash >= PKEY_HASH__LAST ||
	    !hash_algo_name[ms->hash])
		return -ENOPKG;

	key = request_asymmetric_key(sig, ms->signer_len,
				     sig + ms->signer_len, ms->key_id_len);
	if (IS_ERR(key))
		return PTR_ERR(key);

	pks = mod_make_digest(ms->hash, mod, modlen);
	if (IS_ERR(pks)) {
		ret = PTR_ERR(pks);
		goto error_put_key;
	}

	ret = mod_extract_mpi_array(pks, sig + ms->signer_len + ms->key_id_len,
				    sig_len);
	if (ret < 0)
		goto error_free_pks;

	ret = verify_signature(key, pks);
	pr_devel("verify_signature() = %d\n", ret);

	/* check hash of module not in blacklist */
	if (!ret)
		ret = mod_verify_hash(mod, wholelen, pks);

error_free_pks:
	mpi_free(pks->rsa.s);
	kfree(pks);
error_put_key:
	key_put(key);
	pr_devel("<==%s() = %d\n", __func__, ret);
	return ret;	
}
