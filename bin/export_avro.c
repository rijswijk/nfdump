/*
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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <avro.h>
#include <assert.h>
#include "export_avro.h"

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
static void avro_set_long_field(const char *fieldname, long value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_long(&rec_field, value) == 0);
}

static void avro_set_long_union(const char *fieldname, long value)
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

static void avro_set_boolean_union(const char *fieldname, int value)
{
	avro_value_t	rec_field;
	avro_value_t	rec_branch;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_branch(&rec_field, 1, &rec_branch) == 0);
	assert(avro_value_set_boolean(&rec_branch, value) == 0);
}

static void avro_set_double_field(const char *fieldname, double value)
{
	avro_value_t	rec_field;

	assert(avro_value_get_by_name(&flowavro_single_record, fieldname, &rec_field, NULL) == 0);
	assert(avro_value_set_double(&rec_field, value) == 0);
}

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

void flow_record_to_avro(void *record)
{
	/* Start with a clean slate */
	wipe_avro_record();

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

