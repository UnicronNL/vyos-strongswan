/*
 * Copyright (C) 2005-2009 Martin Willi
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "psk_authenticator.h"

#include <daemon.h>
#include <encoding/payloads/auth_payload.h>

typedef struct private_psk_authenticator_t private_psk_authenticator_t;

/**
 * Private data of an psk_authenticator_t object.
 */
struct private_psk_authenticator_t {

	/**
	 * Public authenticator_t interface.
	 */
	psk_authenticator_t public;

	/**
	 * Assigned IKE_SA
	 */
	ike_sa_t *ike_sa;

	/**
	 * nonce to include in AUTH calculation
	 */
	chunk_t nonce;

	/**
	 * IKE_SA_INIT message data to include in AUTH calculation
	 */
	chunk_t ike_sa_init;
};

/*
 * Implementation of authenticator_t.build for builder
 */
static status_t build(private_psk_authenticator_t *this, message_t *message)
{
	identification_t *my_id, *other_id;
	auth_payload_t *auth_payload;
	shared_key_t *key;
	chunk_t auth_data;
	keymat_t *keymat;

	keymat = this->ike_sa->get_keymat(this->ike_sa);
	my_id = this->ike_sa->get_my_id(this->ike_sa);
	other_id = this->ike_sa->get_other_id(this->ike_sa);
	DBG1(DBG_IKE, "authentication of '%Y' (myself) with %N",
		 my_id, auth_method_names, AUTH_PSK);
	key = charon->credentials->get_shared(charon->credentials, SHARED_IKE,
										  my_id, other_id);
	if (key == NULL)
	{
		DBG1(DBG_IKE, "no shared key found for '%Y' - '%Y'", my_id, other_id);
		return NOT_FOUND;
	}
	auth_data = keymat->get_psk_sig(keymat, FALSE, this->ike_sa_init,
									this->nonce, key->get_key(key), my_id);
	key->destroy(key);
	DBG2(DBG_IKE, "successfully created shared key MAC");
	auth_payload = auth_payload_create();
	auth_payload->set_auth_method(auth_payload, AUTH_PSK);
	auth_payload->set_data(auth_payload, auth_data);
	chunk_free(&auth_data);
	message->add_payload(message, (payload_t*)auth_payload);

	return SUCCESS;
}

/**
 * Implementation of authenticator_t.process for verifier
 */
static status_t process(private_psk_authenticator_t *this, message_t *message)
{
	chunk_t auth_data, recv_auth_data;
	identification_t *my_id, *other_id;
	auth_payload_t *auth_payload;
	auth_cfg_t *auth;
	shared_key_t *key;
	enumerator_t *enumerator;
	bool authenticated = FALSE;
	int keys_found = 0;
	keymat_t *keymat;

	auth_payload = (auth_payload_t*)message->get_payload(message, AUTHENTICATION);
	if (!auth_payload)
	{
		return FAILED;
	}
	keymat = this->ike_sa->get_keymat(this->ike_sa);
	recv_auth_data = auth_payload->get_data(auth_payload);
	my_id = this->ike_sa->get_my_id(this->ike_sa);
	other_id = this->ike_sa->get_other_id(this->ike_sa);
	enumerator = charon->credentials->create_shared_enumerator(
							charon->credentials, SHARED_IKE, my_id, other_id);
	while (!authenticated && enumerator->enumerate(enumerator, &key, NULL, NULL))
	{
		keys_found++;

		auth_data = keymat->get_psk_sig(keymat, TRUE, this->ike_sa_init,
									this->nonce, key->get_key(key), other_id);
		if (auth_data.len && chunk_equals(auth_data, recv_auth_data))
		{
			DBG1(DBG_IKE, "authentication of '%Y' with %N successful",
				 other_id, auth_method_names, AUTH_PSK);
			authenticated = TRUE;
		}
		chunk_free(&auth_data);
	}
	enumerator->destroy(enumerator);

	if (!authenticated)
	{
		if (keys_found == 0)
		{
			DBG1(DBG_IKE, "no shared key found for '%Y' - '%Y'", my_id, other_id);
			return NOT_FOUND;
		}
		DBG1(DBG_IKE, "tried %d shared key%s for '%Y' - '%Y', but MAC mismatched",
			 keys_found, keys_found == 1 ? "" : "s", my_id, other_id);
		return FAILED;
	}

	auth = this->ike_sa->get_auth_cfg(this->ike_sa, FALSE);
	auth->add(auth, AUTH_RULE_AUTH_CLASS, AUTH_CLASS_PSK);
	return SUCCESS;
}

/**
 * Implementation of authenticator_t.process for builder
 * Implementation of authenticator_t.build for verifier
 */
static status_t return_failed()
{
	return FAILED;
}

/**
 * Implementation of authenticator_t.destroy.
 */
static void destroy(private_psk_authenticator_t *this)
{
	free(this);
}

/*
 * Described in header.
 */
psk_authenticator_t *psk_authenticator_create_builder(ike_sa_t *ike_sa,
									chunk_t received_nonce, chunk_t sent_init)
{
	private_psk_authenticator_t *this = malloc_thing(private_psk_authenticator_t);

	this->public.authenticator.build = (status_t(*)(authenticator_t*, message_t *message))build;
	this->public.authenticator.process = (status_t(*)(authenticator_t*, message_t *message))return_failed;
	this->public.authenticator.is_mutual = (bool(*)(authenticator_t*))return_false;
	this->public.authenticator.destroy = (void(*)(authenticator_t*))destroy;

	this->ike_sa = ike_sa;
	this->ike_sa_init = sent_init;
	this->nonce = received_nonce;

	return &this->public;
}

/*
 * Described in header.
 */
psk_authenticator_t *psk_authenticator_create_verifier(ike_sa_t *ike_sa,
									chunk_t sent_nonce, chunk_t received_init)
{
	private_psk_authenticator_t *this = malloc_thing(private_psk_authenticator_t);

	this->public.authenticator.build = (status_t(*)(authenticator_t*, message_t *messageh))return_failed;
	this->public.authenticator.process = (status_t(*)(authenticator_t*, message_t *message))process;
	this->public.authenticator.is_mutual = (bool(*)(authenticator_t*))return_false;
	this->public.authenticator.destroy = (void(*)(authenticator_t*))destroy;

	this->ike_sa = ike_sa;
	this->ike_sa_init = received_init;
	this->nonce = sent_nonce;

	return &this->public;
}
