/*
 *  Unix SMB/CIFS implementation.
 *  RPC Pipe client / server routines
 *  Copyright (C) Andrew Tridgell              1992-1998,
 *  Largely re-written : 2005
 *  Copyright (C) Jeremy Allison		1998 - 2005
 *  Copyright (C) Simo Sorce			2010
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"
#include "rpc_server/srv_pipe_internal.h"
#include "rpc_dce.h"
#include "../libcli/named_pipe_auth/npa_tstream.h"
#include "rpc_server/rpc_ncacn_np.h"
#include "librpc/gen_ndr/netlogon.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_RPC_SRV

static int pipes_open;

static struct pipes_struct *InternalPipes;

/* TODO
 * the following prototypes are declared here to avoid
 * code being moved about too much for a patch to be
 * disrupted / less obvious.
 *
 * these functions, and associated functions that they
 * call, should be moved behind a .so module-loading
 * system _anyway_.  so that's the next step...
 */

/****************************************************************************
 Internal Pipe iterator functions.
****************************************************************************/

struct pipes_struct *get_first_internal_pipe(void)
{
	return InternalPipes;
}

struct pipes_struct *get_next_internal_pipe(struct pipes_struct *p)
{
	return p->next;
}

static void free_pipe_rpc_context_internal( PIPE_RPC_FNS *list )
{
	PIPE_RPC_FNS *tmp = list;
	PIPE_RPC_FNS *tmp2;

	while (tmp) {
		tmp2 = tmp->next;
		SAFE_FREE(tmp);
		tmp = tmp2;
	}

	return;
}

bool check_open_pipes(void)
{
	struct pipes_struct *p;

	for (p = InternalPipes; p != NULL; p = p->next) {
		if (num_pipe_handles(p) != 0) {
			return true;
		}
	}
	return false;
}

/****************************************************************************
 Close an rpc pipe.
****************************************************************************/

int close_internal_rpc_pipe_hnd(struct pipes_struct *p)
{
	if (!p) {
		DEBUG(0,("Invalid pipe in close_internal_rpc_pipe_hnd\n"));
		return False;
	}

	TALLOC_FREE(p->auth.auth_ctx);

	free_pipe_rpc_context_internal( p->contexts );

	/* Free the handles database. */
	close_policy_by_pipe(p);

	DLIST_REMOVE(InternalPipes, p);

	ZERO_STRUCTP(p);

	return 0;
}

/****************************************************************************
 Make an internal namedpipes structure
****************************************************************************/

struct pipes_struct *make_internal_rpc_pipe_p(TALLOC_CTX *mem_ctx,
					      const struct ndr_syntax_id *syntax,
					      struct client_address *client_id,
					      const struct auth_serversupplied_info *server_info,
					      struct messaging_context *msg_ctx)
{
	struct pipes_struct *p;

	DEBUG(4,("Create pipe requested %s\n",
		 get_pipe_name_from_syntax(talloc_tos(), syntax)));

	p = TALLOC_ZERO_P(mem_ctx, struct pipes_struct);

	if (!p) {
		DEBUG(0,("ERROR! no memory for pipes_struct!\n"));
		return NULL;
	}

	p->mem_ctx = talloc_named(p, 0, "pipe %s %p",
				 get_pipe_name_from_syntax(talloc_tos(),
							   syntax), p);
	if (p->mem_ctx == NULL) {
		DEBUG(0,("open_rpc_pipe_p: talloc_init failed.\n"));
		TALLOC_FREE(p);
		return NULL;
	}

	if (!init_pipe_handles(p, syntax)) {
		DEBUG(0,("open_rpc_pipe_p: init_pipe_handles failed.\n"));
		TALLOC_FREE(p);
		return NULL;
	}

	p->server_info = copy_serverinfo(p, server_info);
	if (p->server_info == NULL) {
		DEBUG(0, ("open_rpc_pipe_p: copy_serverinfo failed\n"));
		close_policy_by_pipe(p);
		TALLOC_FREE(p);
		return NULL;
	}

	p->msg_ctx = msg_ctx;

	DLIST_ADD(InternalPipes, p);

	p->client_id = client_id;

	p->endian = RPC_LITTLE_ENDIAN;

	p->syntax = *syntax;

	DEBUG(4,("Created internal pipe %s (pipes_open=%d)\n",
		 get_pipe_name_from_syntax(talloc_tos(), syntax), pipes_open));

	talloc_set_destructor(p, close_internal_rpc_pipe_hnd);

	return p;
}

static NTSTATUS rpcint_dispatch(struct pipes_struct *p,
				TALLOC_CTX *mem_ctx,
				uint32_t opnum,
				const DATA_BLOB *in_data,
				DATA_BLOB *out_data)
{
	uint32_t num_cmds = rpc_srv_get_pipe_num_cmds(&p->syntax);
	const struct api_struct *cmds = rpc_srv_get_pipe_cmds(&p->syntax);
	uint32_t i;
	bool ok;

	/* set opnum */
	p->opnum = opnum;

	for (i = 0; i < num_cmds; i++) {
		if (cmds[i].opnum == opnum && cmds[i].fn != NULL) {
			break;
		}
	}

	if (i == num_cmds) {
		return NT_STATUS_RPC_PROCNUM_OUT_OF_RANGE;
	}

	p->in_data.data = *in_data;
	p->out_data.rdata = data_blob_null;

	ok = cmds[i].fn(p);
	p->in_data.data = data_blob_null;
	if (!ok) {
		data_blob_free(&p->out_data.rdata);
		talloc_free_children(p->mem_ctx);
		return NT_STATUS_RPC_CALL_FAILED;
	}

	if (p->fault_state) {
		p->fault_state = false;
		data_blob_free(&p->out_data.rdata);
		talloc_free_children(p->mem_ctx);
		return NT_STATUS_RPC_CALL_FAILED;
	}

	if (p->bad_handle_fault_state) {
		p->bad_handle_fault_state = false;
		data_blob_free(&p->out_data.rdata);
		talloc_free_children(p->mem_ctx);
		return NT_STATUS_RPC_SS_CONTEXT_MISMATCH;
	}

	if (p->rng_fault_state) {
		p->rng_fault_state = false;
		data_blob_free(&p->out_data.rdata);
		talloc_free_children(p->mem_ctx);
		return NT_STATUS_RPC_PROCNUM_OUT_OF_RANGE;
	}

	*out_data = p->out_data.rdata;
	talloc_steal(mem_ctx, out_data->data);
	p->out_data.rdata = data_blob_null;

	talloc_free_children(p->mem_ctx);
	return NT_STATUS_OK;
}

struct rpcint_bh_state {
	struct pipes_struct *p;
};

static bool rpcint_bh_is_connected(struct dcerpc_binding_handle *h)
{
	struct rpcint_bh_state *hs = dcerpc_binding_handle_data(h,
				     struct rpcint_bh_state);

	if (!hs->p) {
		return false;
	}

	return true;
}

static uint32_t rpcint_bh_set_timeout(struct dcerpc_binding_handle *h,
				      uint32_t timeout)
{
	/* TODO: implement timeouts */
	return UINT32_MAX;
}

struct rpcint_bh_raw_call_state {
	DATA_BLOB in_data;
	DATA_BLOB out_data;
	uint32_t out_flags;
};

static struct tevent_req *rpcint_bh_raw_call_send(TALLOC_CTX *mem_ctx,
						  struct tevent_context *ev,
						  struct dcerpc_binding_handle *h,
						  const struct GUID *object,
						  uint32_t opnum,
						  uint32_t in_flags,
						  const uint8_t *in_data,
						  size_t in_length)
{
	struct rpcint_bh_state *hs =
		dcerpc_binding_handle_data(h,
		struct rpcint_bh_state);
	struct tevent_req *req;
	struct rpcint_bh_raw_call_state *state;
	bool ok;
	NTSTATUS status;

	req = tevent_req_create(mem_ctx, &state,
				struct rpcint_bh_raw_call_state);
	if (req == NULL) {
		return NULL;
	}
	state->in_data.data = discard_const_p(uint8_t, in_data);
	state->in_data.length = in_length;

	ok = rpcint_bh_is_connected(h);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_INVALID_CONNECTION);
		return tevent_req_post(req, ev);
	}

	/* TODO: allow async */
	status = rpcint_dispatch(hs->p, state, opnum,
				 &state->in_data,
				 &state->out_data);
	if (!NT_STATUS_IS_OK(status)) {
		tevent_req_nterror(req, status);
		return tevent_req_post(req, ev);
	}

	tevent_req_done(req);
	return tevent_req_post(req, ev);
}

static NTSTATUS rpcint_bh_raw_call_recv(struct tevent_req *req,
					TALLOC_CTX *mem_ctx,
					uint8_t **out_data,
					size_t *out_length,
					uint32_t *out_flags)
{
	struct rpcint_bh_raw_call_state *state =
		tevent_req_data(req,
		struct rpcint_bh_raw_call_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	*out_data = talloc_move(mem_ctx, &state->out_data.data);
	*out_length = state->out_data.length;
	*out_flags = 0;
	tevent_req_received(req);
	return NT_STATUS_OK;
}

struct rpcint_bh_disconnect_state {
	uint8_t _dummy;
};

static struct tevent_req *rpcint_bh_disconnect_send(TALLOC_CTX *mem_ctx,
						struct tevent_context *ev,
						struct dcerpc_binding_handle *h)
{
	struct rpcint_bh_state *hs = dcerpc_binding_handle_data(h,
				     struct rpcint_bh_state);
	struct tevent_req *req;
	struct rpcint_bh_disconnect_state *state;
	bool ok;

	req = tevent_req_create(mem_ctx, &state,
				struct rpcint_bh_disconnect_state);
	if (req == NULL) {
		return NULL;
	}

	ok = rpcint_bh_is_connected(h);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_INVALID_CONNECTION);
		return tevent_req_post(req, ev);
	}

	/*
	 * TODO: do a real async disconnect ...
	 *
	 * For now the caller needs to free pipes_struct
	 */
	hs->p = NULL;

	tevent_req_done(req);
	return tevent_req_post(req, ev);
}

static NTSTATUS rpcint_bh_disconnect_recv(struct tevent_req *req)
{
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	tevent_req_received(req);
	return NT_STATUS_OK;
}

static bool rpcint_bh_ref_alloc(struct dcerpc_binding_handle *h)
{
	return true;
}

static void rpcint_bh_do_ndr_print(struct dcerpc_binding_handle *h,
				   int ndr_flags,
				   const void *_struct_ptr,
				   const struct ndr_interface_call *call)
{
	void *struct_ptr = discard_const(_struct_ptr);

	if (DEBUGLEVEL < 10) {
		return;
	}

	if (ndr_flags & NDR_IN) {
		ndr_print_function_debug(call->ndr_print,
					 call->name,
					 ndr_flags,
					 struct_ptr);
	}
	if (ndr_flags & NDR_OUT) {
		ndr_print_function_debug(call->ndr_print,
					 call->name,
					 ndr_flags,
					 struct_ptr);
	}
}

static const struct dcerpc_binding_handle_ops rpcint_bh_ops = {
	.name			= "rpcint",
	.is_connected		= rpcint_bh_is_connected,
	.set_timeout		= rpcint_bh_set_timeout,
	.raw_call_send		= rpcint_bh_raw_call_send,
	.raw_call_recv		= rpcint_bh_raw_call_recv,
	.disconnect_send	= rpcint_bh_disconnect_send,
	.disconnect_recv	= rpcint_bh_disconnect_recv,

	.ref_alloc		= rpcint_bh_ref_alloc,
	.do_ndr_print		= rpcint_bh_do_ndr_print,
};

static NTSTATUS rpcint_binding_handle_ex(TALLOC_CTX *mem_ctx,
			const struct ndr_syntax_id *abstract_syntax,
			const struct ndr_interface_table *ndr_table,
			struct client_address *client_id,
			const struct auth_serversupplied_info *server_info,
			struct messaging_context *msg_ctx,
			struct dcerpc_binding_handle **binding_handle)
{
	struct dcerpc_binding_handle *h;
	struct rpcint_bh_state *hs;

	if (ndr_table) {
		abstract_syntax = &ndr_table->syntax_id;
	}

	h = dcerpc_binding_handle_create(mem_ctx,
					 &rpcint_bh_ops,
					 NULL,
					 ndr_table,
					 &hs,
					 struct rpcint_bh_state,
					 __location__);
	if (h == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	hs->p = make_internal_rpc_pipe_p(hs,
					 abstract_syntax,
					 client_id,
					 server_info,
					 msg_ctx);
	if (hs->p == NULL) {
		TALLOC_FREE(h);
		return NT_STATUS_NO_MEMORY;
	}

	*binding_handle = h;
	return NT_STATUS_OK;
}
/**
 * @brief Create a new DCERPC Binding Handle which uses a local dispatch function.
 *
 * @param[in]  mem_ctx  The memory context to use.
 *
 * @param[in]  ndr_table Normally the ndr_table_<name>.
 *
 * @param[in]  client_id The info about the connected client.
 *
 * @param[in]  serversupplied_info The server supplied authentication function.
 *
 * @param[in]  msg_ctx   The messaging context that can be used by the server
 *
 * @param[out] binding_handle  A pointer to store the connected
 *                             dcerpc_binding_handle
 *
 * @return              NT_STATUS_OK on success, a corresponding NT status if an
 *                      error occured.
 *
 * @code
 *   struct dcerpc_binding_handle *winreg_binding;
 *   NTSTATUS status;
 *
 *   status = rpcint_binding_handle(tmp_ctx,
 *                                  &ndr_table_winreg,
 *                                  p->client_id,
 *                                  p->server_info,
 *                                  p->msg_ctx
 *                                  &winreg_binding);
 * @endcode
 */
NTSTATUS rpcint_binding_handle(TALLOC_CTX *mem_ctx,
			       const struct ndr_interface_table *ndr_table,
			       struct client_address *client_id,
			       const struct auth_serversupplied_info *server_info,
			       struct messaging_context *msg_ctx,
			       struct dcerpc_binding_handle **binding_handle)
{
	return rpcint_binding_handle_ex(mem_ctx, NULL, ndr_table, client_id,
					server_info, msg_ctx, binding_handle);
}

/**
 * @brief Create a new RPC client context which uses a local dispatch function.
 *
 * @param[in]  mem_ctx  The memory context to use.
 *
 * @param[in]  abstract_syntax Normally the syntax_id of the autogenerated
 *                             ndr_table_<name>.
 *
 * @param[in]  dispatch The corresponding autogenerated dispatch function
 *                      rpc_<name>_dispatch.
 *
 * @param[in]  serversupplied_info The server supplied authentication function.
 *
 * @param[out] presult  A pointer to store the connected rpc client pipe.
 *
 * @return              NT_STATUS_OK on success, a corresponding NT status if an
 *                      error occured.
 *
 * @code
 *   struct rpc_pipe_client *winreg_pipe;
 *   NTSTATUS status;
 *
 *   status = rpc_pipe_open_internal(tmp_ctx,
 *                                   &ndr_table_winreg.syntax_id,
 *                                   rpc_winreg_dispatch,
 *                                   p->server_info,
 *                                   &winreg_pipe);
 * @endcode
 */
NTSTATUS rpc_pipe_open_internal(TALLOC_CTX *mem_ctx,
				const struct ndr_syntax_id *abstract_syntax,
				struct auth_serversupplied_info *serversupplied_info,
				struct client_address *client_id,
				struct messaging_context *msg_ctx,
				struct rpc_pipe_client **presult)
{
	struct rpc_pipe_client *result;
	NTSTATUS status;

	result = TALLOC_ZERO_P(mem_ctx, struct rpc_pipe_client);
	if (result == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	result->abstract_syntax = *abstract_syntax;
	result->transfer_syntax = ndr_transfer_syntax;

	if (client_id == NULL) {
		static struct client_address unknown;
		strlcpy(unknown.addr, "<UNKNOWN>", sizeof(unknown.addr));
		unknown.name = "<UNKNOWN>";
		client_id = &unknown;
	}

	result->max_xmit_frag = -1;
	result->max_recv_frag = -1;

	status = rpcint_binding_handle_ex(result,
					  abstract_syntax,
					  NULL,
					  client_id,
					  serversupplied_info,
					  msg_ctx,
					  &result->binding_handle);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(result);
		return status;
	}

	*presult = result;
	return NT_STATUS_OK;
}

/****************************************************************************
 * External pipes functions
 ***************************************************************************/


struct np_proxy_state *make_external_rpc_pipe_p(TALLOC_CTX *mem_ctx,
				const char *pipe_name,
				const struct tsocket_address *local_address,
				const struct tsocket_address *remote_address,
				struct auth_serversupplied_info *server_info)
{
	struct np_proxy_state *result;
	char *socket_np_dir;
	const char *socket_dir;
	struct tevent_context *ev;
	struct tevent_req *subreq;
	struct netr_SamInfo3 *info3;
	NTSTATUS status;
	bool ok;
	int ret;
	int sys_errno;

	result = talloc(mem_ctx, struct np_proxy_state);
	if (result == NULL) {
		DEBUG(0, ("talloc failed\n"));
		return NULL;
	}

	result->read_queue = tevent_queue_create(result, "np_read");
	if (result->read_queue == NULL) {
		DEBUG(0, ("tevent_queue_create failed\n"));
		goto fail;
	}

	result->write_queue = tevent_queue_create(result, "np_write");
	if (result->write_queue == NULL) {
		DEBUG(0, ("tevent_queue_create failed\n"));
		goto fail;
	}

	ev = s3_tevent_context_init(talloc_tos());
	if (ev == NULL) {
		DEBUG(0, ("s3_tevent_context_init failed\n"));
		goto fail;
	}

	socket_dir = lp_parm_const_string(
		GLOBAL_SECTION_SNUM, "external_rpc_pipe", "socket_dir",
		lp_ncalrpc_dir());
	if (socket_dir == NULL) {
		DEBUG(0, ("externan_rpc_pipe:socket_dir not set\n"));
		goto fail;
	}
	socket_np_dir = talloc_asprintf(talloc_tos(), "%s/np", socket_dir);
	if (socket_np_dir == NULL) {
		DEBUG(0, ("talloc_asprintf failed\n"));
		goto fail;
	}

	info3 = talloc_zero(talloc_tos(), struct netr_SamInfo3);
	if (info3 == NULL) {
		DEBUG(0, ("talloc failed\n"));
		goto fail;
	}

	status = serverinfo_to_SamInfo3(server_info, NULL, 0, info3);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(info3);
		DEBUG(0, ("serverinfo_to_SamInfo3 failed: %s\n",
			  nt_errstr(status)));
		goto fail;
	}

	become_root();
	subreq = tstream_npa_connect_send(talloc_tos(), ev,
					  socket_np_dir,
					  pipe_name,
					  remote_address, /* client_addr */
					  NULL, /* client_name */
					  local_address, /* server_addr */
					  NULL, /* server_name */
					  info3,
					  server_info->user_session_key,
					  data_blob_null /* delegated_creds */);
	if (subreq == NULL) {
		unbecome_root();
		DEBUG(0, ("tstream_npa_connect_send to %s for pipe %s and "
			  "user %s\\%s failed\n",
			  socket_np_dir, pipe_name, info3->base.domain.string,
			  info3->base.account_name.string));
		goto fail;
	}
	ok = tevent_req_poll(subreq, ev);
	unbecome_root();
	if (!ok) {
		DEBUG(0, ("tevent_req_poll to %s for pipe %s and user %s\\%s "
			  "failed for tstream_npa_connect: %s\n",
			  socket_np_dir, pipe_name, info3->base.domain.string,
			  info3->base.account_name.string,
			  strerror(errno)));
		goto fail;

	}
	ret = tstream_npa_connect_recv(subreq, &sys_errno,
				       result,
				       &result->npipe,
				       &result->file_type,
				       &result->device_state,
				       &result->allocation_size);
	TALLOC_FREE(subreq);
	if (ret != 0) {
		DEBUG(0, ("tstream_npa_connect_recv  to %s for pipe %s and "
			  "user %s\\%s failed: %s\n",
			  socket_np_dir, pipe_name, info3->base.domain.string,
			  info3->base.account_name.string,
			  strerror(sys_errno)));
		goto fail;
	}

	return result;

 fail:
	TALLOC_FREE(result);
	return NULL;
}

static NTSTATUS rpc_pipe_open_external(TALLOC_CTX *mem_ctx,
				const char *pipe_name,
				const struct ndr_syntax_id *abstract_syntax,
				struct auth_serversupplied_info *server_info,
				struct rpc_pipe_client **_result)
{
	struct tsocket_address *local, *remote;
	struct rpc_pipe_client *result = NULL;
	struct np_proxy_state *proxy_state = NULL;
	struct pipe_auth_data *auth;
	NTSTATUS status;
	int ret;

	/* this is an internal connection, fake up ip addresses */
	ret = tsocket_address_inet_from_strings(talloc_tos(), "ip",
						NULL, 0, &local);
	if (ret) {
		return NT_STATUS_NO_MEMORY;
	}
	ret = tsocket_address_inet_from_strings(talloc_tos(), "ip",
						NULL, 0, &remote);
	if (ret) {
		return NT_STATUS_NO_MEMORY;
	}

	proxy_state = make_external_rpc_pipe_p(mem_ctx, pipe_name,
						local, remote, server_info);
	if (!proxy_state) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	result = talloc_zero(mem_ctx, struct rpc_pipe_client);
	if (result == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	result->abstract_syntax = *abstract_syntax;
	result->transfer_syntax = ndr_transfer_syntax;

	result->desthost = get_myname(result);
	result->srv_name_slash = talloc_asprintf_strupper_m(
		result, "\\\\%s", result->desthost);
	if ((result->desthost == NULL) || (result->srv_name_slash == NULL)) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	result->max_xmit_frag = RPC_MAX_PDU_FRAG_LEN;
	result->max_recv_frag = RPC_MAX_PDU_FRAG_LEN;

	status = rpc_transport_tstream_init(result,
					    proxy_state->npipe,
					    proxy_state->read_queue,
					    proxy_state->write_queue,
					    &result->transport);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	result->auth = talloc_zero(result, struct pipe_auth_data);
	if (!result->auth) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}
	result->auth->auth_type = DCERPC_AUTH_TYPE_NONE;
	result->auth->auth_level = DCERPC_AUTH_LEVEL_NONE;

	status = rpccli_anon_bind_data(result, &auth);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("Failed to initialize anonymous bind.\n"));
		goto done;
	}

	status = rpc_pipe_bind(result, auth);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("Failed to bind spoolss pipe.\n"));
		goto done;
	}
done:
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(result);
	}
	TALLOC_FREE(proxy_state);
	*_result = result;
	return status;
}

/**
 * @brief Create a new RPC client context which uses a local dispatch function.
 *
 * @param mem_ctx	The memory context on which thje pipe will ultimately
 *			be allocated
 * @param name		The pipe name to connect to.
 * @param server_info	Credentials to use for the connection.
 * @param pipe		[in|out] Checks if a pipe is connected, and connects it
 *				 if not
 *
 * @return              NT_STATUS_OK on success, a corresponding NT status if
 *			an error occured.
 */

NTSTATUS rpc_pipe_open_interface(TALLOC_CTX *mem_ctx,
				 const struct ndr_syntax_id *syntax,
				 struct auth_serversupplied_info *server_info,
				 struct client_address *client_id,
				 struct messaging_context *msg_ctx,
				 struct rpc_pipe_client **cli_pipe)
{
	struct rpc_pipe_client *cli = NULL;
	const char *server_type;
	const char *pipe_name;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx;

	if (cli_pipe && rpccli_is_connected(*cli_pipe)) {
		return NT_STATUS_OK;
	} else {
		TALLOC_FREE(*cli_pipe);
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	pipe_name = get_pipe_name_from_syntax(tmp_ctx, syntax);
	if (pipe_name == NULL) {
		status = NT_STATUS_INVALID_PARAMETER;
		goto done;
	}

	DEBUG(10, ("Connecting to %s pipe.\n", pipe_name));

	server_type = lp_parm_const_string(GLOBAL_SECTION_SNUM,
					   "rpc_server", pipe_name,
					   "embedded");
	if (StrCaseCmp(server_type, "embedded") == 0) {
		status = rpc_pipe_open_internal(tmp_ctx,
						syntax, server_info,
						client_id, msg_ctx,
						&cli);
		if (!NT_STATUS_IS_OK(status)) {
			goto done;
		}
	} else {
		/* It would be nice to just use rpc_pipe_open_ncalrpc() but
		 * for now we need to use the special proxy setup to connect
		 * to spoolssd. */

		status = rpc_pipe_open_external(tmp_ctx,
						pipe_name, syntax,
						server_info,
						&cli);
		if (!NT_STATUS_IS_OK(status)) {
			goto done;
		}
	}

	status = NT_STATUS_OK;
done:
	if (NT_STATUS_IS_OK(status)) {
		*cli_pipe = talloc_move(mem_ctx, &cli);
	}
	TALLOC_FREE(tmp_ctx);
	return status;
}