/* ipv4.h */
/*
 * Routines for dissecting IPv4 and UDP packets.
 */

#ifndef _ipv4_h
#define _ipv4_h

#include <stdint.h>

/*! This is all held in host byte order (including the
 *  IP addresses! You have been warned .. )
 */
typedef struct ipv4_header_s
{
  // IP version. Should really be 4 :-)
  uint8_t version;

  // Header length.
  uint8_t hdr_length;

  // Type of service.
  uint8_t serv_type;

  //! Total length
  uint16_t length;

  //! Ident
  uint16_t ident;

  //! Flags
  uint8_t flags;

  //! Frag offset.
  uint16_t frag_offset;

  //! TTL
  uint8_t ttl;

  //! Protocol (typically UDP or TCP)
  uint8_t proto;

  //! Header checksum (we don't check this!)
  uint16_t csum;

  //! Source address.
  uint32_t src_addr;

  //! Destination address.
  uint32_t dest_addr;

  //! We don't track options.

} ipv4_header_t;

// Is this header UDP?
#define IPV4_HDR_IS_UDP(h) ((h)->proto == 17)

#define IPV4_ERR_PKT_TOO_SHORT (-1)

/*! 
 * Unwrap ipv4.
 * 
 * \param out_st OUT Index into data at which IPv4 payload starts.
 * \param out_len OUT Length of the IPv4 payload.
 * \return 0 on success, -1 on failure.
 */
int ipv4_from_payload(const uint8_t *data, 
		      const uint32_t len,
		      ipv4_header_t *out_hdr,
		      uint32_t *out_st,
		      uint32_t *out_len);


typedef struct ipv4_udp_header_s
{
  //! Source port.
  uint16_t source_port;

  //! Dest port.
  uint16_t dest_port;

  //! Length (yes, yet another one!)
  uint16_t length;

  //! Checksum
  uint16_t csum;
} ipv4_udp_header_t;


/*! 
 * Unwrap UDP.
 *
 */
int ipv4_udp_from_payload(const uint8_t *data,
			  const uint32_t len,
			  ipv4_udp_header_t *out_hdr,
			  uint32_t *out_st,
			  uint32_t *out_len);




#endif

/* End file */

