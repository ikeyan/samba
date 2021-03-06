/* 
   Unix SMB/CIFS implementation.

   scanner for rpc calls

   Copyright (C) Andrew Tridgell 2003
   
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
#include "librpc/gen_ndr/ndr_mgmt_c.h"
#include "librpc/ndr/ndr_table.h"
#include "torture/rpc/torture_rpc.h"
#include "param/param.h"

/*
  work out how many calls there are for an interface
 */
static bool test_num_calls(struct torture_context *tctx, 
			   const struct ndr_interface_table *iface,
			   TALLOC_CTX *mem_ctx,
			   struct ndr_syntax_id *id)
{
	struct dcerpc_pipe *p;
	NTSTATUS status;
	unsigned int i;
	DATA_BLOB stub_in, stub_out;
	struct ndr_interface_table _tbl;
	const struct ndr_interface_table *tbl;

	/* FIXME: This should be fixed when torture_rpc_connection 
	 * takes a ndr_syntax_id */
	tbl = ndr_table_by_syntax(id);
	if (tbl == NULL) {
		_tbl = *iface;
		_tbl.name = "__unknown__";
		_tbl.syntax_id = *id;
		_tbl.num_calls = UINT32_MAX;
		tbl = &_tbl;
	}

	status = torture_rpc_connection(tctx, &p, tbl);
	if (!NT_STATUS_IS_OK(status)) {
		char *uuid_str = GUID_string(mem_ctx, &id->uuid);
		printf("Failed to connect to '%s' on '%s' - %s\n", 
		       uuid_str, iface->name, nt_errstr(status));
		talloc_free(uuid_str);
		return true;
	}

	/* make null calls */
	stub_in = data_blob_talloc(mem_ctx, NULL, 1000);
	memset(stub_in.data, 0xFF, stub_in.length);

	for (i=0;i<200;i++) {
		bool ok;
		uint32_t out_flags = 0;

		status = dcerpc_binding_handle_raw_call(p->binding_handle,
							NULL, i,
							0, /* in_flags */
							stub_in.data,
							stub_in.length,
							mem_ctx,
							&stub_out.data,
							&stub_out.length,
							&out_flags);
		ok = dcerpc_binding_handle_is_connected(p->binding_handle);
		if (!ok) {
			printf("\tpipe disconnected at %u\n", i);
			goto done;
		}

		if (NT_STATUS_EQUAL(status, NT_STATUS_RPC_PROCNUM_OUT_OF_RANGE)) {
			break;
		}

		if (NT_STATUS_EQUAL(status, NT_STATUS_ACCESS_DENIED)) {
			printf("\taccess denied at %u\n", i);
			goto done;
		}

		if (NT_STATUS_EQUAL(status, NT_STATUS_RPC_PROTOCOL_ERROR)) {
			printf("\tprotocol error at %u\n", i);
		}
	}

	printf("\t%d calls available\n", i);
	if (tbl->num_calls == UINT32_MAX) {
		printf("\tinterface not known in local IDL\n");
	} else if (tbl->num_calls != i) {
		printf("\tWARNING: local IDL defines %u calls\n",
			(unsigned int)tbl->num_calls);
	} else {
		printf("\tOK: matches num_calls in local IDL\n");
	}

done:
	talloc_free(p);
	return true;
}



bool torture_rpc_scanner(struct torture_context *torture)
{
        NTSTATUS status;
        struct dcerpc_pipe *p;
	TALLOC_CTX *loop_ctx;
	bool ret = true;
	const struct ndr_interface_list *l;
	struct dcerpc_binding *b;
	enum dcerpc_transport_t transport;

	status = torture_rpc_binding(torture, &b);
	if (!NT_STATUS_IS_OK(status)) {
		return false;
	}
	transport = dcerpc_binding_get_transport(b);

	for (l=ndr_table_list();l;l=l->next) {		
		loop_ctx = talloc_named(torture, 0, "torture_rpc_scanner loop context");
		/* some interfaces are not mappable */
		if (l->table->num_calls == 0 ||
		    strcmp(l->table->name, "mgmt") == 0) {
			talloc_free(loop_ctx);
			continue;
		}

		printf("\nTesting pipe '%s'\n", l->table->name);

		if (transport == NCACN_IP_TCP) {
			status = dcerpc_epm_map_binding(torture, b, l->table,
							torture->ev,
							torture->lp_ctx);
			if (!NT_STATUS_IS_OK(status)) {
				printf("Failed to map port for uuid %s\n", 
					   GUID_string(loop_ctx, &l->table->syntax_id.uuid));
				talloc_free(loop_ctx);
				continue;
			}
		} else {
			status = dcerpc_binding_set_string_option(b, "endpoint",
								  l->table->name);
			if (!NT_STATUS_IS_OK(status)) {
				talloc_free(loop_ctx);
				ret = false;
				continue;
			}
			status = dcerpc_binding_set_abstract_syntax(b,
							&l->table->syntax_id);
			if (!NT_STATUS_IS_OK(status)) {
				talloc_free(loop_ctx);
				ret = false;
				continue;
			}
		}

		lpcfg_set_cmdline(torture->lp_ctx, "torture:binding", dcerpc_binding_string(torture, b));

		status = torture_rpc_connection(torture, &p, &ndr_table_mgmt);
		if (!NT_STATUS_IS_OK(status)) {
			talloc_free(loop_ctx);
			ret = false;
			continue;
		}
	
		if (!test_inq_if_ids(torture, p->binding_handle, torture, test_num_calls, l->table)) {
			ret = false;
		}
	}

	return ret;
}

