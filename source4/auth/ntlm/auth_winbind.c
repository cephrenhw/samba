/* 
   Unix SMB/CIFS implementation.

   Winbind authentication mechnism

   Copyright (C) Tim Potter 2000
   Copyright (C) Andrew Bartlett 2001 - 2002
   Copyright (C) Stefan Metzmacher 2005
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include <tevent.h>
#include "../lib/util/tevent_ntstatus.h"
#include "auth/auth.h"
#include "auth/ntlm/auth_proto.h"
#include "librpc/gen_ndr/ndr_winbind_c.h"
#include "lib/messaging/irpc.h"
#include "param/param.h"
#include "nsswitch/libwbclient/wbclient.h"
#include "auth/auth_sam_reply.h"
#include "libcli/security/security.h"
#include "dsdb/samdb/samdb.h"
#include "auth/auth_sam.h"

_PUBLIC_ NTSTATUS auth4_winbind_init(TALLOC_CTX *);

static NTSTATUS winbind_want_check(struct auth_method_context *ctx,
				   TALLOC_CTX *mem_ctx,
				   const struct auth_usersupplied_info *user_info)
{
	if (!user_info->mapped.account_name || !*user_info->mapped.account_name) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}

	/* TODO: maybe limit the user scope to remote users only */
	return NT_STATUS_OK;
}

static NTSTATUS winbind_rodc_want_check(struct auth_method_context *ctx,
					TALLOC_CTX *mem_ctx,
					const struct auth_usersupplied_info *user_info)
{
	int ret;
	bool am_rodc;

	if (!user_info->mapped.account_name || !*user_info->mapped.account_name) {
		return NT_STATUS_NOT_IMPLEMENTED;
	}

	if (ctx->auth_ctx->sam_ctx == NULL) {
		DBG_ERR("ctx->auth_ctx->sam_ctx == NULL, don't check.\n");
		return NT_STATUS_NOT_IMPLEMENTED;
	}

	ret = samdb_rodc(ctx->auth_ctx->sam_ctx, &am_rodc);
	if (ret != LDB_SUCCESS) {
		DBG_ERR("samdb_rodc() failed %d %s, don't check.\n",
			ret, ldb_errstring(ctx->auth_ctx->sam_ctx));
		return NT_STATUS_NOT_IMPLEMENTED;
	}

	if (!am_rodc) {
		/*
		 * We don't support trusts yet and we
		 * don't want to add them using the
		 * semi-async irpc call that uses
		 * a nested event loop.
		 */
		return NT_STATUS_NOT_IMPLEMENTED;
	}

	/*
	 * We're a RODC, so we forward the request to our winbind.
	 * As the RODC is not yet production ready anyway, we keep
	 * the semi-async behavior with nested event loops in order
	 * to keep autobuild happy.
	 */
	return NT_STATUS_OK;
}

struct winbind_check_password_state {
	struct auth_method_context *ctx;
	const struct auth_usersupplied_info *user_info;
	struct winbind_SamLogon req;
	struct auth_user_info_dc *user_info_dc;
	bool authoritative;
};

static void winbind_check_password_done(struct tevent_req *subreq);

/*
 Authenticate a user with a challenge/response
 using IRPC to the winbind task
*/
static struct tevent_req *winbind_check_password_send(TALLOC_CTX *mem_ctx,
				struct tevent_context *ev,
				struct auth_method_context *ctx,
				const struct auth_usersupplied_info *user_info)
{
	struct tevent_req *req = NULL;
	struct winbind_check_password_state *state = NULL;
	NTSTATUS status;
	struct dcerpc_binding_handle *irpc_handle;
	const struct auth_usersupplied_info *user_info_new;
	struct netr_IdentityInfo *identity_info;
	struct imessaging_context *msg_ctx;
	struct tevent_req *subreq = NULL;

	req = tevent_req_create(mem_ctx, &state,
				struct winbind_check_password_state);
	if (req == NULL) {
		return NULL;
	}
	state->ctx = ctx;
	state->user_info = user_info;
	state->authoritative = true;

	msg_ctx = imessaging_client_init(state, ctx->auth_ctx->lp_ctx, ev);
	if (msg_ctx == NULL) {
		DEBUG(1, ("imessaging_init failed\n"));
		tevent_req_nterror(req, NT_STATUS_INVALID_SERVER_STATE);
		return tevent_req_post(req, ev);
	}

	irpc_handle = irpc_binding_handle_by_name(state, msg_ctx,
						  "winbind_server",
						  &ndr_table_winbind);
	if (irpc_handle == NULL) {
		DEBUG(0, ("Winbind authentication for [%s]\\[%s] failed, " 
			  "no winbind_server running!\n",
			  user_info->client.domain_name, user_info->client.account_name));
		tevent_req_nterror(req, NT_STATUS_NO_LOGON_SERVERS);
		return tevent_req_post(req, ev);
	}

	/*
	 * 120 seconds should be enough even for trusted domains.
	 *
	 * Currently winbindd has a much lower limit.
	 * And tests with Windows RODCs show that it
	 * returns NO_LOGON_SERVERS after 90-100 seconds
	 * if it can't reach any RWDC.
	 */
	dcerpc_binding_handle_set_timeout(irpc_handle, 120);

	if (user_info->flags & USER_INFO_INTERACTIVE_LOGON) {
		struct netr_PasswordInfo *password_info;

		status = encrypt_user_info(state, ctx->auth_ctx, AUTH_PASSWORD_HASH,
					   user_info, &user_info_new);
		if (tevent_req_nterror(req, status)) {
			return tevent_req_post(req, ev);
		}
		user_info = user_info_new;

		password_info = talloc_zero(state, struct netr_PasswordInfo);
		if (tevent_req_nomem(password_info, req)) {
			return tevent_req_post(req, ev);
		}

		password_info->lmpassword = *user_info->password.hash.lanman;
		password_info->ntpassword = *user_info->password.hash.nt;

		identity_info = &password_info->identity_info;
		state->req.in.logon_level	= 1;
		state->req.in.logon.password= password_info;
	} else {
		struct netr_NetworkInfo *network_info;
		uint8_t chal[8];

		status = encrypt_user_info(state, ctx->auth_ctx, AUTH_PASSWORD_RESPONSE,
					   user_info, &user_info_new);
		if (tevent_req_nterror(req, status)) {
			return tevent_req_post(req, ev);
		}
		user_info = user_info_new;

		network_info = talloc_zero(state, struct netr_NetworkInfo);
		if (tevent_req_nomem(network_info, req)) {
			return tevent_req_post(req, ev);
		}

		status = auth_get_challenge(ctx->auth_ctx, chal);
		if (tevent_req_nterror(req, status)) {
			return tevent_req_post(req, ev);
		}

		memcpy(network_info->challenge, chal, sizeof(network_info->challenge));

		network_info->nt.length = user_info->password.response.nt.length;
		network_info->nt.data	= user_info->password.response.nt.data;

		network_info->lm.length = user_info->password.response.lanman.length;
		network_info->lm.data	= user_info->password.response.lanman.data;

		identity_info = &network_info->identity_info;
		state->req.in.logon_level	= 2;
		state->req.in.logon.network = network_info;
	}

	identity_info->domain_name.string	= user_info->client.domain_name;
	identity_info->parameter_control	= user_info->logon_parameters; /* see MSV1_0_* */
	identity_info->logon_id_low		= 0;
	identity_info->logon_id_high		= 0;
	identity_info->account_name.string	= user_info->client.account_name;
	identity_info->workstation.string	= user_info->workstation_name;

	state->req.in.validation_level = 3;

	subreq = dcerpc_winbind_SamLogon_r_send(state, ev, irpc_handle,
						&state->req);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq,
				winbind_check_password_done,
				req);

	return req;
}

static void winbind_check_password_done(struct tevent_req *subreq)
{
	struct tevent_req *req =
		tevent_req_callback_data(subreq,
		struct tevent_req);
	struct winbind_check_password_state *state =
		tevent_req_data(req,
		struct winbind_check_password_state);
	struct auth_method_context *ctx = state->ctx;
	const struct auth_usersupplied_info *user_info = state->user_info;
	const char *account_name = user_info->mapped.account_name;
	struct ldb_dn *domain_dn = NULL;
	struct ldb_message *msg = NULL;
	const char *p = NULL;
	NTSTATUS status;

	status = dcerpc_winbind_SamLogon_r_recv(subreq, state);
	if (NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
		status = NT_STATUS_NO_LOGON_SERVERS;
	}
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	status = state->req.out.result;
	if (!NT_STATUS_IS_OK(status)) {
		if (!state->req.out.authoritative) {
			state->authoritative = false;
		}
		tevent_req_nterror(req, status);
		return;
	}

	/*
	 * At best, reset the badPwdCount to 0 if the account exists.
	 * This means that lockouts happen at a badPwdCount earlier than
	 * normal, but makes it more fault tolerant.
	 */
	p = strchr_m(account_name, '@');
	if (p != NULL) {
		const char *nt4_domain = NULL;
		const char *nt4_account = NULL;

		status = crack_name_to_nt4_name(state,
						ctx->auth_ctx->sam_ctx,
						DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL,
						account_name,
						&nt4_domain, &nt4_account);
		if (NT_STATUS_IS_OK(status) &&
		    lpcfg_is_mydomain(ctx->auth_ctx->lp_ctx, nt4_domain))
		{
			account_name = nt4_account;
		}
	}

	domain_dn = ldb_get_default_basedn(ctx->auth_ctx->sam_ctx);
	if (domain_dn != NULL) {
		status = authsam_search_account(state, ctx->auth_ctx->sam_ctx,
						account_name, domain_dn, &msg);
		if (NT_STATUS_IS_OK(status)) {
			authsam_logon_success_accounting(
				ctx->auth_ctx->sam_ctx, msg,
				domain_dn,
				user_info->flags & USER_INFO_INTERACTIVE_LOGON,
				NULL);
		}
	}

	status = make_user_info_dc_netlogon_validation(state,
						      user_info->client.account_name,
						      state->req.in.validation_level,
						      &state->req.out.validation,
						      true, /* This user was authenticated */
						      &state->user_info_dc);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	tevent_req_done(req);
}

static NTSTATUS winbind_check_password_recv(struct tevent_req *req,
					    TALLOC_CTX *mem_ctx,
					    struct auth_user_info_dc **user_info_dc,
					    bool *pauthoritative)
{
	struct winbind_check_password_state *state =
		tevent_req_data(req,
		struct winbind_check_password_state);
	NTSTATUS status = NT_STATUS_OK;

	*pauthoritative = state->authoritative;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	*user_info_dc = talloc_move(mem_ctx, &state->user_info_dc);

	tevent_req_received(req);
	return NT_STATUS_OK;
}

/*
 Authenticate a user with a challenge/response
 using the samba3 winbind protocol via libwbclient
*/
static NTSTATUS winbind_check_password_wbclient(struct auth_method_context *ctx,
						TALLOC_CTX *mem_ctx,
						const struct auth_usersupplied_info *user_info,
						struct auth_user_info_dc **user_info_dc,
						bool *authoritative)
{
	struct wbcAuthUserParams params;
	struct wbcAuthUserInfo *info = NULL;
	struct wbcAuthErrorInfo *err = NULL;
	wbcErr wbc_status;
	NTSTATUS nt_status;
	struct netr_SamInfo6 *info6 = NULL;
	union netr_Validation validation;

	/* Send off request */
	const struct auth_usersupplied_info *user_info_temp;
	nt_status = encrypt_user_info(mem_ctx, ctx->auth_ctx,
				      AUTH_PASSWORD_RESPONSE,
				      user_info, &user_info_temp);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}
	user_info = user_info_temp;

	ZERO_STRUCT(params);
	ZERO_STRUCT(validation);
	/*params.flags = WBFLAG_PAM_INFO3_NDR;*/

	params.parameter_control = user_info->logon_parameters;
	params.parameter_control |= WBC_MSV1_0_ALLOW_WORKSTATION_TRUST_ACCOUNT |
				    WBC_MSV1_0_ALLOW_SERVER_TRUST_ACCOUNT;
	params.level = WBC_AUTH_USER_LEVEL_RESPONSE;

	params.account_name     = user_info->client.account_name;
	params.domain_name      = user_info->client.domain_name;
	params.workstation_name = user_info->workstation_name;

	DEBUG(5,("looking up %s@%s logging in from %s\n",
		  params.account_name, params.domain_name,
		  params.workstation_name));

	memcpy(params.password.response.challenge,
	       ctx->auth_ctx->challenge.data.data,
	       sizeof(params.password.response.challenge));

	params.password.response.lm_length =
		user_info->password.response.lanman.length;
	params.password.response.nt_length =
		user_info->password.response.nt.length;

	params.password.response.lm_data =
		user_info->password.response.lanman.data;
	params.password.response.nt_data =
		user_info->password.response.nt.data;

	wbc_status = wbcAuthenticateUserEx(&params, &info, &err);
	if (wbc_status == WBC_ERR_AUTH_ERROR) {
		if (err) {
			DEBUG(1, ("error was %s (0x%08x)\nerror message was '%s'\n",
			      err->nt_string, err->nt_status, err->display_string));
			nt_status = NT_STATUS(err->nt_status);
			wbcFreeMemory(err);
		} else {
			nt_status = NT_STATUS_LOGON_FAILURE;
		}
		NT_STATUS_NOT_OK_RETURN(nt_status);
	} else if (!WBC_ERROR_IS_OK(wbc_status)) {
		DEBUG(1, ("wbcAuthenticateUserEx: failed with %u - %s\n",
			wbc_status, wbcErrorString(wbc_status)));
		if (err) {
			DEBUG(1, ("error was %s (0x%08x)\nerror message was '%s'\n",
			      err->nt_string, err->nt_status, err->display_string));
		}
		return NT_STATUS_LOGON_FAILURE;
	}
	info6 = wbcAuthUserInfo_to_netr_SamInfo6(mem_ctx, info);
	wbcFreeMemory(info);
	if (!info6) {
		DEBUG(1, ("wbcAuthUserInfo_to_netr_SamInfo6 failed\n"));
		return NT_STATUS_NO_MEMORY;
	}

	validation.sam6 = info6;
	nt_status = make_user_info_dc_netlogon_validation(mem_ctx,
							  user_info->client.account_name,
							  6, &validation,
							  true, /* This user was authenticated */
							  user_info_dc);
	return nt_status;

}

static const struct auth_operations winbind_ops = {
	.name			= "winbind",
	.want_check		= winbind_want_check,
	.check_password_send	= winbind_check_password_send,
	.check_password_recv	= winbind_check_password_recv
};

static const struct auth_operations winbind_rodc_ops = {
	.name			= "winbind_rodc",
	.want_check		= winbind_rodc_want_check,
	.check_password_send	= winbind_check_password_send,
	.check_password_recv	= winbind_check_password_recv
};

static const struct auth_operations winbind_wbclient_ops = {
	.name		= "winbind_wbclient",
	.want_check	= winbind_want_check,
	.check_password	= winbind_check_password_wbclient
};

_PUBLIC_ NTSTATUS auth4_winbind_init(TALLOC_CTX *ctx)
{
	NTSTATUS ret;

	ret = auth_register(ctx, &winbind_ops);
	if (!NT_STATUS_IS_OK(ret)) {
		DEBUG(0,("Failed to register 'winbind' auth backend!\n"));
		return ret;
	}

	ret = auth_register(ctx, &winbind_rodc_ops);
	if (!NT_STATUS_IS_OK(ret)) {
		DEBUG(0,("Failed to register 'winbind_rodc' auth backend!\n"));
		return ret;
	}

	ret = auth_register(ctx, &winbind_wbclient_ops);
	if (!NT_STATUS_IS_OK(ret)) {
		DEBUG(0,("Failed to register 'winbind_wbclient' auth backend!\n"));
		return ret;
	}

	return NT_STATUS_OK;
}
