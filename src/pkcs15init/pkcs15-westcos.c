/*
 * pkcs15-westcos.c: pkcs15 support for westcos card
 *
 * Copyright (C) 2009 francois.leblanc@cev-sa.com 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <opensc/opensc.h>
#include <opensc/cardctl.h>
#include "pkcs15-init.h"
#include "profile.h"

#ifdef ENABLE_OPENSSL
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#endif

extern int sc_check_sw(sc_card_t *card, unsigned int sw1, unsigned int sw2);

static int westcos_pkcs15init_init_card(sc_profile_t *profile, 
						sc_card_t *card)
{
	int     r;
	struct	sc_path path;

	sc_format_path("3F00", &path);
	r = sc_select_file(card, &path, NULL);
	if(r) return (r);

	return r;
}

static int westcos_pkcs15init_create_dir(sc_profile_t *profile, 
						sc_card_t *card, 
						sc_file_t *df)
{
	int		r;

	/* Create the application DF */
	r = sc_pkcs15init_create_file(profile, card, df);

	r = sc_select_file(card, &df->path, NULL);
	if(r) return r;

	return 0;
}

/*
 * Select the PIN reference
 */
static int westcos_pkcs15_select_pin_reference(sc_profile_t *profile, 
					sc_card_t *card,
					sc_pkcs15_pin_info_t *pin_info)
{

	if (pin_info->flags & SC_PKCS15_PIN_FLAG_SO_PIN) {
		pin_info->reference = 1;
	} else {
		pin_info->reference = 0;
	}

	return 0;
}

/*
 * Create a new PIN inside a DF
 */
static int westcos_pkcs15_create_pin(sc_profile_t *profile, 
					sc_card_t *card, sc_file_t *df,
					sc_pkcs15_object_t *pin_obj,
					const u8 *pin, size_t pin_len,
					const u8 *puk, size_t puk_len)
{
	int r;
	sc_file_t *file = sc_file_new();
	sc_path_t	path;

	if(pin_len>9 || puk_len>9 || pin_len<0 || puk_len<0)
		return SC_ERROR_INVALID_ARGUMENTS;

	file->type = SC_FILE_TYPE_INTERNAL_EF;
	file->ef_structure = SC_FILE_EF_TRANSPARENT;
	file->shareable = 0;
		
	file->id = 0xAAAA;
	file->size = 37;

	r = sc_file_add_acl_entry(file, SC_AC_OP_READ, SC_AC_NONE, 0);
	if(r) return r;
	r = sc_file_add_acl_entry(file, SC_AC_OP_UPDATE, SC_AC_NONE, 0);
	if(r) return r;
	r = sc_file_add_acl_entry(file, SC_AC_OP_ERASE, SC_AC_NONE, 0);
	if(r) return r;

	r = sc_create_file(card, file);
	if(r)
	{
		if(r != SC_ERROR_FILE_ALREADY_EXISTS)
			return (r);

		sc_format_path("3F005015AAAA", &path);
		r = sc_select_file(card, &path, NULL);
		if(r) return (r);
	}

	if(file)
		sc_file_free(file);

	if(pin != NULL)
	{
		sc_changekey_t ck;
		struct sc_pin_cmd_pin pin_cmd;

		memset(&pin_cmd, 0, sizeof(pin_cmd));
		memset(&ck, 0, sizeof(ck));

		memcpy(ck.key_template, "\x1e\x00\x00\x10", 4);

		pin_cmd.encoding = SC_PIN_ENCODING_GLP;
		pin_cmd.len = pin_len;
		pin_cmd.data = pin;
		pin_cmd.max_length = 8;

		ck.new_key.key_len = sc_build_pin(ck.new_key.key_value, 
			sizeof(ck.new_key.key_value), &pin_cmd, 1); 
		if(ck.new_key.key_len<0)
			return SC_ERROR_CARD_CMD_FAILED;

		r = sc_card_ctl(card, SC_CARDCTL_WESTCOS_CHANGE_KEY, &ck);
		if(r) return r;
	}

	if(puk != NULL)
	{
		sc_changekey_t ck;
		struct sc_pin_cmd_pin puk_cmd;

		memset(&puk_cmd, 0, sizeof(puk_cmd));
		memset(&ck, 0, sizeof(ck));

		memcpy(ck.key_template, "\x1e\x00\x00\x20", 4);

		puk_cmd.encoding = SC_PIN_ENCODING_GLP;
		puk_cmd.len = puk_len;
		puk_cmd.data = puk;
		puk_cmd.max_length = 8;

		ck.new_key.key_len = sc_build_pin(ck.new_key.key_value, 
			sizeof(ck.new_key.key_value), &puk_cmd, 1); 
		if(ck.new_key.key_len<0)
			return SC_ERROR_CARD_CMD_FAILED;

		r = sc_card_ctl(card, SC_CARDCTL_WESTCOS_CHANGE_KEY, &ck);
		if(r) return r;
	}

	return 0;
}

/*
 * Create a new key file
 */
static int westcos_pkcs15init_create_key(sc_profile_t *profile, 
						sc_card_t *card, 
						sc_pkcs15_object_t *obj)
{
	int             r;
	size_t          size;
	sc_file_t       *keyfile = NULL;
	sc_pkcs15_prkey_info_t *key_info = (sc_pkcs15_prkey_info_t *) obj->data;

	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		return SC_ERROR_NOT_SUPPORTED;
	}

	switch (key_info->modulus_length) {
		case  128: size = 112; break;
		case  256: size = 184; break;
		case  512: size = 336; break;
		case  768: size = 480; break;
		case 1024: size = 616; break;
		case 1536: size = 912; break;
		case 2048: size = 1200; break;
		default:
			r = SC_ERROR_INVALID_ARGUMENTS;
			goto out;
	}

	keyfile = sc_file_new();
	if(keyfile == NULL)
		return SC_ERROR_OUT_OF_MEMORY;

	keyfile->path = key_info->path;

	keyfile->type = SC_FILE_TYPE_WORKING_EF;
	keyfile->ef_structure = SC_FILE_EF_TRANSPARENT;
	keyfile->shareable = 0;
	keyfile->size = size;

	r = sc_file_add_acl_entry(keyfile, SC_AC_OP_READ, SC_AC_CHV, 0);
	if(r) goto out;
	r = sc_file_add_acl_entry(keyfile, SC_AC_OP_UPDATE, SC_AC_CHV, 0);
	if(r) goto out;
	r = sc_file_add_acl_entry(keyfile, SC_AC_OP_ERASE, SC_AC_CHV, 0);
	if(r) goto out;

	r = sc_pkcs15init_create_file(profile, card, keyfile);
	if(r)
	{
		if(r != SC_ERROR_FILE_ALREADY_EXISTS)
			goto out;
		r = 0;
	}

out:
	if(keyfile)
		sc_file_free(keyfile);

	return r;
}



/*
 * Store a private key
 */
static int westcos_pkcs15init_store_key(sc_profile_t *profile, 
						sc_card_t *card,
						sc_pkcs15_object_t *obj,
						sc_pkcs15_prkey_t *key)
{
	return SC_ERROR_NOT_SUPPORTED;
}

/*
 * Generate key
 */
static int westcos_pkcs15init_generate_key(sc_profile_t *profile, 
						sc_card_t *card,
						sc_pkcs15_object_t *obj,
						sc_pkcs15_pubkey_t *pubkey)
{
	int             r = SC_ERROR_UNKNOWN;
	long			lg;
	char			*p;
	sc_pkcs15_prkey_info_t *key_info = (sc_pkcs15_prkey_info_t *) obj->data;
#ifdef ENABLE_OPENSSL
	RSA				*rsa = RSA_new();
	BIGNUM			*bn = BN_new();
	BIO				*mem = BIO_new(BIO_s_mem());
#endif

#ifndef ENABLE_OPENSSL
	r = SC_ERROR_NOT_SUPPORTED;
#else
	sc_file_t 		*prkf = NULL;
	
	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		return SC_ERROR_NOT_SUPPORTED;
	}

	if(rsa == NULL || bn == NULL || mem == NULL) 
	{
		r = SC_ERROR_OUT_OF_MEMORY;
		goto out;
	}

	/* pkcs11 re-route routine cryptage vers la carte fixe default to use openssl */
	rsa->meth = RSA_PKCS1_SSLeay();

	if(!BN_set_word(bn, RSA_F4) || 
		!RSA_generate_key_ex(rsa, key_info->modulus_length, bn, NULL))
	{
		r = SC_ERROR_UNKNOWN;
		goto out;
	}

	if(pubkey != NULL)
	{
		if(!i2d_RSAPublicKey_bio(mem, rsa))
		{
			r = SC_ERROR_UNKNOWN;
			goto out;
		}

		lg = BIO_get_mem_data(mem, &p);

		pubkey->algorithm = SC_ALGORITHM_RSA;

		r = sc_pkcs15_decode_pubkey(card->ctx, pubkey, p, lg);
	}

	BIO_reset(mem);

	if(!i2d_RSAPrivateKey_bio(mem, rsa))
	{
		r = SC_ERROR_UNKNOWN;
		goto out;
	}

	lg = BIO_get_mem_data(mem, &p);

	/* Get the private key file */
	r = sc_profile_get_file_by_path(profile, &key_info->path, &prkf);
	if (r < 0) 
	{
		char pbuf[SC_MAX_PATH_STRING_SIZE];

		r = sc_path_print(pbuf, sizeof(pbuf), &key_info->path);
		if (r != SC_SUCCESS)
			pbuf[0] = '\0';

		return r;
	}

	r = sc_pkcs15init_update_file(profile, card, prkf, p, lg);
	if(r) goto out;

out:
	if(mem)
		BIO_free(mem);
	if(bn)
		BN_free(bn);
	if(rsa)
		RSA_free(rsa);
	if(prkf)
		sc_file_free(prkf);
#endif

	return r;
}

static int westcos_pkcs15init_finalize_card(sc_card_t *card)
{
	int r;

	/* be sure authentificate card */
	r = sc_card_ctl(card, SC_CARDCTL_WESTCOS_AUT_KEY, NULL);
	if(r) return (r);

	return sc_pkcs15init_set_lifecycle(card, SC_CARDCTRL_LIFECYCLE_USER);
}

static struct sc_pkcs15init_operations sc_pkcs15init_westcos_operations = {
        NULL,								/* erase_card */
        westcos_pkcs15init_init_card,		/* init_card  */
        westcos_pkcs15init_create_dir,		/* create_dir */
        NULL,								 /* create_domain */
        westcos_pkcs15_select_pin_reference,/* select_pin_reference */
        westcos_pkcs15_create_pin,			/* create_pin */
        NULL,								/* select_key_reference */
        westcos_pkcs15init_create_key,		/* create_key */
        westcos_pkcs15init_store_key,		/* store_key */
        westcos_pkcs15init_generate_key,	/* generate_key */
        NULL, NULL,							/* encode private/public key */
        westcos_pkcs15init_finalize_card,	/* finalize_card */
		NULL,NULL,NULL,NULL,				/* old style app */
        NULL,								/* old_generate_key */
        NULL								/* delete_object */
};
	
struct sc_pkcs15init_operations* sc_pkcs15init_get_westcos_ops(void)
{
	return &sc_pkcs15init_westcos_operations;
}


