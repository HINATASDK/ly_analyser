/*
 *  Copyright (c) 2009, Peter Haag
 *  Copyright (c) 2004-2008, SWITCH - Teleinformatikdienste fuer Lehre und Forschung
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
 *   * Neither the name of SWITCH nor the names of its contributors may be 
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
 *  $Author: haag $
 *
 *  $Id: nfdump.c 69 2010-09-09 07:17:43Z haag $
 *
 *  $LastChangedRevision: 69 $
 *	
 *
 */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include "nffile.h"
#include "nfx.h"
#include "nfnet.h"
#include "bookkeeper.h"
#include "nfxstat.h"
#include "collector.h"
#include "exporter.h"
#include "nf_common.h"
#include "netflow_v5_v7.h"
#include "netflow_v9.h"
#include "rbtree.h"
#include "nftree.h"
#include "nfprof.h"
#include "nfdump.h"
#include "nflowcache.h"
#include "nfstat.h"
#include "nfexport.h"
#include "ipconv.h"
#include "util.h"
#include "flist.h"
#include "strmap.h"
#include "lmdb.h"

/* hash parameters */
#define NumPrealloc 128000

#define AGGR_SIZE 7

/* Global Variables */
FilterEngine_data_t	*Engine;

extern char	*FilterFilename;
extern uint32_t loopcnt;
extern extension_descriptor_t extension_descriptor[];

/* Local Variables */
const char *nfdump_version = VERSION;

static int sep=0; // how many seconds will returning result be separated by 

static uint64_t total_bytes;
static uint32_t total_flows;
static uint32_t skipped_blocks;
static uint32_t	is_anonymized;
static time_t 	t_first_flow, t_last_flow;
static char		Ident[IDENTLEN];


int hash_hit = 0; 
int hash_miss = 0;
int hash_skip = 0;

extension_map_list_t extension_map_list;

extern generic_exporter_t **exporter_list;
/*
 * Output Formats:
 * User defined output formats can be compiled into nfdump, for easy access
 * The format has the same syntax as describe in nfdump(1) -o fmt:<format>
 *
 * A format description consists of a single line containing arbitrary strings
 * and format specifier as described below:
 *
 * 	%ts		// Start Time - first seen
 * 	%te		// End Time	- last seen
 * 	%td		// Duration
 * 	%pr		// Protocol
 * 	%sa		// Source Address
 * 	%da		// Destination Address
 * 	%sap	// Source Address:Port
 * 	%dap	// Destination Address:Port
 * 	%sp		// Source Port
 * 	%dp		// Destination Port
 *  %nh		// Next-hop IP Address
 *  %nhb	// BGP Next-hop IP Address
 * 	%sas	// Source AS
 * 	%das	// Destination AS
 * 	%in		// Input Interface num
 * 	%out	// Output Interface num
 * 	%pkt	// Packets - default input
 * 	%ipkt	// Input Packets
 * 	%opkt	// Output Packets
 * 	%byt	// Bytes - default input
 * 	%ibyt	// Input Bytes
 * 	%obyt	// Output Bytes
 * 	%fl		// Flows
 * 	%flg	// TCP Flags
 * 	%tos	// Tos - Default src
 * 	%stos	// Src Tos
 * 	%dtos	// Dst Tos
 * 	%dir	// Direction: ingress, egress
 * 	%smk	// Src mask
 * 	%dmk	// Dst mask
 * 	%fwd	// Forwarding Status
 * 	%svln	// Src Vlan
 * 	%dvln	// Dst Vlan
 * 	%ismc	// Input Src Mac Addr
 * 	%odmc	// Output Dst Mac Addr
 * 	%idmc	// Output Src Mac Addr
 * 	%osmc	// Input Dst Mac Addr
 * 	%mpls1	// MPLS label 1
 * 	%mpls2	// MPLS label 2
 * 	%mpls3	// MPLS label 3
 * 	%mpls4	// MPLS label 4
 * 	%mpls5	// MPLS label 5
 * 	%mpls6	// MPLS label 6
 * 	%mpls7	// MPLS label 7
 * 	%mpls8	// MPLS label 8
 * 	%mpls9	// MPLS label 9
 * 	%mpls10	// MPLS label 10
 *
 * 	%bps	// bps - bits per second
 * 	%pps	// pps - packets per second
 * 	%bpp	// bps - Bytes per package
 *
 * The nfdump standard output formats line, long and extended are defined as follows:
 */

#define FORMAT_line "%ts %td %pr %sap -> %dap %pkt %byt %fl"

#define FORMAT_long "%ts %td %pr %sap -> %dap %flg %tos %pkt %byt %fl"

#define FORMAT_extended "%ts %td %pr %sap -> %dap %flg %tos %pkt %byt %pps %bps %bpp %fl"

#define FORMAT_biline "%ts %td %pr %sap <-> %dap %opkt %ipkt %obyt %ibyt %fl"

#define FORMAT_bilong "%ts %td %pr %sap <-> %dap %flg %tos %opkt %ipkt %obyt %ibyt %fl"

/* The appropriate header line is compiled automatically.
 *
 * For each defined output format a v6 long format automatically exists as well e.g.
 * line -> line6, long -> long6, extended -> extended6
 * v6 long formats need more space to print IP addresses, as IPv6 addresses are printed in full length,
 * where as in standard output format IPv6 addresses are condensed for better readability.
 * 
 * Define your own output format and compile it into nfdumnp:
 * 1. Define your output format string.
 * 2. Test the format using standard syntax -o "fmt:<your format>"
 * 3. Create a #define statement for your output format, similar than the standard output formats above.
 * 4. Add another line into the printmap[] struct below BEFORE the last NULL line for you format:
 *    { "formatname", format_special, FORMAT_definition, NULL },
 *   The first parameter is the name of your format as recognized on the command line as -o <formatname>
 *   The second parameter is always 'format_special' - the printing function.
 *   The third parameter is your format definition as defined in #define.
 *   The forth parameter is always NULL for user defined formats.
 * 5. Recompile nfdump
 */

// Assign print functions for all output options -o
// Teminated with a NULL record
printmap_t printmap[] = {
	{ "raw",		format_file_block_record,  	NULL 			},
	{ "line", 		format_special,      		FORMAT_line 	},
	{ "long", 		format_special, 			FORMAT_long 	},
	{ "extended",	format_special, 			FORMAT_extended	},
	{ "biline", 	format_special,      		FORMAT_biline 	},
	{ "bilong", 	format_special,      		FORMAT_bilong 	},
	{ "pipe", 		flow_record_to_pipe,      	NULL 			},
	{ "csv", 		flow_record_to_csv,      	NULL 			},
// add your formats here

// This is always the last line
	{ NULL,			NULL,                       NULL			}
};

#define DefaultMode "line"

// For automatic output format generation in case of custom aggregation
#define AggrPrependFmt	"%ts %td "
#define AggrAppendFmt	"%pkt %byt %bps %bpp %fl"

// compare at most 16 chars
#define MAXMODELEN	16	

/* Function Prototypes */
static void usage(char *name);

static void PrintSummary(stat_record_t *stat_record, int plain_numbers, int csv_output);

/* Functions */

#include "nfdump_inline.c"
#include "nffile_inline.c"


static void OutputAndDestroy(int aggregate, char* print_order, char* wfile, int compress, int bidir,
    int date_sorted, char *Ident, printer_t print_record, uint32_t limitflows, int  do_tag,
    int GuessDir, int flow_stat, char* record_header, int topN, int quiet, int pipe_output,
    int csv_output, stat_record_t* sum_stat, int plain_numbers, nfprof_t* profile_data, int element_stat, int orig_plain_numbers);

static stat_record_t process_data(char *wfile, int element_stat, int flow_stat, int sort_flows,
	printer_t print_header, printer_t print_record, time_t twin_start, time_t twin_end, 
	uint64_t limitflows, int tag, int compress, int do_xstat,
// added by lxh start
    int new_aggregate, char* print_order, int bidir,
    int date_sorted, char *newIdent, int  do_tag,
    int GuessDir, int new_flow_stat, char* record_header, int topN, int quiet, int pipe_output,
    int csv_output, int plain_numbers, nfprof_t*  profile_data, int orig_plain_numbers);
// added_by lxh end


StrMap *service_map = NULL;
StrMap *scanner_map = NULL;
StrMap *popular_service_map = NULL;
StrMap *whitelist_map = NULL;
StrMap *blacklist_map = NULL;
static int exclude_services = 0;
static int services_only = 0;
static int exclude_scanners = 0;
static int scanners_only = 0;
static int exclude_popular_services = 0;
static int popular_services_only = 0;
static int exclude_whitelist = 0;
static int whitelist_only = 0;
static int exclude_blacklist = 0;
static int blacklist_only = 0;
static int exclude_xdayypercent = 0;
static int xdayypercent_only = 0;
#define MDB_DATA_DIR "/Agent/data/mdb"
static char service_db_path[] = MDB_DATA_DIR "/service";
static char scanner_db_path[] = MDB_DATA_DIR "/scanner";
static char service_file[] = "/Agent/data/service";
static char scanner_file[] = "/Agent/data/scanner";
static char popular_service_file[] = "/Agent/data/popular_service";
static char whitelist_file[] = "/Agent/data/whitelist";
static char blacklist_file[] = "/Agent/data/blacklist";
static char xdayypercent_db_path[] = MDB_DATA_DIR "/xdayypercent";
static char xdayypercent_db_ip[] = "ip";
static char xdayypercent_db_svc[] = "svc";
static unsigned long long xdayypercent_mapsize = 1024ULL*1024*1024*10;
static unsigned int xdayypercent_db_count = 2;
typedef struct {
  MDB_env *env;
  MDB_txn *txn;
  MDB_dbi dbi_ip, dbi_svc;
  unsigned int exclusion_x, exclusion_y;
  unsigned int inclusion_x, inclusion_y;
  unsigned long long exclusion_ip_threshold, inclusion_ip_threshold;
} XDayYPercent_CTX;
static XDayYPercent_CTX xdayypercent_ctx;
typedef struct {
  MDB_env *env;
  MDB_txn *txn;
  MDB_dbi dbi_
typedef struct {
  uint32_t ip;
  uint16_t protocol, port;
} SvcKey;
typedef struct {
  uint32_t peer_ip;
  uint32_t timestamp;
} SvcValue;

#define MDB_ISOK(rc) (!rc)
#define MDB_CHECK(rc) {if (!MDB_ISOK(rc)) {fprintf(stderr, "%s:%d mdb_error: (%d) %s\n", __FILE__, __LINE__, rc, mdb_strerror(rc));}}

static int parse_xdayypercent(char *str, char *format, unsigned int *x, unsigned int *y) {
  return 2 == sscanf(str, format, x, y);
}

static unsigned long long get_xdayypercent_threshold(int x, int y) {
  XDayYPercent_CTX* ctx = & xdayypercent_ctx;
  unsigned int timestamp, now = time(NULL);
  MDB_cursor* cur;
  int rc = mdb_cursor_open(ctx->txn, ctx->dbi_ip, &cur);
  MDB_CHECK(rc);
  timestamp = now - x * 86400;
  unsigned long long count = 0;
  MDB_val dup_key, dup_value;
  rc = mdb_cursor_get(cur, &dup_key, &dup_value, MDB_FIRST);
  while (MDB_ISOK(rc)) {
    count += *(uint32_t *)dup_value.mv_data >= timestamp;
    rc = mdb_cursor_get(cur, &dup_key, &dup_value, MDB_NEXT);
  }
  if (!MDB_ISOK(rc) && (rc != MDB_NOTFOUND)) {
    MDB_CHECK(rc);
  }
  mdb_cursor_close(cur);
  return count*y/100;
}

static void build_service_db(char* db_path) {
  
static void load_xdayypercent_db(char* db_path) {
  XDayYPercent_CTX* ctx = & xdayypercent_ctx;
  int rc = mdb_env_create(&ctx->env);
  MDB_CHECK(rc);
  rc = mdb_env_set_mapsize(ctx->env, xdayypercent_mapsize);
  MDB_CHECK(rc);
  rc = mdb_env_set_maxdbs(ctx->env, xdayypercent_db_count);
  MDB_CHECK(rc);
  rc = mdb_env_open(ctx->env, db_path, MDB_NOLOCK|MDB_RDONLY, 0);
  MDB_CHECK(rc);
  rc = mdb_txn_begin(ctx->env, NULL, MDB_RDONLY, &ctx->txn);
  MDB_CHECK(rc);
  rc = mdb_dbi_open(ctx->txn, xdayypercent_db_ip, 0, &ctx->dbi_ip);
  MDB_CHECK(rc);
  rc = mdb_dbi_open(ctx->txn, xdayypercent_db_svc, MDB_DUPSORT, &ctx->dbi_svc);
  MDB_CHECK(rc);
  if (exclude_xdayypercent && ctx->exclusion_x && ctx->exclusion_y)
    ctx->exclusion_ip_threshold = get_xdayypercent_threshold(ctx->exclusion_x, ctx->exclusion_y);
  if (xdayypercent_only && ctx->inclusion_x && ctx->inclusion_y)
    ctx->inclusion_ip_threshold = get_xdayypercent_threshold(ctx->inclusion_x, ctx->inclusion_y);
  fprintf(stderr, "exclude_xdayypercent_threshold:%llu, include_xdayypercent_threshold:%llu\n", ctx->exclusion_ip_threshold, ctx->inclusion_ip_threshold);
}


static unsigned long long get_xdayypercent_svc_client_count(
  unsigned char protocol, unsigned int ip, unsigned short int port, int x, int y) {
  XDayYPercent_CTX* ctx = & xdayypercent_ctx;
  unsigned int timestamp = time(NULL);
  timestamp -= x * 86400;
  timestamp -= timestamp % 43200;
  MDB_cursor* cur;
  int rc = mdb_cursor_open(ctx->txn, ctx->dbi_svc, &cur);
  MDB_CHECK(rc);
  
  SvcKey key;
  key.ip = ip;
  key.port = port;
  key.protocol = protocol;
  MDB_val dup_key, dup_value;
  dup_key.mv_size = sizeof(key);
  dup_key.mv_data = &key;
  rc = mdb_cursor_get(cur, &dup_key, &dup_value, MDB_SET_KEY);
  
  unsigned long long count = 0;
  while (MDB_ISOK(rc)) {
    count += ((SvcValue *)dup_value.mv_data)->timestamp >= timestamp;
    rc = mdb_cursor_get(cur, &dup_key, &dup_value, MDB_NEXT_DUP);
  }
  if (!MDB_ISOK(rc) && (rc != MDB_NOTFOUND)) {
    MDB_CHECK(rc);
  }
  mdb_cursor_close(cur);
  return count;
}

static int check_xdayypercent_filter(master_record_t* r) {
  XDayYPercent_CTX* ctx = & xdayypercent_ctx;
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  unsigned short sport = r->srcport;
  unsigned short dport = r->dstport;
  unsigned long long src_client_count = -1, dst_client_count = -1;
  if (exclude_xdayypercent && ctx->exclusion_ip_threshold) {
    src_client_count = get_xdayypercent_svc_client_count(r->prot, sip, sport, ctx->exclusion_x, ctx->exclusion_y);
    //fprintf(stderr, "====src_client_count:%llu, threshold:%llu\n", src_client_count, ctx->exclusion_ip_threshold);
    if (src_client_count > ctx->exclusion_ip_threshold) return 0;
    dst_client_count = get_xdayypercent_svc_client_count(r->prot, dip, dport, ctx->exclusion_x, ctx->exclusion_y);
    //fprintf(stderr, "====dst_client_count:%llu, threshold:%llu\n", dst_client_count, ctx->exclusion_ip_threshold);
    if (dst_client_count > ctx->exclusion_ip_threshold) return 0;
    //fprintf(stderr, "====src_client_count:%llu, dst_client_count:%llu, threshold:%llu\n", src_client_count, dst_client_count, ctx->exclusion_ip_threshold);
  }
  if (xdayypercent_only && ctx->inclusion_ip_threshold) {
    if (src_client_count == -1) src_client_count = get_xdayypercent_svc_client_count(r->prot, sip, sport, ctx->inclusion_x, ctx->inclusion_y);
    if (src_client_count <= ctx->inclusion_ip_threshold) return 0;
    if (dst_client_count == -1) dst_client_count = get_xdayypercent_svc_client_count(r->prot, dip, dport, ctx->inclusion_x, ctx->inclusion_y);
    if (dst_client_count <= ctx->inclusion_ip_threshold) return 0;
  }
  return 1;
}

static void unload_xdayypercent_db(void) {
  XDayYPercent_CTX* ctx = & xdayypercent_ctx;
  int rc = mdb_txn_commit(ctx->txn);
  MDB_CHECK(rc);
  mdb_env_close(ctx->env);
}

static int build_service_map(char *service_file) {
  FILE* fp = fopen(service_file, "r");
  if (fp == NULL) return 0;
  StrMap **sm = &service_map;
  *sm = sm_new(100000);
  if (*sm == NULL)  return 0;
  
  char line[1000];
  unsigned int ip, proto, port, peerips;
  while (fgets(line, sizeof(line), fp)) {
    char * s;
    s = strtok(line, "\r\n");
    if ((s == NULL) || (s[0] == '\0') || (s[0] == '#')) continue;
    if (4 != sscanf(s, "%u\t%u\t%u\t%u", &ip, &proto, &port, &peerips)) continue;
    sprintf(line, "%u:%u:%u", ip, proto, port);
    sm_put(*sm, line, "1");
  }
  fclose(fp);
  fprintf(stderr, "serivce map built: %d\n", sm_get_count(*sm));
  return 1;
}

static int build_scanner_map(char *scanner_file) {
  FILE* fp = fopen(scanner_file, "r");
  if (fp == NULL) return 0;
  StrMap **sm = &scanner_map;
  *sm = sm_new(100000);
  if (*sm == NULL)  return 0;
  
  char line[1000];
  unsigned int t, ip, proto, peerport, peerips;
  while (fgets(line, sizeof(line), fp)) {
    char * s;
    s = strtok(line, "\r\n");
    if ((s == NULL) || (s[0] == '\0') || (s[0] == '#')) continue;
    if (5 != sscanf(s, "%u\t%u\t%u\t%u\t%u", &t, &ip, &proto, &peerport, &peerips)) continue;
    sprintf(line, "%u:%u:%u", ip, proto, peerport);
    sm_put(*sm, line, "1");
  }
  fclose(fp);
  fprintf(stderr, "scanner map built: %d\n", sm_get_count(*sm));
  return 1;
}

static int build_popular_service_map(char *popular_service_file) {
  FILE* fp = fopen(popular_service_file, "r");
  if (fp == NULL) return 0;
  StrMap **sm = &popular_service_map;
  *sm = sm_new(1000000);
  if (*sm == NULL)  return 0;
  
  char line[1000];
  while (fgets(line, sizeof(line), fp)) {
    char * s;
    s = strtok(line, "\r\n");
    if ((s == NULL) || (s[0] == '\0') || (s[0] == '#')) continue;
    char * ip = strtok(s, "\t\r\n");
    char * dname = strtok(s, "\t\r\n");
    sm_put(*sm, ip, dname);
  }
  fclose(fp);
  fprintf(stderr, "popular service map built: %d\n", sm_get_count(*sm));
  return 1;
}

static int build_whitelist_map(char *whitelist_file) {
  FILE* fp = fopen(whitelist_file, "r");
  if (fp == NULL) return 0;
  StrMap **sm = &whitelist_map;
  *sm = sm_new(100000);
  if (*sm == NULL)  return 0;
  
  char line[1000];
  unsigned int t, ip, port;
  while (fgets(line, sizeof(line), fp)) {
    char * s;
    s = strtok(line, "\r\n");
    if ((s == NULL) || (s[0] == '\0') || (s[0] == '#')) continue;
    if (5 != sscanf(s, "%u\t%u\t%u", &t, &ip, &port)) continue;
    sprintf(line, "%u:%u", ip, port);
    sm_put(*sm, line, "1");
  }
  fclose(fp);
  fprintf(stderr, "whitelist map built: %d\n", sm_get_count(*sm));
  return 1;
}

static int build_blacklist_map(char *blacklist_file) {
  FILE* fp = fopen(blacklist_file, "r");
  if (fp == NULL) return 0;
  StrMap **sm = &blacklist_map;
  *sm = sm_new(100000);
  if (*sm == NULL)  return 0;
  
  char line[1000];
  unsigned int t, ip, port;
  while (fgets(line, sizeof(line), fp)) {
    char * s;
    s = strtok(line, "\r\n");
    if ((s == NULL) || (s[0] == '\0') || (s[0] == '#')) continue;
    if (5 != sscanf(s, "%u\t%u\t%u", &t, &ip, &port)) continue;
    sprintf(line, "%u:%u", ip, port);
    sm_put(*sm, line, "1");
  }
  fclose(fp);
  fprintf(stderr, "blacklist map built: %d\n", sm_get_count(*sm));
  return 1;
}

static int in_service_map(unsigned int ip, unsigned int proto, unsigned int port) {
  char line[100];
  snprintf(line, sizeof(line), "%u:%u:%u", ip, proto, port);
  return sm_exists(service_map, line);
}

static int in_scanner_map(unsigned int ip, unsigned int proto, unsigned int peerport) {
  char line[100];
  snprintf(line, sizeof(line), "%u:%u:%u", ip, proto, peerport);
  return sm_exists(scanner_map, line);
}

static int in_popular_service_map(char* ipstr) {
  return sm_exists(popular_service_map, ipstr);
}

static int in_whitelist_map(unsigned int ip, unsigned int port) {
  char line[100];
  snprintf(line, sizeof(line), "%u:%u", ip,port);
  if (sm_exists(scanner_map, line)) return 1;
  snprintf(line, sizeof(line), "0:%u", port);
  if (sm_exists(scanner_map, line)) return 1;
  snprintf(line, sizeof(line), "%u:0", ip);
  if (sm_exists(scanner_map, line)) return 1;
  return 0;
}

static int in_blacklist_map(unsigned int ip, unsigned int port) {
  char line[100];
  snprintf(line, sizeof(line), "%u:%u", ip,port);
  if (sm_exists(scanner_map, line)) return 1;
  snprintf(line, sizeof(line), "0:%u", port);
  if (sm_exists(scanner_map, line)) return 1;
  snprintf(line, sizeof(line), "%u:0", ip);
  if (sm_exists(scanner_map, line)) return 1;
  return 0;
}

static int check_service_filter(master_record_t* r) {
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  r->service = (in_service_map(sip, r->prot, r->srcport) ? 1 : 0) | (in_service_map(dip, r->prot, r->dstport) ? 2 : 0);
  if (exclude_services) {
    return !r->service;
  }
  if (services_only) {
    return !!r->service;
  }
  return 1;
}

static int check_scanner_filter(master_record_t* r) {
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  r->scanner = (in_scanner_map(sip, r->prot, r->dstport) ? 1 : 0) | (in_scanner_map(dip, r->prot, r->srcport) ? 2 : 0);
  if (exclude_scanners) {
    return !r->scanner;
  }
  if (scanners_only) {
    return !!r->scanner;
  }
  return 1;
}

static int check_popular_service_filter(master_record_t* r) {
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  char* sipstr, *dipstr;
  struct sockaddr_in in;
  in.sin_addr.s_addr = ntohl(sip);
  sipstr = strdup(inet_ntoa(in.sin_addr));
  in.sin_addr.s_addr = ntohl(dip);
  dipstr = strdup(inet_ntoa(in.sin_addr));

  r->popular_service = (in_popular_service_map(sipstr) ? 1 : 0) | (in_popular_service_map(dipstr) ? 2 : 0);
/*if ((strcmp(dipstr, "114.255.29.195")==0) && (strcmp(sipstr, "219.142.70.10") == 0)) {
    fprintf(stderr, "r->popular_service:%d\n", r->popular_service);
    fprintf(stderr, "sip:%d, dip:%d \n", in_popular_service_map(sipstr), in_popular_service_map(dipstr));
}*/
  if (exclude_popular_services) {
    return !r->popular_service;
  }
  if (popular_services_only) {
    return !!r->popular_service;
  }
  return 1;
}

static int check_whitelist_filter(master_record_t* r) {
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  r->whitelist = (in_whitelist_map(sip, r->dstport) ? 1 : 0) | (in_whitelist_map(dip, r->srcport) ? 2 : 0);
  if (exclude_whitelist) {
    return !r->whitelist;
  }
  if (whitelist_only) {
    return !!r->whitelist;
  }
  return 1;
}

static int check_blacklist_filter(master_record_t* r) {
  unsigned int sip = r->v6.srcaddr[1] & 0xffffffffLL;
  unsigned int dip = r->v6.dstaddr[1] & 0xffffffffLL;
  r->blacklist = (in_blacklist_map(sip, r->dstport) ? 1 : 0) | (in_blacklist_map(dip, r->srcport) ? 2 : 0);
  if (exclude_blacklist) {
    return !r->blacklist;
  }
  if (blacklist_only) {
    return !!r->blacklist;
  }
  return 1;
}

static void destroy_service_map(void) {
  if (service_map) sm_delete(service_map);
  service_map = NULL;
}

static void destroy_scanner_map(void) {
  if (scanner_map) sm_delete(scanner_map);
  scanner_map = NULL;
}

static void destroy_whitelist_map(void) {
  if (whitelist_map) sm_delete(whitelist_map);
  whitelist_map = NULL;
}

static void destroy_blacklist_map(void) {
  if (blacklist_map) sm_delete(blacklist_map);
  blacklist_map = NULL;
}

static void usage(char *name) {
		printf("usage %s [options] [\"filter\"]\n"
					"-h\t\tthis text you see right here\n"
					"-V\t\tPrint version and exit.\n"
					"-a\t\tAggregate netflow data.\n"
					"-A <expr>[/net]\tHow to aggregate: ',' sep list of tags see nfdump(1)\n"
					"\t\tor subnet aggregation: srcip4/24, srcip6/64.\n"
					"-b\t\tAggregate netflow records as bidirectional flows.\n"
					"-B\t\tAggregate netflow records as bidirectional flows - Guess direction.\n"
					"-r <file>\tread input from file\n"
					"-w <file>\twrite output to file\n"
					"-f\t\tread netflow filter from file\n"
					"-n\t\tDefine number of top N. \n"
					"-c\t\tLimit number of records to display\n"
					"-D <dns>\tUse nameserver <dns> for host lookup.\n"
					"-N\t\tPrint plain numbers\n"
                    "-S exclude_services|services_only\n"
                    "-S exclude_scanners|scanners_only\n"
                    "-S exclude_whitelist|whitelist_only\n"
                    "-S exclude_blacklist|blacklist_only\n"
                    "-S exclude_popular_services|popular_services_only\n"
					"-s <expr>[/<order>]\tGenerate statistics for <expr> any valid record element.\n"
					"\t\tand ordered by <order>: packets, bytes, flows, bps pps and bpp.\n"
					"-q\t\tQuiet: Do not print the header and bottom stat lines.\n"
					"-H Add xstat histogram data to flow file.(default 'no')\n"
					"-i <ident>\tChange Ident to <ident> in file given by -r.\n"
					"-j <file>\tCompress/Uncompress file.\n"
					"-z\t\tCompress flows in output file. Used in combination with -w.\n"
					"-l <expr>\tSet limit on packets for line and packed output format.\n"
					"\t\tkey: 32 character string or 64 digit hex string starting with 0x.\n"
					"-L <expr>\tSet limit on bytes for line and packed output format.\n"
					"-I \t\tPrint netflow summary statistics info from file, specified by -r.\n"
					"-M <expr>\tRead input from multiple directories.\n"
					"\t\t/dir/dir1:dir2:dir3 Read the same files from '/dir/dir1' '/dir/dir2' and '/dir/dir3'.\n"
					"\t\trequests either -r filename or -R firstfile:lastfile without pathnames\n"
					"-m\t\tPrint netflow data date sorted. Only useful with -M\n"
					"-R <expr>\tRead input from sequence of files.\n"
					"\t\t/any/dir  Read all files in that directory.\n"
					"\t\t/dir/file Read all files beginning with 'file'.\n"
					"\t\t/dir/file1:file2: Read all files from 'file1' to file2.\n"
					"-o <mode>\tUse <mode> to print out netflow records:\n"
					"\t\t raw      Raw record dump.\n"
					"\t\t line     Standard output line format.\n"
					"\t\t long     Standard output line format with additional fields.\n"
					"\t\t extended Even more information.\n"
					"\t\t csv      ',' separated, machine parseable output format.\n"
					"\t\t pipe     '|' separated legacy machine parseable output format.\n"
					"\t\t\tmode may be extended by '6' for full IPv6 listing. e.g.long6, extended6.\n"
					"-E <file>\tPrint exporter ans sampling info for collected flows.\n"
					"-v <file>\tverify netflow data file. Print version and blocks.\n"
					"-x <file>\tverify extension records in netflow data file.\n"
					"-X\t\tDump Filtertable and exit (debug option).\n"
					"-Z\t\tCheck filter syntax and exit.\n"
					"-t <time>\ttime window for filtering packets\n"
					"\t\tyyyy/MM/dd.hh:mm:ss[-yyyy/MM/dd.hh:mm:ss]\n", name);
} /* usage */


static void PrintSummary(stat_record_t *stat_record, int plain_numbers, int csv_output) {
static double	duration;
uint64_t	bps, pps, bpp;
char 		byte_str[32], packet_str[32], bps_str[32], pps_str[32], bpp_str[32];

	bps = pps = bpp = 0;
	if ( stat_record->last_seen ) {
		duration = stat_record->last_seen - stat_record->first_seen;
		duration += ((double)stat_record->msec_last - (double)stat_record->msec_first) / 1000.0;
	} else {
		// no flows to report
		duration = 0;
	}
	if ( duration > 0 && stat_record->last_seen > 0 ) {
		bps = ( stat_record->numbytes << 3 ) / duration;	// bits per second. ( >> 3 ) -> * 8 to convert octets into bits
		pps = stat_record->numpackets / duration;			// packets per second
		bpp = stat_record->numpackets ? stat_record->numbytes / stat_record->numpackets : 0;    // Bytes per Packet
	}
	if ( csv_output ) {
		printf("Summary\n");
		printf("flows,bytes,packets,avg_bps,avg_pps,avg_bpp\n");
		printf("%llu,%llu,%llu,%llu,%llu,%llu\n",
			(long long unsigned)stat_record->numflows, (long long unsigned)stat_record->numbytes, 
			(long long unsigned)stat_record->numpackets, (long long unsigned)bps, 
			(long long unsigned)pps, (long long unsigned)bpp );
	} else if ( plain_numbers ) {
		printf("Summary: total flows: %llu, total bytes: %llu, total packets: %llu, avg bps: %llu, avg pps: %llu, avg bpp: %llu\n",
			(long long unsigned)stat_record->numflows, (long long unsigned)stat_record->numbytes, 
			(long long unsigned)stat_record->numpackets, (long long unsigned)bps, 
			(long long unsigned)pps, (long long unsigned)bpp );
	} else {
		format_number(stat_record->numbytes, byte_str, VAR_LENGTH);
		format_number(stat_record->numpackets, packet_str, VAR_LENGTH);
		format_number(bps, bps_str, VAR_LENGTH);
		format_number(pps, pps_str, VAR_LENGTH);
		format_number(bpp, bpp_str, VAR_LENGTH);
		printf("Summary: total flows: %llu, total bytes: %s, total packets: %s, avg bps: %s, avg pps: %s, avg bpp: %s\n",
		(unsigned long long)stat_record->numflows, byte_str, packet_str, bps_str, pps_str, bpp_str );
	}

} // End of PrintSummary

stat_record_t process_data(char *wfile, int element_stat, int flow_stat, int sort_flows,
	printer_t print_header, printer_t print_record, time_t twin_start, time_t twin_end, 
	uint64_t limitflows, int tag, int compress, int do_xstat,
// added by lxh start
    int new_aggregate, char* print_order, int bidir,
    int date_sorted, char* newIdent, int  do_tag,
    int GuessDir, int new_flow_stat, char* record_header, int topN, int quiet, int pipe_output,
    int csv_output, int plain_numbers, nfprof_t* profile_data, int orig_plain_numbers)
// added_by lxh end

{
common_record_t 	*flow_record;
master_record_t		*master_record;
nffile_t			*nffile_w, *nffile_r;
xstat_t				*xstat;
stat_record_t 		stat_record;
int 				done, write_file;

#ifdef COMPAT15
int	v1_map_done = 0;
#endif

// added by lxh start
printer_t tmp_print_record = print_record;
// added by lxh end

	// time window of all matched flows
	memset((void *)&stat_record, 0, sizeof(stat_record_t));
	stat_record.first_seen = 0x7fffffff;
	stat_record.msec_first = 999;

	// Do the logic first

	// print flows later, when all records are processed and sorted
	// flow limits apply at that time
	if ( sort_flows ) {
// moded by lxh start
		//print_record = NULL;
		tmp_print_record = NULL;
// moded by lxh end
		limitflows   = 0;
	}

	// do not print flows when doing any stats
	if ( flow_stat || element_stat ) {
// moded by lxh start
		//print_record = NULL;
		tmp_print_record = NULL;
// moded by lxh end
		limitflows   = 0;
	}

	// do not write flows to file, when doing any stats
	// -w may apply for flow_stats later
	write_file = !(sort_flows || flow_stat || element_stat) && wfile;
	nffile_r = NULL;
	nffile_w = NULL;
	xstat  	 = NULL;

	// Get the first file handle
	nffile_r = GetNextFile(NULL, twin_start, twin_end);
	if ( !nffile_r ) {
		LogError("GetNextFile() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno) );
		return stat_record;
	}
	if ( nffile_r == EMPTY_LIST ) {
		LogError("Empty file list. No files to process\n");
		return stat_record;
	}

	// preset time window of all processed flows to the stat record in first flow file
	t_first_flow = nffile_r->stat_record->first_seen;
	t_last_flow  = nffile_r->stat_record->last_seen;

	// store infos away for later use
	// although multiple files may be processed, it is assumed that all 
	// have the same settings
	is_anonymized = IP_ANONYMIZED(nffile_r);
	strncpy(Ident, nffile_r->file_header->ident, IDENTLEN);
	Ident[IDENTLEN-1] = '\0';

	// prepare output file if requested
	if ( write_file ) {
		nffile_w = OpenNewFile(wfile, NULL, compress, IP_ANONYMIZED(nffile_r), NULL );
		if ( !nffile_w ) {
			if ( nffile_r ) {
				CloseFile(nffile_r);
				DisposeFile(nffile_r);
			}
			return stat_record;
		}
		if ( do_xstat ) {
			xstat = InitXStat(nffile_w);
			if ( !xstat ) {
				if ( nffile_r ) {
					CloseFile(nffile_r);
					DisposeFile(nffile_r);
				}
				return stat_record;
			}
		}
	}

	// setup Filter Engine to point to master_record, as any record read from file
	// is expanded into this record
	// Engine->nfrecord = (uint64_t *)master_record;

	done = 0;
	while ( !done ) {
	int i, ret;

		// get next data block from file
		ret = ReadBlock(nffile_r);

		switch (ret) {
			case NF_CORRUPT:
			case NF_ERROR:
				if ( ret == NF_CORRUPT ) 
					LogError("Skip corrupt data file '%s'\n",GetCurrentFilename());
				else 
					LogError("Read error in file '%s': %s\n",GetCurrentFilename(), strerror(errno) );
				// fall through - get next file in chain
			case NF_EOF: {
				nffile_t *next = GetNextFile(nffile_r, twin_start, twin_end);
				if ( next == EMPTY_LIST ) {
					done = 1;
				} else if ( next == NULL ) {
					done = 1;
					LogError("Unexpected end of file list\n");
				} else {
					// Update global time span window
					if ( next->stat_record->first_seen < t_first_flow )
						t_first_flow = next->stat_record->first_seen;
					if ( next->stat_record->last_seen > t_last_flow ) 
						t_last_flow = next->stat_record->last_seen;
					// continue with next file
// add by lxh begin
    {
      static int kk;
      kk++;
      if (kk == sep / 300)
      {
        kk=0;
        OutputAndDestroy(new_aggregate, print_order, wfile, compress, bidir, date_sorted, newIdent, print_record,
          limitflows, do_tag, GuessDir, new_flow_stat, record_header, topN, quiet, pipe_output, csv_output, &stat_record,
          plain_numbers, profile_data, element_stat, orig_plain_numbers);
      }
    }
// add by lxh end
				}
				continue;

				} break; // not really needed
			default:
				// successfully read block
				total_bytes += ret;
		}


#ifdef COMPAT15
		if ( nffile_r->block_header->id == DATA_BLOCK_TYPE_1 ) {
			common_record_v1_t *v1_record = (common_record_v1_t *)nffile_r->buff_ptr;
			// create an extension map for v1 blocks
			if ( v1_map_done == 0 ) {
				extension_map_t *map = malloc(sizeof(extension_map_t) + 2 * sizeof(uint16_t) );
				if ( ! map ) {
					LogError("malloc() allocation error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno) );
					exit(255);
				}
				map->type 	= ExtensionMapType;
				map->size 	= sizeof(extension_map_t) + 2 * sizeof(uint16_t);
				if (( map->size & 0x3 ) != 0 ) {
					map->size += 4 - ( map->size & 0x3 );
				}

				map->map_id = INIT_ID;

				map->ex_id[0]  = EX_IO_SNMP_2;
				map->ex_id[1]  = EX_AS_2;
				map->ex_id[2]  = 0;
				
				map->extension_size  = 0;
				map->extension_size += extension_descriptor[EX_IO_SNMP_2].size;
				map->extension_size += extension_descriptor[EX_AS_2].size;

				if ( Insert_Extension_Map(&extension_map_list,map) && write_file ) {
					// flush new map
					AppendToBuffer(nffile_w, (void *)map, map->size);
				} // else map already known and flushed

				v1_map_done = 1;
			}

			// convert the records to v2
			for ( i=0; i < nffile_r->block_header->NumRecords; i++ ) {
				common_record_t *v2_record = (common_record_t *)v1_record;
				Convert_v1_to_v2((void *)v1_record);
				// now we have a v2 record -> use size of v2_record->size
				v1_record = (common_record_v1_t *)((pointer_addr_t)v1_record + v2_record->size);
			}
			nffile_r->block_header->id = DATA_BLOCK_TYPE_2;
		}
#endif

		if ( nffile_r->block_header->id == Large_BLOCK_Type ) {
			// skip
			printf("Xstat block skipped ...\n");
			continue;
		}

		if ( nffile_r->block_header->id != DATA_BLOCK_TYPE_2 ) {
			if ( nffile_r->block_header->id == DATA_BLOCK_TYPE_1 ) {
				LogError("Can't process nfdump 1.5.x block type 1. Add --enable-compat15 to compile compatibility code. Skip block.\n");
			} else {
				LogError("Can't process block type %u. Skip block.\n", nffile_r->block_header->id);
			}
			skipped_blocks++;
			continue;
		}

		flow_record = nffile_r->buff_ptr;
		for ( i=0; i < nffile_r->block_header->NumRecords; i++ ) {

			switch ( flow_record->type ) {
				case CommonRecordType:  {
					int match;
					uint32_t map_id = flow_record->ext_map;
					generic_exporter_t *exp_info = exporter_list[flow_record->exporter_sysid];
					if ( map_id >= MAX_EXTENSION_MAPS ) {
						LogError("Corrupt data file. Extension map id %u too big.\n", flow_record->ext_map);
						exit(255);
					}
					if ( extension_map_list.slot[map_id] == NULL ) {
						LogError("Corrupt data file. Missing extension map %u. Skip record.\n", flow_record->ext_map);
						flow_record = (common_record_t *)((pointer_addr_t)flow_record + flow_record->size);	
						continue;
					} 

					total_flows++;
					master_record = &(extension_map_list.slot[map_id]->master_record);
					Engine->nfrecord = (uint64_t *)master_record;
					ExpandRecord_v2( flow_record, extension_map_list.slot[map_id], 
						exp_info ? &(exp_info->info) : NULL, master_record);

					// Time based filter
					// if no time filter is given, the result is always true
					match  = twin_start && (master_record->first < twin_start || master_record->last > twin_end) ? 0 : 1;
					match &= limitflows ? stat_record.numflows < limitflows : 1;

					// filter netflow record with user supplied filter
					if ( match ) 
						match = (*Engine->FilterEngine)(Engine);
	
					/*if ( (match == 0) 
                            //|| !check_service_filter(master_record)) {
                            || !check_scanner_filter(master_record)) {
                            || !check_whitelist_filter(master_record)
                            || !check_blacklist_filter(master_record)) { 
                            || !check_popular_service_filter(master_record)
                            || !check_xdayypercent_filter(master_record)) { // record failed to pass all filters*/
          if (match == 0 || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)
             || !check_xdayypercent_filter(master_record)) {
						// increment pointer by number of bytes for netflow record
						flow_record = (common_record_t *)((pointer_addr_t)flow_record + flow_record->size);	
						// go to next record
						continue;
					}

					// Records passed filter -> continue record processing
					// Update statistics
					UpdateStat(&stat_record, master_record);

					// update number of flows matching a given map
					extension_map_list.slot[map_id]->ref_count++;
	
					if ( flow_stat ) {
						AddFlow(flow_record, master_record);
						if ( element_stat ) {
							AddStat(flow_record, master_record);
						} 
					} else if ( element_stat ) {
						AddStat(flow_record, master_record);
					} else if ( sort_flows ) {
						InsertFlow(flow_record, master_record);
					} else {
						if ( write_file ) {
							AppendToBuffer(nffile_w, (void *)flow_record, flow_record->size);
							if ( xstat ) 
								UpdateXStat(xstat, master_record);
// modified by lxh start
						//} else if ( print_record ) {
						} else if ( tmp_print_record ) {
// modified by lxh end
							char *string;
							// if we need to print out this record
							print_record(master_record, &string, tag);
							if ( string ) {
								if ( limitflows ) {
									if ( (stat_record.numflows <= limitflows) )
										printf("%s\n", string);
								} else 
									printf("%s\n", string);
							}
						} else { 
							// mutually exclusive conditions should prevent executing this code
							// this is buggy!
							printf("Bug! - this code should never get executed in file %s line %d\n", __FILE__, __LINE__);
						}
					} // sort_flows - else
					} break; 
				case ExtensionMapType: {
					extension_map_t *map = (extension_map_t *)flow_record;
	
					if ( Insert_Extension_Map(&extension_map_list, map) && write_file ) {
						// flush new map
						AppendToBuffer(nffile_w, (void *)map, map->size);
					} // else map already known and flushed
					} break;
				case ExporterRecordType:
				case SamplerRecordype:
						// Silently skip exporter records
					break;
				case ExporterInfoRecordType: {
					int ret = AddExporterInfo((exporter_info_record_t *)flow_record);
					if ( ret != 0 ) {
						if ( write_file && ret == 1 ) 
							AppendToBuffer(nffile_w, (void *)flow_record, flow_record->size);
					} else {
						LogError("Failed to add Exporter Record\n");
					}
					} break;
				case ExporterStatRecordType:
					AddExporterStat((exporter_stats_record_t *)flow_record);
					break;
				case SamplerInfoRecordype: {
					int ret = AddSamplerInfo((sampler_info_record_t *)flow_record);
					if ( ret != 0 ) {
						if ( write_file && ret == 1 ) 
							AppendToBuffer(nffile_w, (void *)flow_record, flow_record->size);
					} else {
						LogError("Failed to add Sampler Record\n");
					}
					} break;
				default: {
					LogError("Skip unknown record type %i\n", flow_record->type);
				}
			}

		// Advance pointer by number of bytes for netflow record
		flow_record = (common_record_t *)((pointer_addr_t)flow_record + flow_record->size);	


		} // for all records

		// check if we are done, due to -c option 
		if ( limitflows ) 
			done = stat_record.numflows >= limitflows;

	} // while

	CloseFile(nffile_r);

	// flush output file
	if ( write_file ) {
		// flush current buffer to disc
		if ( nffile_w->block_header->NumRecords ) {
			if ( WriteBlock(nffile_w) <= 0 ) {
				LogError("Failed to write output buffer to disk: '%s'" , strerror(errno));
			} 
		}

		if ( xstat ) {
			if ( WriteExtraBlock(nffile_w, xstat->block_header ) <= 0 ) {
				LogError("Failed to write xstat buffer to disk: '%s'" , strerror(errno));
			} 
		}

		/* Stat info */
		if ( write_file ) {
			/* Copy stat info and close file */
			memcpy((void *)nffile_w->stat_record, (void *)&stat_record, sizeof(stat_record_t));
			CloseUpdateFile(nffile_w, nffile_r->file_header->ident );
			nffile_w = DisposeFile(nffile_w);
		} // else stdout
	}	 

	PackExtensionMapList(&extension_map_list);

	DisposeFile(nffile_r);
	return stat_record;

} // End of process_data


int main( int argc, char **argv ) {
struct stat stat_buff;
stat_record_t	sum_stat;
printer_t 	print_header, print_record;
nfprof_t 	profile_data;
char 		*rfile, *Rfile, *Mdirs, *wfile, *ffile, *filter, *tstring, *stat_type;
char		*byte_limit_string, *packet_limit_string, *print_format, *record_header;
char		*print_order, *query_file, *UnCompress_file, *nameserver, *aggr_fmt;
int 		c, ffd, ret, element_stat, fdump;
int 		i, user_format, quiet, flow_stat, topN, aggregate, aggregate_mask, bidir;
int 		print_stat, syntax_only, date_sorted, do_tag, compress, do_xstat;
int			plain_numbers, GuessDir, pipe_output, csv_output;
time_t 		t_start, t_end;
uint16_t	Aggregate_Bits;
uint32_t	limitflows;
uint64_t	AggregateMasks[AGGR_SIZE];
char 		Ident[IDENTLEN];

	rfile = Rfile = Mdirs = wfile = ffile = filter = tstring = stat_type = NULL;
	byte_limit_string = packet_limit_string = NULL;
	fdump = aggregate = 0;
	aggregate_mask	= 0;
	bidir			= 0;
	t_start = t_end = 0;
	syntax_only	    = 0;
	topN	        = 10;
	flow_stat       = 0;
	print_stat      = 0;
	element_stat  	= 0;
	do_xstat 		= 0;
	limitflows		= 0;
	date_sorted		= 0;
	total_bytes		= 0;
	total_flows		= 0;
	skipped_blocks	= 0;
	do_tag			= 0;
	quiet			= 0;
	user_format		= 0;
	compress		= 0;
	plain_numbers   = 0;
	pipe_output		= 0;
	csv_output		= 0;
	is_anonymized	= 0;
	GuessDir		= 0;
	nameserver		= NULL;

	print_format    = NULL;
	print_header 	= NULL;
	print_record  	= NULL;
	print_order  	= NULL;
	query_file		= NULL;
	UnCompress_file	= NULL;
	aggr_fmt		= NULL;
	record_header 	= NULL;
	Aggregate_Bits	= 0xFFFF;	// set all bits

	Ident[0] = '\0';

	for ( i=0; i<AGGR_SIZE; AggregateMasks[i++] = 0 ) ;

	while ((c = getopt(argc, argv, "6aA:p:S:Bbc:D:E:s:hHn:i:j:f:qzr:v:w:K:M:NImO:R:XZt:TVv:x:l:L:o:F:G:")) != EOF) {
		switch (c) {
            case 'p':
                sep = atoi(optarg);
                sep -= sep % 300;
                if (sep<0) sep=0;
                break;
            case 'S':
                if (strcmp(optarg, "exclude_services") == 0)
                  exclude_services = 1;
                else if (strcmp(optarg, "services_only") == 0)
                  services_only = 1;
                else if (strcmp(optarg, "exclude_scanners") == 0)
                  exclude_scanners = 1;
                else if (strcmp(optarg, "scanners_only") == 0)
                  scanners_only = 1;
                else if (strcmp(optarg, "exclude_popular_services") == 0)
                  exclude_popular_services = 1;
                else if (strcmp(optarg, "popular_services_only") == 0)
                  popular_services_only = 1;
                else if (strcmp(optarg, "exclude_whitelist") == 0)
                  exclude_whitelist = 1;
                else if (strcmp(optarg, "whitelist_only") == 0)
                  whitelist_only = 1;
                else if (strcmp(optarg, "exclude_blacklist") == 0)
                  exclude_blacklist = 1;
                else if (strcmp(optarg, "blacklist_only") == 0)
                  blacklist_only = 1;
                else if (strncmp(optarg, "exclude_xdayypercent", strlen("exclude_xdayypercent")) == 0)
                  exclude_xdayypercent = parse_xdayypercent(optarg, "exclude_xdayypercent=%u:%u",  &xdayypercent_ctx.exclusion_x, &xdayypercent_ctx.exclusion_y);
                else if (strncmp(optarg, "xdayypercent_only", strlen("xdayypercent_only")) == 0)
                  xdayypercent_only = parse_xdayypercent(optarg, "xdayypercent_only=%u:%u",  &xdayypercent_ctx.inclusion_x, &xdayypercent_ctx.inclusion_y);
                else {
                  fprintf(stderr, "error in -S filter:%s\n", optarg);
                  usage(argv[0]);
                  exit(1);
                }
                if (exclude_services || services_only) build_service_map(service_file);
                if (exclude_scanners || scanners_only) build_scanner_map(scanner_file);
                if (exclude_popular_services || popular_services_only) build_popular_service_map(popular_service_file);
                if (exclude_whitelist || whitelist_only) build_whitelist_map(whitelist_file);
                if (exclude_blacklist || blacklist_only) build_blacklist_map(blacklist_file);
                if (exclude_xdayypercent || xdayypercent_only) load_xdayypercent_db(xdayypercent_db_path);
                break;
			case 'h':
				usage(argv[0]);
				exit(0);
				break;
			case 'a':
				aggregate = 1;
				break;
			case 'A':
				if ( !ParseAggregateMask(optarg, &aggr_fmt ) ) {
					exit(255);
				}
				aggregate_mask = 1;
				break;
			case 'B':
				GuessDir = 1;
			case 'b':
				if ( !SetBidirAggregation() ) {
					exit(255);
				}
				bidir	  = 1;
				// implies
				aggregate = 1;
				break;
			case 'D':
				nameserver = optarg;
				if ( !set_nameserver(nameserver) ) {
					exit(255);
				}
				break;
			case 'E':
				query_file = optarg;
				if ( !InitExporterList() ) {
					exit(255);
				}
				PrintExporters(query_file);
				exit(0);
				break;
			case 'X':
				fdump = 1;
				break;
			case 'Z':
				syntax_only = 1;
				break;
			case 'q':
				quiet = 1;
				break;
			case 'z':
				compress = 1;
				break;
			case 'c':	
				limitflows = atoi(optarg);
				if ( !limitflows ) {
					LogError("Option -c needs a number > 0\n");
					exit(255);
				}
				break;
			case 's':
				stat_type = optarg;
                if ( !SetStat(stat_type, &element_stat, &flow_stat) ) {
                    exit(255);
                } 
				break;
			case 'V':
				printf("%s: Version: %s\n",argv[0], nfdump_version);
				exit(0);
				break;
			case 'l':
				packet_limit_string = optarg;
				break;
			case 'K':
				LogError("*** Anonymisation moved! Use nfanon to anonymise flows!\n");
				exit(255);
				break;
			case 'H':
				do_xstat = 1;
				break;
			case 'L':
				byte_limit_string = optarg;
				break;
			case 'N':
				plain_numbers = 1;
				break;
			case 'f':
				ffile = optarg;
				break;
			case 't':
				tstring = optarg;
				break;
			case 'r':
				rfile = optarg;
				if ( strcmp(rfile, "-") == 0 )
					rfile = NULL;
				break;
			case 'm':
				print_order = "tstart";
				Parse_PrintOrder(print_order);
				date_sorted = 1;
				LogError("Option -m depricated. Use '-O tstart' instead\n");
				break;
			case 'M':
				Mdirs = optarg;
				break;
			case 'I':
				print_stat++;
				break;
			case 'o':	// output mode
				print_format = optarg;
				break;
			case 'O': {	// stat order by
				int ret;
				print_order = optarg;
				ret = Parse_PrintOrder(print_order);
				if ( ret < 0 ) {
					LogError("Unknown print order '%s'\n", print_order);
					exit(255);
				}
				date_sorted = ret == 6;		// index into order_mode
				} break;
			case 'R':
				Rfile = optarg;
				break;
			case 'w':
				wfile = optarg;
				break;
			case 'n':
				topN = atoi(optarg);
				if ( topN < 0 ) {
					LogError("TopnN number %i out of range\n", topN);
					exit(255);
				}
				break;
			case 'T':
				do_tag = 1;
				break;
			case 'i':
				strncpy(Ident, optarg, IDENT_SIZE);
				Ident[IDENT_SIZE - 1] = 0;
				if ( strchr(Ident, ' ') ) {
					LogError("Ident must not contain spaces\n");
					exit(255);
				}
				break;
			case 'j':
				UnCompress_file = optarg;
				UnCompressFile(UnCompress_file);
				exit(0);
				break;
			case 'x':
				query_file = optarg;
				InitExtensionMaps(NULL);
				DumpExMaps(query_file);
				exit(0);
				break;
			case 'v':
				query_file = optarg;
				QueryFile(query_file);
				exit(0);
				break;
			case '6':	// print long IPv6 addr
				Setv6Mode(1);
				break;
			default:
				usage(argv[0]);
				exit(0);
		}
	}
	if (argc - optind > 1) {
		usage(argv[0]);
		exit(255);
	} else {
		/* user specified a pcap filter */
		filter = argv[optind];
		FilterFilename = NULL;
	}
	
	// Change Ident only
	if ( rfile && strlen(Ident) > 0 ) {
		ChangeIdent(rfile, Ident);
		exit(0);
	}

	if ( (element_stat && !flow_stat) && aggregate_mask ) {
		LogError("Warning: Aggregation ignored for element statistics\n");
		aggregate_mask = 0;
	}

	if ( !flow_stat && aggregate_mask ) {
		aggregate = 1;
	}

	if ( rfile && Rfile ) {
		LogError("-r and -R are mutually exclusive. Plase specify either -r or -R\n");
		exit(255);
	}
	if ( Mdirs && !(rfile || Rfile) ) {
		LogError("-M needs either -r or -R to specify the file or file list. Add '-R .' for all files in the directories.\n");
		exit(255);
	}

	InitExtensionMaps(&extension_map_list);
	if ( !InitExporterList() ) {
		exit(255);
	}

	SetupInputFileSequence(Mdirs, rfile, Rfile);

	if ( print_stat ) {
		nffile_t *nffile;
		if ( !rfile && !Rfile && !Mdirs) {
			LogError("Expect data file(s).\n");
			exit(255);
		}

		memset((void *)&sum_stat, 0, sizeof(stat_record_t));
		sum_stat.first_seen = 0x7fffffff;
		sum_stat.msec_first = 999;
		nffile = GetNextFile(NULL, 0, 0);
		if ( !nffile ) {
			LogError("Error open file: %s\n", strerror(errno));
			exit(250);
		}
		while ( nffile && nffile != EMPTY_LIST ) {
			SumStatRecords(&sum_stat, nffile->stat_record);
			nffile = GetNextFile(nffile, 0, 0);
		}
		PrintStat(&sum_stat);
		exit(0);
	}

	// handle print mode
	if ( !print_format ) {
		// automatically select an appropriate output format for custom aggregation
		// aggr_fmt is compiled by ParseAggregateMask
		if ( aggr_fmt ) {
			int len = strlen(AggrPrependFmt) + strlen(aggr_fmt) + strlen(AggrAppendFmt) + 7;	// +7 for 'fmt:', 2 spaces and '\0'
			print_format = malloc(len);
			if ( !print_format ) {
				LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno) );
				exit(255);
			}
			snprintf(print_format, len, "fmt:%s %s %s",AggrPrependFmt, aggr_fmt, AggrAppendFmt );
			print_format[len-1] = '\0';
		} else if ( bidir ) {
			print_format = "biline";
		} else
			print_format = DefaultMode;
	}

	if ( strncasecmp(print_format, "fmt:", 4) == 0 ) {
		// special user defined output format
		char *format = &print_format[4];
		if ( strlen(format) ) {
			if ( !ParseOutputFormat(format, plain_numbers, printmap) )
				exit(255);
			print_record  = format_special;
			record_header = get_record_header();
			user_format	  = 1;
		} else {
			LogError("Missing format description for user defined output format!\n");
			exit(255);
		}
	} else {
		// predefined output format

		// Check for long_v6 mode
		i = strlen(print_format);
		if ( i > 2 ) {
			if ( print_format[i-1] == '6' ) {
				Setv6Mode(1);
				print_format[i-1] = '\0';
			} else 
				Setv6Mode(0);
		}

		i = 0;
		while ( printmap[i].printmode ) {
			if ( strncasecmp(print_format, printmap[i].printmode, MAXMODELEN) == 0 ) {
				if ( printmap[i].Format ) {
					if ( !ParseOutputFormat(printmap[i].Format, plain_numbers, printmap) )
						exit(255);
					// predefined custom format
					print_record  = printmap[i].func;
					record_header = get_record_header();
					user_format	  = 1;
				} else {
					// To support the pipe output format for element stats - check for pipe, and remember this
					if ( strncasecmp(print_format, "pipe", MAXMODELEN) == 0 ) {
						pipe_output = 1;
					}
					if ( strncasecmp(print_format, "csv", MAXMODELEN) == 0 ) {
						csv_output = 1;
						set_record_header();
						record_header = get_record_header();
					}
					// predefined static format
					print_record  = printmap[i].func;
					user_format	  = 0;
				}
				break;
			}
			i++;
		}
	}

	if ( !print_record ) {
		LogError("Unknown output mode '%s'\n", print_format);
		exit(255);
	}

	// this is the only case, where headers are printed.
	if ( strncasecmp(print_format, "raw", 16) == 0 )
		print_header = format_file_block_header;
	
	if ( aggregate && (flow_stat || element_stat) ) {
		aggregate = 0;
		LogError("Command line switch -s overwrites -a\n");
	}

	if ( !filter && ffile ) {
		if ( stat(ffile, &stat_buff) ) {
			LogError("Can't stat filter file '%s': %s\n", ffile, strerror(errno));
			exit(255);
		}
		filter = (char *)malloc(stat_buff.st_size+1);
		if ( !filter ) {
			LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno) );
			exit(255);
		}
		ffd = open(ffile, O_RDONLY);
		if ( ffd < 0 ) {
			LogError("Can't open filter file '%s': %s\n", ffile, strerror(errno));
			exit(255);
		}
		ret = read(ffd, (void *)filter, stat_buff.st_size);
		if ( ret < 0   ) {
			perror("Error reading filter file");
			close(ffd);
			exit(255);
		}
		total_bytes += ret;
		filter[stat_buff.st_size] = 0;
		close(ffd);

		FilterFilename = ffile;
	}

	// if no filter is given, set the default ip filter which passes through every flow
	if ( !filter  || strlen(filter) == 0 ) 
		filter = "any";

	Engine = CompileFilter(filter);
	if ( !Engine ) 
		exit(254);

	if ( fdump ) {
		printf("StartNode: %i Engine: %s\n", Engine->StartNode, Engine->Extended ? "Extended" : "Fast");
		DumpList(Engine);
		exit(0);
	}

	if ( syntax_only )
		exit(0);

	if ( print_order && flow_stat ) {
		printf("-s record and -m are mutually exclusive options\n");
		exit(255);
	}

	if ((aggregate || flow_stat || print_order)  && !Init_FlowTable() )
			exit(250);

	if (element_stat && !Init_StatTable(HashBits, NumPrealloc) )
			exit(250);

	SetLimits(element_stat || aggregate || flow_stat, packet_limit_string, byte_limit_string);

	if ( tstring ) {
		if ( !ScanTimeFrame(tstring, &t_start, &t_end) )
			exit(255);
	}


	if ( !(flow_stat || element_stat || wfile || quiet ) && record_header ) {
		if ( user_format ) {
			printf("%s\n", record_header);
		} else {
			// static format - no static format with header any more, but keep code anyway
			if ( Getv6Mode() ) {
				printf("%s\n", record_header);
			} else
				printf("%s\n", record_header);
		}
	}

	nfprof_start(&profile_data);
    int orig_plain_numbers = plain_numbers;
	sum_stat = process_data(wfile, element_stat, aggregate || flow_stat, print_order != NULL,
						print_header, print_record, t_start, t_end, 
						limitflows, do_tag, compress, do_xstat,
// added by lxh start
        aggregate, print_order, bidir, date_sorted, Ident, do_tag,
        GuessDir, flow_stat, record_header, topN, quiet, pipe_output, csv_output,
        plain_numbers, &profile_data, orig_plain_numbers);
// added by lxh end
	nfprof_end(&profile_data, total_flows);

	if ( total_bytes == 0 ) {
		printf("No matched flows\n");
		exit(0);
	}
    OutputAndDestroy(aggregate, print_order, wfile, compress, bidir, date_sorted, Ident, print_record,
        limitflows, do_tag, GuessDir, flow_stat, record_header, topN, quiet, pipe_output, csv_output, &sum_stat,
        plain_numbers, &profile_data, element_stat, orig_plain_numbers);

	FreeExtensionMaps(&extension_map_list);
#ifdef DEVEL
	if ( hash_hit || hash_miss )
		printf("Hash hit: %i, miss: %i, skip: %i, ratio: %5.3f\n", hash_hit, hash_miss, hash_skip, (float)hash_hit/((float)(hash_hit+hash_miss)));
#endif

	return 0;
}

// moded by lxh start
static void OutputAndDestroy(int aggregate, char* print_order, char* wfile, int compress, int bidir,
    int date_sorted, char *Ident, printer_t print_record, uint32_t limitflows, int  do_tag,
    int GuessDir, int flow_stat, char* record_header, int topN, int quiet, int pipe_output,
    int csv_output, stat_record_t* sum_stat, int plain_numbers, nfprof_t* profile_data, int element_stat,
    int  orig_plain_numbers) {
  
    nfprof_end(profile_data, total_flows);

	if (aggregate || print_order) {
		if ( wfile ) {
			nffile_t *nffile = OpenNewFile(wfile, NULL, compress, is_anonymized, NULL);
			if ( !nffile ) 
				exit(255);
			if ( ExportFlowTable(nffile, aggregate, bidir, date_sorted) ) {
				CloseUpdateFile(nffile, Ident );	
			} else {
				CloseFile(nffile);
				unlink(wfile);
			}
			DisposeFile(nffile);
		} else {
			PrintFlowTable(print_record, limitflows, do_tag, GuessDir);
		}
	}

	if (flow_stat) {
		PrintFlowStat(record_header, print_record, topN, do_tag, quiet, csv_output);
#ifdef DEVEL
		printf("Loopcnt: %u\n", loopcnt);
#endif
        printf("\n");
	} 

	if (element_stat) {
		PrintElementStat(sum_stat, record_header, print_record, topN, do_tag, quiet, pipe_output, csv_output);
	} 

	if ( !quiet ) {
		if ( csv_output ) {
			PrintSummary(sum_stat, plain_numbers, csv_output);
		} else if ( !wfile ) {
			if (is_anonymized)
				printf("IP addresses anonymised\n");
			PrintSummary(sum_stat, plain_numbers, csv_output);
			if ( t_last_flow == 0 ) {
				// in case of a pre 1.6.6 collected and empty flow file
 				printf("Time window: <unknown>\n");
			} else {
 				printf("Time window: %s\n", TimeString(t_first_flow, t_last_flow));
			}
			printf("Total flows processed: %u, Blocks skipped: %u, Bytes read: %llu\n", 
				total_flows, skipped_blocks, (unsigned long long)total_bytes);
            printf("\n");
			nfprof_print(profile_data, stdout);
		}
	}

	Dispose_FlowTable();
	Dispose_StatTable();
    skipped_blocks = 0;
    total_flows = 0;
    total_bytes = 0;
    t_first_flow = 0x7fffffff;
    t_last_flow = 0;
    plain_numbers = orig_plain_numbers;
	memset((void *)sum_stat, 0, sizeof(stat_record_t));
	sum_stat->first_seen = 0x7fffffff;
	sum_stat->msec_first = 999;
    
	if ((aggregate || flow_stat || print_order)  && !Init_FlowTable() )
			exit(250);

	if (element_stat && !Init_StatTable(HashBits, NumPrealloc) )
			exit(250);

    nfprof_start(profile_data);
}
// moded by lxh start
