/*
 *  Copyright (c) 2017, Peter Haag
 *  Copyright (c) 2018, SURFnet bv
 *  Copyright (c) 2018, Roland van Rijswijk-Deij
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without 
 *  modification, are permitted provided that the following conditions are met:
 *  
 *   * Redistributions of source code must retain the above copyright notice, 
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice, 
 *     this list of conditions and the following disclaimer in the documentation 
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be 
 *     used to endorse or promote products derived from this software without 
 *     specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 *  POSSIBILITY OF SUCH DAMAGE.
 *  
 */

/*
 * TODO:
 * Unfortunately, the Apache Avro format only supports signed integer values,
 * which is probably an annoying habit inherited from its roots in the Java
 * ecosystem. This means that the "int" and "long" values, in which, for
 * example, the number of bytes or packets are encoded will flip to negative
 * numbers if the highest bit is set. While this seems unlikely, if it occurs
 * we may need to re-think how to encode these values in Avro files.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <avro.h>
#include <assert.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include "nffile.h"
#include "util.h"
#include "nf_common.h"
#include "export_avro.h"

#ifdef NSEL
#warning "NSEL support not yet implemented for Apache Avro export"
#endif

/*
 * The Avro codec to use. The default setting is to use the gzip codec,
 * which is the most space efficient and is always included in builds of
 * the Avro library. If need be, and if it is available in your local build
 * of the Avro library, you can also choose snappy, which will perform
 * markedly better in terms of speed.
 */
#define DEFAULT_AVRO_CODEC	"deflate"

/*
 * The Avro writer block size; you may need to increase this if
 * the writer fails to write records with the following error
 * message:
 *
 * "Value too large for file block size"
 */
#define AVRO_BLOCKSIZE		65536

/* Include the Avro schema */
#include "flowdata_avsc.inc"

static avro_schema_t            flowavro_schema;
static avro_file_writer_t       flowavro_writer;
static avro_value_iface_t*      flowavro_class;
static avro_value_t             flowavro_single_record;

static int open_avro_file(const char *output_filename)
{
	memset(&flowavro_writer, 0, sizeof(avro_file_writer_t));

	return avro_file_writer_create_with_codec(output_filename, flowavro_schema, &flowavro_writer, DEFAULT_AVRO_CODEC, AVRO_BLOCKSIZE);
}

static void finish_avro_file(void)
{
	avro_file_writer_close(flowavro_writer);
}

int init_avro_export(const char *output_filename)
{
	/* Load the Avro schema */
	memset(&flowavro_schema, 0, sizeof(flowavro_schema));

	if (avro_schema_from_json_length((const char*) flowdata_avsc, flowdata_avsc_len, &flowavro_schema) != 0)
	{
		return -1;
	}

	/*
	 * Instantiate a single instance of the schema. We will re-use a
	 * single record instance for the writer, as suggested in section 4
	 * of the libavro documentation.
	 */
	flowavro_class = avro_generic_class_from_schema(flowavro_schema);

	if (flowavro_class == NULL)
	{
		return -1;
	}

	if (avro_generic_value_new(flowavro_class, &flowavro_single_record) != 0)
	{
		return -1;
	}

	return open_avro_file(output_filename);
}

/* Wipe the single Avro record instance we re-use */
static void wipe_avro_record(void)
{
	size_t	field_ct	= 0;
	size_t	i		= 0;

	assert(avro_value_reset(&flowavro_single_record) == 0);
	assert(avro_value_get_size(&flowavro_single_record, &field_ct) == 0);

	for (i = 0; i < field_ct; i++)
	{
		avro_value_t	rec_field;
		avro_value_t	rec_branch;
		int		rv;

		assert(avro_value_get_by_index(&flowavro_single_record, i, &rec_field, NULL) == 0);

		/*
		 * The schema is set up such that all the optional fields have
		 * two branches, the first of which is a 'null' value. We need
		 * to properly initialise this value because the C version of
		 * libavro doesn't do this.
		 */
		rv = avro_value_set_branch(&rec_field, 0, &rec_branch);
		assert((rv == 0) || (rv == EINVAL));

		if (rv == EINVAL)
		{
			/* This value is not a union, so we can skip it */
			continue;
		}

		assert(avro_value_set_null(&rec_branch) == 0);
	}
}

/*
 * General remark about record setter functions:
 * We use assert(..) to check the results, since these setter functions should
 * in principle always succeed. If they don't, this means something is wrong
 * with the Avro schema, and since that is included in the build, it needs a
 * fix in the code. Dropping out with an assert(..) failure is then a safe
 * solution.
 *
 * Also: there are two versions of each setter function, one that sets a value
 * in a fixed field, and one that sets a value in an optional field (an Avro
 * union).
 */
static void avro_set_long_field(const char *fieldname, long long value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_long(&rec_field, value) == 0);
}

static void avro_set_long_union(const char *fieldname, long long value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_long(&rec_branch, value) == 0);
}

static void avro_set_int_field(const char *fieldname, int value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_int(&rec_field, value) == 0);
}

static void avro_set_int_union(const char *fieldname, int value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_int(&rec_branch, value) == 0);
}

static void avro_set_boolean_field(const char *fieldname, int value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_boolean(&rec_field, value) == 0);
}

/*
 * CURRENTLY UNUSED, BUT MAY BE NEEDED IN THE FUTURE
static void avro_set_boolean_union(const char *fieldname, int value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_boolean(&rec_branch, value) == 0);
}
 */

/*
 * CURRENTLY UNUSED, BUT MAY BE NEEDED IN THE FUTURE
static void avro_set_double_field(const char *fieldname, double value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_double(&rec_field, value) == 0);
}
 */

static void avro_set_double_union(const char *fieldname, double value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_double(&rec_branch, value) == 0);
}

static void avro_set_string_field(const char *fieldname, const char *value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_string(&rec_field, value) == 0);
}

static void avro_set_string_union(const char *fieldname, const char *value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_string(&rec_branch, value) == 0);
}

/* Convert TCP flags to a string, based on the JSON outputter */
static void tcp_flags_to_str(master_record_t *r, char *string) 
{
	// if record contains unusual flags, print the flags in hex as 0x.. number
	if ( r->tcp_flags > 63 ) 
	{
		snprintf(string, 7, "0x%2x", r->tcp_flags );
	} 
	else 
	{
		snprintf(string, 7, "%c%c%c%c%c%c",
			r->tcp_flags & 32 ? 'U' : '.',
			r->tcp_flags & 16 ? 'A' : '.',
			r->tcp_flags &  8 ? 'P' : '.',
			r->tcp_flags &  4 ? 'R' : '.',
			r->tcp_flags &  2 ? 'S' : '.',
			r->tcp_flags &  1 ? 'F' : '.');
	}
}

/* Output a flow record to the Avro file; based on the JSON outputter */
void flow_record_to_avro(void *record)
{
	master_record_t	*r		= (master_record_t *) record;
	extension_map_t	*extension_map	= r->map_ref;
	long long	ts		= 0L;
	char		tcp_flags[16]	= { 0 };
	int		i		= 0;
	int		id		= 0;

	/* Start with a clean slate */
	wipe_avro_record();

	/* Start time of the flow in milliseconds */
	ts = (r->first * 1000L) + r->msec_first;
	avro_set_long_field("start_ts", ts);

	/* End time of the flow in milliseconds */
	ts = (r->last * 1000L) + r->msec_last;
	avro_set_long_field("end_ts", ts);

	/* Flow type */
	avro_set_string_field("type", TestFlag(r->flags, FLAG_EVENT) ? "EVENT" : "FLOW");

	/* Is the flow sampled? */
	avro_set_boolean_field("sampled", TestFlag(r->flags, FLAG_SAMPLED) ? 1 : 0);

	/* The system ID of the flow exporter */
	avro_set_long_field("export_sysid", r->exporter_sysid);

	/* The protocol */
	avro_set_int_field("protocol", r->prot);

	/* Source and destination IP */
	if (TestFlag(r->flags,FLAG_IPV6_ADDR ) != 0)
	{
		/* This is an IPv6 flow */
		uint64_t	_src[2]			= { 0 };
		uint64_t	_dst[2]			= { 0 };
		char		ipstr[INET6_ADDRSTRLEN]	= { 0 };

		_src[0] = htonll(r->V6.srcaddr[0]);
		_src[1] = htonll(r->V6.srcaddr[1]);
		_dst[0] = htonll(r->V6.dstaddr[0]);
		_dst[1] = htonll(r->V6.dstaddr[1]);

		assert(inet_ntop(AF_INET6, _src, ipstr, INET6_ADDRSTRLEN) != NULL);

		avro_set_string_union("src_v6_str", ipstr);
		avro_set_long_union("src_v6_int_hi", r->V6.srcaddr[0]);
		avro_set_long_union("src_v6_int_lo", r->V6.srcaddr[1]);

		assert(inet_ntop(AF_INET6, _dst, ipstr, INET6_ADDRSTRLEN) != NULL);

		avro_set_string_union("dst_v6_str", ipstr);
		avro_set_long_union("dst_v6_int_hi", r->V6.dstaddr[0]);
		avro_set_long_union("dst_v6_int_lo", r->V6.dstaddr[1]);
	} 
	else 
	{
		/* This is an IPv4 flow */
		uint32_t	_src			= 0;
		uint32_t	_dst			= 0;
		char		ipstr[INET_ADDRSTRLEN]	= { 0 };

		_src = htonl(r->V4.srcaddr);
		_dst = htonl(r->V4.dstaddr);

		assert(inet_ntop(AF_INET, &_src, ipstr, INET_ADDRSTRLEN) != NULL);

		avro_set_string_union("src_v4_str", ipstr);
		avro_set_int_union("src_v4_int", r->V4.srcaddr);

		assert(inet_ntop(AF_INET, &_dst, ipstr, INET_ADDRSTRLEN) != NULL);

		avro_set_string_union("dst_v4_str", ipstr);
		avro_set_int_union("dst_v4_int", r->V4.dstaddr);
	}
	
	/* ICMP information or source and destination port */
	if ( r->prot == IPPROTO_ICMP || r->prot == IPPROTO_ICMPV6 ) 
	{ 
		avro_set_int_union("icmp_type", r->icmp_type);
		avro_set_int_union("icmp_code", r->icmp_code);
	} 
	else 
	{
		avro_set_int_union("src_port", r->srcport);
		avro_set_int_union("dst_port", r->dstport);
	}

	/* Forwarding status */
	avro_set_int_field("fwd_status", r->fwd_status);
	
	/* TCP flags */
	tcp_flags_to_str(r, tcp_flags);
	avro_set_string_field("tcp_flags", tcp_flags);

	/* Source TOS */
	avro_set_int_field("src_tos", r->tos);

	/* Number of packets in the flow */
	avro_set_long_field("in_packets", r->dPkts);

	/* Number of bytes in the flow */
	avro_set_long_field("in_bytes", r->dOctets);

	/* Process extension fields */
	i = 0;
	while ((id = extension_map->ex_id[i]) != 0) 
	{
		switch(id) 
		{
		case EX_IO_SNMP_2:
		case EX_IO_SNMP_4:
				avro_set_int_union("input_snmp", r->input);
				avro_set_int_union("output_snmp", r->output);
				break;
		case EX_AS_2:
		case EX_AS_4:
				avro_set_int_union("src_as", r->srcas);
				avro_set_int_union("dst_as", r->dstas);
				break;
		case EX_BGPADJ:
				avro_set_int_union("next_as", r->bgpNextAdjacentAS);
				avro_set_int_union("prev_as", r->bgpPrevAdjacentAS);
				break;
		case EX_MULIPLE:
				avro_set_int_union("src_mask", r->src_mask);
				avro_set_int_union("dst_mask", r->dst_mask);
				avro_set_int_union("dst_tos", r->dst_tos);
				avro_set_int_union("direction", r->dir);
				break;
		case EX_NEXT_HOP_v4: 
			{
				uint32_t	_ip			= 0;
				char		ipstr[INET_ADDRSTRLEN]	= { 0 };

				_ip = htonl(r->ip_nexthop.V4);

				assert(inet_ntop(AF_INET, &_ip, ipstr, INET_ADDRSTRLEN) != NULL);

				avro_set_string_union("ip4_next_hop_str", ipstr);
			} 
			break;
		case EX_NEXT_HOP_v6: 
			{
				uint64_t	_ip[2]			= { 0 };
				char		ipstr[INET6_ADDRSTRLEN]	= { 0 };

				_ip[0] = htonll(r->ip_nexthop.V6[0]);
				_ip[1] = htonll(r->ip_nexthop.V6[1]);

				assert(inet_ntop(AF_INET6, _ip, ipstr, INET6_ADDRSTRLEN) != NULL);

				avro_set_string_union("ip6_next_hop_str", ipstr);
			} 
			break;
		case EX_NEXT_HOP_BGP_v4: 
			{
				uint32_t	_ip			= 0;
				char		ipstr[INET_ADDRSTRLEN]	= { 0 };

				_ip = htonl(r->bgp_nexthop.V4);

				assert(inet_ntop(AF_INET, &_ip, ipstr, INET_ADDRSTRLEN) != NULL);

				avro_set_string_union("bgp4_next_hop_str", ipstr);
			} 
			break;
		case EX_NEXT_HOP_BGP_v6: 
			{
				uint64_t	_ip[2]			= { 0 };
				char		ipstr[INET6_ADDRSTRLEN]	= { 0 };

				_ip[0] = htonll(r->bgp_nexthop.V6[0]);
				_ip[1] = htonll(r->bgp_nexthop.V6[1]);

				assert(inet_ntop(AF_INET6, _ip, ipstr, INET6_ADDRSTRLEN) != NULL);

				avro_set_string_union("bgp6_next_hop_str", ipstr);
			} 
			break;
		case EX_VLAN:
			avro_set_int_union("src_vlan", r->src_vlan);
			avro_set_int_union("dst_vlan", r->dst_vlan);
			break;
		case EX_OUT_PKG_4:
		case EX_OUT_PKG_8:
			avro_set_long_union("out_packets", r->out_pkts);
			break;
		case EX_OUT_BYTES_4:
		case EX_OUT_BYTES_8:
			avro_set_long_union("out_bytes", r->out_bytes);
			break;
		case EX_AGGR_FLOWS_4:
		case EX_AGGR_FLOWS_8:
			avro_set_long_union("aggr_flows", r->aggr_flows);
			break;
		case EX_MAC_1: 
			{
				int	j	= 0;
				char	mac1_str[(6 * 3) + 2]	= { 0 };
				char	mac2_str[(6 * 3) + 2]	= { 0 };

				for (j = 0; j < 6; j++)
				{
					snprintf(&mac1_str[j*3], 4, "%02x:", (unsigned int) (r->in_src_mac >> (j*8)) & 0xFF);
					snprintf(&mac2_str[j*3], 4, "%02x:", (unsigned int) (r->out_dst_mac >> (j*8)) & 0xFF);
				}

				mac1_str[(6*3)-1] = '\0';
				mac2_str[(6*3)-1] = '\0';

				avro_set_string_union("in_src_mac", mac1_str);
				avro_set_string_union("out_dst_mac", mac2_str);
			}
			break;
		case EX_MAC_2: 
			{
				int	j	= 0;
				char	mac1_str[(6 * 3) + 2]	= { 0 };
				char	mac2_str[(6 * 3) + 2]	= { 0 };

				for (j = 0; j < 6; j++)
				{
					snprintf(&mac1_str[j*3], 4, "%02x:", (unsigned int) (r->in_dst_mac >> (j*8)) & 0xFF);
					snprintf(&mac2_str[j*3], 4, "%02x:", (unsigned int) (r->out_src_mac >> (j*8)) & 0xFF);
				}

				mac1_str[(6*3)-1] = '\0';
				mac2_str[(6*3)-1] = '\0';

				avro_set_string_union("in_dst_mac", mac1_str);
				avro_set_string_union("out_src_mac", mac2_str);
			}
			break;
		case EX_MPLS:
			{
				/* FIXME:
				 * Having a fixed number of MPLS labels seems
				 * a bit of a kludge. If that ever changes,
				 * this code will need to be update
				 * accordingly.
				 */
				int	j		= 0;
				char	label_str[14]	= { 0 };
				char	label_val[64]	= { 0 };

				for (j = 0; j < 10; j++)
				{
					snprintf(label_str, 14, "mpls_label_%02d", j + 1);
					snprintf(label_val, 64, "%u-%u-%u", 
						r->mpls_label[j] >> 4,
						(r->mpls_label[i] & 0xF ) >> 1,
						r->mpls_label[i] & 1);

					avro_set_string_union(label_str, label_val);
				}
			}
			break;
		case EX_ROUTER_IP_v4:
			{
				uint32_t	_ip			= 0;
				char		ipstr[INET_ADDRSTRLEN]	= { 0 };

				_ip = htonl(r->ip_router.V4);

				assert(inet_ntop(AF_INET, &_ip, ipstr, INET_ADDRSTRLEN) != NULL);

				avro_set_string_union("ip4_router_str", ipstr);
			}
			break;
		case EX_ROUTER_IP_v6:
			{
				uint64_t	_ip[2]			= { 0 };
				char		ipstr[INET6_ADDRSTRLEN]	= { 0 };

				_ip[0] = htonll(r->ip_router.V6[0]);
				_ip[1] = htonll(r->ip_router.V6[1]);

				assert(inet_ntop(AF_INET6, _ip, ipstr, INET6_ADDRSTRLEN) != NULL);

				avro_set_string_union("ip6_router_str", ipstr);
			}
			break;
		case EX_LATENCY: 
			{
				double client_latency	= 0.0f;
				double server_latency	= 0.0f;
				double app_latency	= 0.0f;

				client_latency = (double) r->client_nw_delay_usec / 1000.0f;
				server_latency = (double) r->server_nw_delay_usec / 1000.0f;
				app_latency = (double) r->appl_latency_usec / 1000.0f;

				avro_set_double_union("cli_latency", client_latency);
				avro_set_double_union("srv_latency", server_latency);
				avro_set_double_union("app_latency", app_latency);
			}
			break;
		case EX_ROUTER_ID:
			avro_set_int_union("engine_type", r->engine_type);
			avro_set_int_union("engine_id", r->engine_id);
			break;
		case EX_RECEIVED:
			avro_set_long_union("t_received", r->received);
			break;
		}

		/* Process next extension */
		i++;
	}

	/* Finally, add label */
	if (r->label)
	{
		avro_set_string_union("label", r->label);
	}

	/* Output Avro record to file */
	/* TODO: using an assert may not be the nicest way to do this, if
	 *       merged, this may need to be changed to return an error
	 *       that is caught in the main nfdump program and gets
	 *       reported to users
	 */
	assert(avro_file_writer_append_value(flowavro_writer, &flowavro_single_record) == 0);
}

void finish_avro_export(void)
{
	/* Close the output file */
	finish_avro_file();

	/* Free the single record instance */
	avro_value_decref(&flowavro_single_record);

	/* Free the generic class instance */
	avro_value_iface_decref(flowavro_class);

	/* Free the Avro schema */
	avro_schema_decref(flowavro_schema);
}

