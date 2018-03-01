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
#define DJ_AVRO_BLOCKSIZE       65536

/* Include the Avro schema */
#include "flowdata_avsc.inc"

static avro_schema_t            flowavro_schema;
static avro_file_writer_t       flowavro_writer;
static avro_value_iface_t*      flowavro_class;
static avro_value_t             flowavro_single_record;

int init_avro_export(const char *output_filename)
{
	/* Load the Avro schema */
	if (avro_schema_from_json_length((const char*) flowdata_avsc, flowdata_avsc_len, &flowavro_schema) != 0)
	{
		return -1;
	}

	return 0;
}

void flow_record_to_avro(void *record)
{
}

void finish_avro_export(void)
{
}

