/*
 * $Id: mod_sslhaf.c 12 2009-10-06 10:01:43Z ivanr $
 * Copyright (c) 2009 Ivan Ristic. All rights reserved.
 *
 * THIS PRODUCT IS NOT READY FOR PRODUCTION USE. DEPLOY AT YOUR OWN RISK.
 * 
 * This product is released under the terms of the General Public Licence
 * version 2 (GPLv2).
 *
 * NO WARRANTY. YOU MUST READ AND AGREE TO THE LICENCE BEFORE YOU USE THIS PROGRAM.
 */
 
/*
 * This Apache module will extract the list of SSL cipher suites
 * offered by HTTP clients during the SSL negotiation phase. The idea
 * is that different clients use different SSL implementations and
 * configure them in different ways. By looking at the differences in
 * the cipher suites we should be able to identify clients, irrespective
 * of what they seem to be (looking at HTTP request headers).
 *
 * This way of fingerprinting is much more reliable than other approaches
 * (e.g. TCP/IP fingerprinting), for the following reasons:
 *
 * - HTTP proxies do not terminate SSL, which means that every client
 *   creates a unique data stream that is sent directly to servers.
 *
 * - NAT will modify TCP/IP packets, but leave the SSL data stream
 *   untouched.
 *
 *
 * Notes:
 *
 * - In this version the module only extracts the cipher suite list
 *   for logging. Future versions will use a database of known clients
 *   for identification.
 *
 * - Once the module nears completion I will write and release a whitepaper
 *   to document it.
 *
 * - A similar technique can be used to fingerprint SSL servers. I am
 *   working on that also.
 *
 *
 * To compile and install the module, do this:
 *
 *     # apxs -cia mod_sslhaf.c
 *
 * The above script will try to add a LoadModule statement to your
 * configuration file but it will fail if it can't find at least one
 * previous such statement. If that happens (you'll see the error
 * message) you'll need to add the following line manually:
 *
 *     LoadModule sslhaf_module /path/to/modules/mod_sslhaf.so
 *
 * You will also need to add a custom log to record cipher suite information.
 * For example (add to the virtual host where you want the fingerprinting
 * to take place):
 *
 *     CustomLog logs/sslhaf.log "%t %h \"%{SSLHAF_HANDSHAKE}e\" \
 *     \"%{SSLHAF_PROTOCOL}e\" \"%{SSLHAF_SUITES}e\" \"%{User-Agent}i\"" 
 *
 * As an example, these are the values you'd get from a visit by the Google
 * search engine:
 *
 *     SSLHAF_HANDSHAKE 	2
 *     SSLHAF_PROTOCOL		3.1
 *     SSLHAF_SUITES		04,010080,05,0a
 *
 * The tokens have the following meaning:
 *
 * - SSL_HANDSHAKE contains the handshake version used: 2 and 3 for an SSL v2 and SSL v3+
 *   handshake, respectively. You can see in the example that Google bot uses a SSLv2 handshake,
 *   which means that it is ready to use SSL v2 or better.
 *
 * - SSL_PROTOCOL The second token contains the best SSL/TLS version supported by the client. For
 *   example, SSLv3 is "3.0"; TLS 1.0 is "3.1"; TLS 1.1 is "3.2", etc.
 *
 * - SSLHAF_SUITES contains a list of the supported cipher suites. Each value, a hexadecimal number,
 *   corresponds to one cipher suite. From the example, 0x04 stands for SSL_RSA_WITH_RC4_128_MD5,
 *   0x010080 stands for SSL_CK_RC4_128_WITH_MD5 (a SSLv2 suite) and 0x05 stands
 *   for SSL_RSA_WITH_RC4_128_SHA.
 *
 * - SSLHAF_LOG is defined (and contains "1") only on the first request in a connection. This
 *   variable can be used to reduce the amount of logging (SSL parameters will typically not
 *   change across requests on the same connection). Example:
 *
 *   CustomLog logs/sslhaf.log "YOUR_LOG_STRING_HERE" env=SSLHAF_LOG
 *
 */

#include "ap_config.h" 

#include "apr_lib.h"
#include "apr_hash.h"
#include "apr_optional.h"
#include "apr_sha1.h"
#include "apr_strings.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"

#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_connection.h"
#include "http_log.h"
#include "http_protocol.h"

#include "mod_log_config.h"

module AP_MODULE_DECLARE_DATA sslhaf_module;

static const char sslhaf_filter_name[] = "FINGERPRINT_INPUT";

struct sslhaf_cfg_t {
    /* Inspection state; see above for the constants. */
    int state;

    /* The buffer we use to store the first SSL packet.
     * Allocated from the connection pool.
     */        
    unsigned char *buf;
    apr_size_t buf_len;
    apr_size_t buf_to_go;
    
    /* The client hello version used; 2 or 3. */
    unsigned int hello_version;
    
    /* SSL version indicated in the handshake. */
    unsigned int protocol_high;
    unsigned int protocol_low;
    
    /* How many suites are there? */
    unsigned int slen; 			
    
    /* Pointer to the first suite. Do note that a v3 suites consumes
     * 2 bytes whereas a v2 suite consumes 3 bytes. You need to check
     * hello_version before you access the suites.
     */
    const char *suites; 
    
    /* Handkshake version as string. */
    const char *thandshake;
    
    /* Protocol version number as string. */
    const char *tprotocol;
    
    /* Suites as text. */
    const char *tsuites;

    /* How many requests were there on this connection? */    
    unsigned int request_counter;

    /* SHA1 hash of the remote address. */
    const char *ipaddress_hash;
};

typedef struct sslhaf_cfg_t sslhaf_cfg_t;

#define STATE_START 	0
#define STATE_BUFFER 	1
#define STATE_GOAWAY	2

#define BUF_LIMIT 	1024

/**
 * Convert the bytes given on input into their hexadecimal representation.
 */
char *bytes2hex(apr_pool_t *pool, unsigned char *data, int len) {
    static unsigned char b2hex[] = "0123456789abcdef";
    char *hex = NULL;
    int i, j;

    hex = apr_palloc(pool, (len * 2) + 1);
    if (hex == NULL) return NULL;

    j = 0;
    for(i = 0; i < len; i++) {
        hex[j++] = b2hex[data[i] >> 4];
        hex[j++] = b2hex[data[i] & 0x0f];
    }
    
    hex[j] = '\0';

    return hex;
}

/**
 * Generate a sha1 hash of 
 */
char *generate_sha1(apr_pool_t *pool, char *data, int len) {
    unsigned char digest[APR_SHA1_DIGESTSIZE];
    apr_sha1_ctx_t context;

    apr_sha1_init(&context);
    apr_sha1_update(&context, (const char *)data, len);
    apr_sha1_final(digest, &context);

    return bytes2hex(pool, digest, APR_SHA1_DIGESTSIZE);
}

/**
 * Convert one byte into its hexadecimal representation.
 */
unsigned char *c2x(unsigned what, unsigned char *where) {
    static const char c2x_table[] = "0123456789abcdef";
    
    what = what & 0xff;
    *where++ = c2x_table[what >> 4];
    *where++ = c2x_table[what & 0x0f];
                
    return where;
}

/**
 * Decode SSLv2 packet.
 */
static int decode_packet_v2(ap_filter_t *f, sslhaf_cfg_t *cfg) {
    unsigned char *buf = cfg->buf;
    apr_size_t len = cfg->buf_len;
    int cslen;
    unsigned char *q;

    // There are 6 bytes before the list of cipher suites:
    // cipher suite length (2 bytes), session ID length (2 bytes)
    // and challenge length (2 bytes).
    if (len < 6) {
        return -1;
    }
    
    // How many bytes do the cipher suites consume?
    cslen = (buf[0] * 256) + buf[1];

    // Skip over to the list.    
    buf += 6;
    len -= 6;

    // Check that we have the suites in the buffer.
    if (len < (apr_size_t)cslen) {
        return -2;
    }

    // In SSLv2 each suite consumes 3 bytes.
    cslen = cslen / 3;
    
    // Keep the pointer to where the suites begin. The memory
    // was allocated from the connection pool, so it should
    // be around for as long as we need it.
    cfg->slen = cslen;
    cfg->suites = (const char *)buf;
    
    cfg->thandshake = apr_psprintf(f->c->pool, "%i", cfg->hello_version);
    cfg->tprotocol = apr_psprintf(f->c->pool, "%i.%i", cfg->protocol_high, cfg->protocol_low);
                
    // Create a list of suites as text, for logging. Each 3-byte
    // suite can consume up to 6 bytes (in hexadecimal form) with
    // an additional byte for a comma. We need 9 bytes at the
    // beginning (handshake and version), as well as a byte for
    // the terminating NUL byte.
    q = apr_pcalloc(f->c->pool, (cslen * 7) + 1);
    if (q == NULL) {
        return -3;
    }
    
    cfg->tsuites = (const char *)q;
    
    // Extract cipher suites; each suite consists of 3 bytes.
    while(cslen--) {
        if ((const char *)q != cfg->tsuites) {
            *q++ = ',';
        }

        if (*buf != 0) {
            c2x(*buf, q);
            q += 2;
            
            c2x(*(buf + 1), q);
            q += 2;
        } else {
            if (*(buf + 1) != 0) {
                c2x(*(buf + 1), q);
                q += 2;
            }
        }
        
        c2x(*(buf + 2), q);
        q += 2;
                    
        buf += 3;
    }
            
    *q = '\0';

    return 1;
}

/**
 * Decode SSLv3 packet.
 */
static int decode_packet_v3(ap_filter_t *f, sslhaf_cfg_t *cfg) {
    unsigned char *buf = cfg->buf;
    apr_size_t len = cfg->buf_len;

    // Loop while there's data in buffer.
    while(len > 0) {
        apr_size_t ml;
        int mt;

        // Check for size first        
        if (len < 4) {
            return -1;
        }
        
        // Extract message meta data
        mt = buf[0]; // message type
        ml = (buf[1] * 65536) + (buf[2] * 256) + buf[3]; // message length
        
        // Does the message length correspond to the size
        // of our buffer?
        if (ml > len - 4) {
            return -2;
        }
        
        // Is this a Client Hello message?    
        if (mt == 1) {
            unsigned char *p = buf + 4; // skip over the message type and length
            unsigned char *q;
            apr_size_t mylen = ml;
            int idlen;
            int cslen;
            
            if (mylen < 34) { // for the version number and random value
                return -3;
            }
            
            p += 2; // version number
            p += 32; // random value
            mylen -= 34;
            
            if (mylen < 1) { // for the ID length byte
                return -4;
            }
            
            idlen = *p;
            p += 1; // ID len
            mylen -= 1;
            
            if (mylen < (apr_size_t)idlen) { // for the ID
                return -5;
            }
                
            p += idlen; // ID
            mylen -= idlen;
            
            if (mylen < 2) { // for the CS length bytes
                return -6;
            }
            
            cslen = (*p * 256) + *(p + 1);
            cslen = cslen / 2; // each suite consumes 2 bytes
            
            p += 2; // Cipher Suites len
            mylen -= 2;
            
            if (mylen < (apr_size_t)cslen * 2) { // for the suites
                return -7;
            }
                
            // Keep the pointer to where the suites begin. The memory
            // was allocated from the connection pool, so it should
            // be around for as long as we need it.
            cfg->slen = cslen;
            cfg->suites = (const char *)p;
            
            cfg->thandshake = apr_psprintf(f->c->pool, "%i", cfg->hello_version);
            cfg->tprotocol = apr_psprintf(f->c->pool, "%i.%i", cfg->protocol_high, cfg->protocol_low);
            
            // Create a list of suites as text, for logging
            q = apr_pcalloc(f->c->pool, (cslen * 7) + 1);            
            cfg->tsuites = (const char *)q;
            
            // Extract cipher suites; each suite consists of 2 bytes
            while(cslen--) {
                if ((const char *)q != cfg->tsuites) {
                    *q++ = ',';
                }

                if (*p != 0) {
                    c2x(*p, q);
                    q += 2;
                }
                
                c2x(*(p + 1), q);
                q += 2;
                    
                p += 2;
            }
            
            *q = '\0';
        }
            
        // Skip over the message
        len -= 4;
        len -= ml;
        buf += 4;
        buf += ml;              
    }
    
    return 1;
}

/**
 * Deal with a single bucket. We look for a handshake SSL packet, buffer
 * it (possibly across several invocations), then invoke a function to analyse it.
 */
static int decode_bucket(ap_filter_t *f, sslhaf_cfg_t *cfg,
    const unsigned char *inputbuf, apr_size_t inputlen)
{
    // Loop while there's input to process
    while(inputlen > 0) {
        // Are we looking for a handshake packet?
        if (cfg->state == STATE_START) {
            apr_size_t len;
            
            // We're expecting a handshake packet
            if ((inputbuf[0] != 22)&&(inputbuf[0] != 128)) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, f->c->base_server,
                    "mod_sslhaf: first byte (%d) of this connection does not indicate SSL; skipping", inputbuf[0]);
                return -1;
            }

            if (inputbuf[0] == 22) { // SSLv3 and better
                // Go over packet type            
                inputbuf++;
                inputlen--;
                
                // Are there enough bytes to begin analysis?            
                if (inputlen < 4) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: internal error; less than 5 bytes from the packet available in this bucket");
                    return -1;
                }
                
                cfg->hello_version = 3;
                cfg->protocol_high = inputbuf[0];
                cfg->protocol_low = inputbuf[1];
            
                // SSL version
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, f->c->base_server,
                    "mod_sslhaf: SSL version %d.%d (v3 hello)", cfg->protocol_high, cfg->protocol_low);
                
                // SSL version sanity check
                if ((cfg->protocol_high != 3)||(cfg->protocol_low > 9)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: SSL version does not make sense: %d.%d", cfg->protocol_high, cfg->protocol_low);
                    return -1;
                }
                
                // Go over the version bytes
                inputbuf += 2;
                inputlen -= 2;                
            
                // Get packet length
                len = (inputbuf[0] * 256) + inputbuf[1];
            
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, f->c->base_server,
                    "mod_sslhaf: packet length %" APR_SIZE_T_FMT, len);
            
                // Limit what we are willing to accept.
                if ((len <= 0)||(len > BUF_LIMIT)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: internal error; invalid packet length %" APR_SIZE_T_FMT, len);
                    return -1;
                }
            
                // Go over the packet length bytes
                inputbuf += 2;
                inputlen -= 2;
        
                // Allocate a buffer to hold the entire packet            
                cfg->buf = apr_pcalloc(f->c->pool, len);
                if (cfg->buf == NULL) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: failed to allocate %" APR_SIZE_T_FMT " bytes", len);
                    return -1;
                }

                // Go into buffering mode            
                cfg->state = STATE_BUFFER;
                cfg->buf_to_go = len;
            }
            
            if (inputbuf[0] == 128) { // Possible SSLv2 ClientHello
                // Go over packet type            
                inputbuf++;
                inputlen--;
                
                // Are there enough bytes to begin analysis?            
                if (inputlen < 4) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: internal error; less than 5 bytes from the packet available in this bucket");
                    return -1;
                }
                
                // Check that it is indeed ClientHello
                if (inputbuf[1] != 1) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: Not SSLv2 ClientHello (%d)", inputbuf[1]);
                    return -1;
                }
                
                cfg->hello_version = 2;
                if ((inputbuf[2] == 0x00)&&(inputbuf[3] == 0x02)) {
                    // SSL v2 uses 0x0002 for the version number
                    cfg->protocol_high = inputbuf[3];
                    cfg->protocol_low = inputbuf[2];
                } else {
                    // SSL v3 will use 0x0300, 0x0301, etc.
                    cfg->protocol_high = inputbuf[2];
                    cfg->protocol_low = inputbuf[3];
                }
                
                // SSL version
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, f->c->base_server,
                    "mod_sslhaf: SSL version %d.%d (v2 hello)", cfg->protocol_high, cfg->protocol_low);
                    
                // SSL version sanity check
                if (((cfg->protocol_high != 3)&&(cfg->protocol_high != 2))||(cfg->protocol_low > 9)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: SSL version does not make sense: %d.%d", cfg->protocol_high, cfg->protocol_low);
                    return -1;
                }
                
                len = inputbuf[0] - 3; // We've already consumed 3 bytes from the packet.

                // Limit what we are willing to accept.
                if ((len <= 0)||(len > BUF_LIMIT)) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: internal error; invalid packet length %" APR_SIZE_T_FMT, len);
                    return -1;
                }
            
                // Go over the packet length (1 byte), message
                // type (1 byte) and version (2 bytes).
                inputbuf += 4;
                inputlen -= 4;
        
                // Allocate a buffer to hold the entire packet            
                cfg->buf = apr_pcalloc(f->c->pool, len);
                if (cfg->buf == NULL) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: failed to allocate %" APR_SIZE_T_FMT " bytes", len);
                    return -1;
                }

                // Go into buffering mode            
                cfg->state = STATE_BUFFER;
                cfg->buf_to_go = len;
            }
        }

        // Are we buffering?        
        if (cfg->state == STATE_BUFFER) {
            if (cfg->buf_to_go <= inputlen) {
                int rc;
                
                // We have enough data to complete this packet
                memcpy(cfg->buf + cfg->buf_len, inputbuf, cfg->buf_to_go);
                cfg->buf_len += cfg->buf_to_go;
                inputbuf += cfg->buf_to_go;
                inputlen -= cfg->buf_to_go;
                cfg->buf_to_go = 0;
                
                // Decode the packet now
                if (cfg->hello_version == 3) {
                    rc = decode_packet_v3(f, cfg);
                } else {
                    rc = decode_packet_v2(f, cfg);
                }
                
                if (rc < 0) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
                        "mod_sslhaf: packet decoding error rc %d (hello %d)",
                        rc, cfg->hello_version);
                    return -1;
                }
                
                // TODO For now assume we've received what we wanted in
                //      the first packet.
                // cfg->state = STATE_START;

                // The state will be updated by the caller
                // based on our return status.
                // cfg->state = STATE_GOAWAY;                
                return 0;
            } else {
                // There's not enough data; copy what we can and
                // we'll get the rest later.
                memcpy(cfg->buf + cfg->buf_len, inputbuf, inputlen);
                cfg->buf_len += inputlen;
                cfg->buf_to_go -= inputlen;
                inputbuf += inputlen;
                inputlen = 0;
            }
        }
    }
    
    return 1;
}

/**
 * This input filter will basicall sniff on a connection and analyse
 * the packets when it detects SSL.
 */
static apr_status_t sslhaf_in_filter(ap_filter_t *f,
                                    apr_bucket_brigade *bb,
                                    ap_input_mode_t mode,
                                    apr_read_type_e block,
                                    apr_off_t readbytes)
{
    sslhaf_cfg_t *cfg = ap_get_module_config(f->c->conn_config, &sslhaf_module);
    apr_status_t status;
    apr_bucket *bucket;
    
    // Return straight away if there's no configuration
    if (cfg == NULL) {
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }
    
    // Sanity check first.
    if (cfg->state == STATE_GOAWAY) {
        // ap_log_error(APLOG_MARK, APLOG_ERR, 0, f->c->base_server,
        //    "mod_sslhaf: internal error; seen STATE_GOAWAY");
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }

    // Get brigade
    status = ap_get_brigade(f->next, bb, mode, block, readbytes);
    if (status != APR_SUCCESS) {
        // Do not log, since we're passing the status anyway.
        // ap_log_error(APLOG_MARK, APLOG_ERR, status, f->c->base_server,
        //    "mod_sslhaf: error while retrieving brigade");
        cfg->state = STATE_GOAWAY;
        return status;
    }

    // Loop through the buckets
    for(bucket = APR_BRIGADE_FIRST(bb);
        bucket != APR_BRIGADE_SENTINEL(bb);
        bucket = APR_BUCKET_NEXT(bucket))
    {
        const char *buf = NULL;
        apr_size_t buflen = 0;
        
        if (!(APR_BUCKET_IS_METADATA(bucket))) {
            status = apr_bucket_read(bucket, &buf, &buflen, APR_BLOCK_READ);
            if (status != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, status, f->c->base_server,
                    "mod_sslhaf: error while reading bucket");
                return status;
            }
            
            // Look into the bucket                
            if (decode_bucket(f, cfg, (const unsigned char *)buf, buflen) <= 0) {
                cfg->state = STATE_GOAWAY;
            }
        }
    }
    
    return APR_SUCCESS;
}

/**
 * Attach our filter to every incoming connection.
 */
static int sslhaf_pre_conn(conn_rec *c, void *csd) {
    sslhaf_cfg_t *cfg = NULL;
    
    // TODO Can we determine if SSL is enabled on this connection
    //      and don't bother if it isn't? It is actually possible that
    //      someone speaks SSL on a non-SSL connection, but we won't
    //      be able to detect that. It wouldn't matter, though, because
    //      Apache will not process such a request.

    cfg = apr_pcalloc(c->pool, sizeof(*cfg));
    if (cfg == NULL) return OK;
    
    ap_set_module_config(c->conn_config, &sslhaf_module, cfg);

    ap_add_input_filter(sslhaf_filter_name, NULL, NULL, c);
    
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, c->base_server,
        "mod_sslhaf: connection from %s", c->remote_ip);

    return OK;
}

/**
 * Take the textual representation of the client's cipher suite
 * list and attach it to the request.
 */
static int sslhaf_post_request(request_rec *r) {
    sslhaf_cfg_t *cfg = ap_get_module_config(r->connection->conn_config, &sslhaf_module);
    
    if ((cfg != NULL)&&(cfg->tsuites != NULL)) {
        /* Make the handshake information available to other modules. */
        apr_table_setn(r->subprocess_env, "SSLHAF_HANDSHAKE", cfg->thandshake);
        apr_table_setn(r->subprocess_env, "SSLHAF_PROTOCOL", cfg->tprotocol);
        apr_table_setn(r->subprocess_env, "SSLHAF_SUITES", cfg->tsuites);

        /* Keep track of how many requests there were. */
        cfg->request_counter++;
        
        /* Help to log only once per connection. */        
        if (cfg->request_counter == 1) {
            apr_table_setn(r->subprocess_env, "SSLHAF_LOG", "1");
        }
        
        #if 0
        /* Generate a sha1 of the remote address on the first request. */
        if (cfg->ipaddress_hash == NULL) {
            cfg->ipaddress_hash = generate_sha1(r->connection->pool,
                r->connection->remote_ip, strlen(r->connection->remote_ip));
        }
        
        apr_table_setn(r->subprocess_env, "SSLHAF_IP_HASH", cfg->ipaddress_hash);
        #endif
    }
    
    return DECLINED;
}

/**
 * Main entry point.
 */
static void register_hooks(apr_pool_t *p) {
    static const char * const afterme[] = { "mod_security2.c", NULL };
    
    ap_hook_pre_connection(sslhaf_pre_conn, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(sslhaf_post_request, NULL, afterme, APR_HOOK_REALLY_FIRST);

    ap_register_input_filter(sslhaf_filter_name, sslhaf_in_filter,
        NULL, AP_FTYPE_NETWORK - 1);
}

module AP_MODULE_DECLARE_DATA sslhaf_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-dir config */
    NULL,                       /* merge per-dir config */
    NULL,                       /* server config */
    NULL,                       /* merge server config */
    NULL,                       /* command apr_table_t */
    register_hooks              /* register hooks */
};
