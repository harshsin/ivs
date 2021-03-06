/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

/*
 * Translate between OpenFlow matches (of_match_t), internal
 * match structure (struct ind_ovs_cfr), and OVS flow key
 * (struct ind_ovs_parsed_key / nlattrs).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize (4)
#endif
#include "ovs_driver_int.h"
#include <byteswap.h>
#include <linux/if_ether.h>

/* Recursive (for encap) helper for ind_ovs_parse_key */
static void
ind_ovs_parse_key__(struct nlattr *key, struct ind_ovs_parsed_key *pkey)
{
    struct nlattr *attrs[OVS_KEY_ATTR_MAX+1];
    if (nla_parse_nested(attrs, OVS_KEY_ATTR_MAX, key, NULL) < 0) {
        abort();
    }

#define field(attr, name, type) \
    if (attrs[attr]) { \
        assert(sizeof(type) == sizeof(pkey->name)); \
        memcpy(&pkey->name, nla_data(attrs[attr]), sizeof(type)); \
        ATTR_BITMAP_SET(pkey->populated, (attr)); \
    }
    OVS_KEY_FIELDS
#undef field

    if (attrs[OVS_KEY_ATTR_ENCAP]) {
        ind_ovs_parse_key__(attrs[OVS_KEY_ATTR_ENCAP], pkey);
    }

    if (attrs[OVS_KEY_ATTR_TUNNEL]) {
        struct nlattr *tunnel_attrs[OVS_TUNNEL_KEY_ATTR_MAX+1];
        if (nla_parse_nested(tunnel_attrs, OVS_TUNNEL_KEY_ATTR_MAX,
                             attrs[OVS_KEY_ATTR_TUNNEL], NULL) < 0) {
            abort();
        }

#define field(attr, name, type) \
        if (tunnel_attrs[attr]) { \
            assert(sizeof(type) == sizeof(pkey->tunnel.name)); \
            memcpy(&pkey->tunnel.name, nla_data(tunnel_attrs[attr]), sizeof(type)); \
        }
        OVS_TUNNEL_KEY_FIELDS
#undef field
    }
}

void
ind_ovs_parse_key(struct nlattr *key, struct ind_ovs_parsed_key *pkey)
{
    pkey->populated = 0;
    pkey->in_port = -1;
    pkey->tunnel.id = 0;
    pkey->tunnel.ipv4_src = 0;
    pkey->tunnel.ipv4_dst = 0;
    pkey->tunnel.tos = 0;
    pkey->tunnel.ttl = 64;
    ind_ovs_parse_key__(key, pkey);
    assert(ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ETHERNET));
}

/* Should only be used when creating the match for a packet-in */
void
ind_ovs_key_to_match(const struct ind_ovs_parsed_key *pkey,
                     of_match_t *match)
{
    memset(match, 0, sizeof(*match));

    /* We only populate the masks for this OF version */
    match->version = ind_ovs_version;

    of_match_fields_t *fields = &match->fields;

    assert(ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_IN_PORT));
    fields->in_port = pkey->in_port;
    OF_MATCH_MASK_IN_PORT_EXACT_SET(match);

    assert(ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ETHERNET));
    memcpy(&fields->eth_dst, pkey->ethernet.eth_dst, OF_MAC_ADDR_BYTES);
    memcpy(&fields->eth_src, pkey->ethernet.eth_src, OF_MAC_ADDR_BYTES);
    OF_MATCH_MASK_ETH_DST_EXACT_SET(match);
    OF_MATCH_MASK_ETH_SRC_EXACT_SET(match);

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ETHERTYPE)) {
        fields->eth_type = ntohs(pkey->ethertype);
        if (fields->eth_type <= OF_DL_TYPE_NOT_ETH_TYPE) {
            fields->eth_type = OF_DL_TYPE_NOT_ETH_TYPE;
        }
    } else {
        fields->eth_type = OF_DL_TYPE_NOT_ETH_TYPE;
    }
    OF_MATCH_MASK_ETH_TYPE_EXACT_SET(match);

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_VLAN)) {
        fields->vlan_vid = VLAN_VID(ntohs(pkey->vlan));
        fields->vlan_pcp = VLAN_PCP(ntohs(pkey->vlan));
        if (ind_ovs_version == OF_VERSION_1_3) {
            fields->vlan_vid |= VLAN_CFI_BIT;
        }
    } else {
        if (ind_ovs_version == OF_VERSION_1_0) {
            fields->vlan_vid = -1;
        } else {
            fields->vlan_vid = 0;
        }
        fields->vlan_pcp = 0;
    }
    OF_MATCH_MASK_VLAN_VID_EXACT_SET(match);
    OF_MATCH_MASK_VLAN_PCP_EXACT_SET(match);

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_IPV4)) {
        fields->ipv4_src = ntohl(pkey->ipv4.ipv4_src);
        fields->ipv4_dst = ntohl(pkey->ipv4.ipv4_dst);
        fields->ip_dscp = pkey->ipv4.ipv4_tos;
        fields->ip_proto = pkey->ipv4.ipv4_proto;
        OF_MATCH_MASK_IPV4_SRC_EXACT_SET(match);
        OF_MATCH_MASK_IPV4_DST_EXACT_SET(match);
        OF_MATCH_MASK_IP_DSCP_EXACT_SET(match);
        OF_MATCH_MASK_IP_PROTO_EXACT_SET(match);
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_IPV6)) {
        memcpy(&fields->ipv6_src, pkey->ipv6.ipv6_src, OF_IPV6_BYTES);
        memcpy(&fields->ipv6_dst, pkey->ipv6.ipv6_dst, OF_IPV6_BYTES);
        fields->ipv6_flabel = ntohl(pkey->ipv6.ipv6_label);
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ARP)) {
        fields->arp_op = ntohs(pkey->arp.arp_op);
        fields->arp_spa = ntohl(pkey->arp.arp_sip);
        fields->arp_tpa = ntohl(pkey->arp.arp_tip);
        memcpy(&fields->arp_sha, pkey->arp.arp_sha, OF_MAC_ADDR_BYTES);
        memcpy(&fields->arp_tha, pkey->arp.arp_tha, OF_MAC_ADDR_BYTES);

        /* Special case ARP for OF 1.0 */
        if (ind_ovs_version == OF_VERSION_1_0) {
            fields->ipv4_src = ntohl(pkey->arp.arp_sip);
            fields->ipv4_dst = ntohl(pkey->arp.arp_tip);
            fields->ip_proto = ntohs(pkey->arp.arp_op) & 0xFF;
            OF_MATCH_MASK_IPV4_SRC_EXACT_SET(match);
            OF_MATCH_MASK_IPV4_DST_EXACT_SET(match);
            OF_MATCH_MASK_IP_PROTO_EXACT_SET(match);
        }
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_TCP)) {
        fields->tcp_dst = ntohs(pkey->tcp.tcp_dst);
        fields->tcp_src = ntohs(pkey->tcp.tcp_src);
        OF_MATCH_MASK_TCP_DST_EXACT_SET(match);
        OF_MATCH_MASK_TCP_SRC_EXACT_SET(match);
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_UDP)) {
        fields->udp_dst = ntohs(pkey->udp.udp_dst);
        fields->udp_src = ntohs(pkey->udp.udp_src);

        /* Special case UDP for OF 1.0 */
        if (ind_ovs_version == OF_VERSION_1_0) {
            fields->tcp_dst = ntohs(pkey->udp.udp_dst);
            fields->tcp_src = ntohs(pkey->udp.udp_src);
            OF_MATCH_MASK_TCP_DST_EXACT_SET(match);
            OF_MATCH_MASK_TCP_SRC_EXACT_SET(match);
        }
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ICMP)) {
        fields->icmpv4_type = pkey->icmp.icmp_type;
        fields->icmpv4_code = pkey->icmp.icmp_code;

        /* Special case ICMP for OF 1.0 */
        if (ind_ovs_version == OF_VERSION_1_0) {
            fields->tcp_dst = pkey->icmp.icmp_code;
            fields->tcp_src = pkey->icmp.icmp_type;
            OF_MATCH_MASK_TCP_DST_EXACT_SET(match);
            OF_MATCH_MASK_TCP_SRC_EXACT_SET(match);
        }
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ICMPV6)) {
        fields->icmpv6_type = pkey->icmpv6.icmpv6_type;
        fields->icmpv6_code = pkey->icmpv6.icmpv6_code;
    }

    /*
     * Not supported by OVS:
     * sctp_dst, sctp_src, ipv6_nd_target, ipv6_nd_sll, ipv6_nd_tll,
     * mpls_label, mpls_tc, ip_ecn, in_phy_port, metadata
    */
}

void
ind_ovs_key_to_cfr(const struct ind_ovs_parsed_key *pkey,
                   struct ind_ovs_cfr *cfr)
{
    cfr->in_port = pkey->in_port;

    /* Set a bit in the in_ports bitmap */
    {
        uint32_t idx = aim_imin(IVS_MAX_BITMAP_IN_PORT, pkey->in_port);
        uint32_t word = IVS_MAX_BITMAP_IN_PORT/32 - idx/32;
        uint32_t bit = idx % 32;
        memset(cfr->in_ports, 0, sizeof(cfr->in_ports));
        cfr->in_ports[word] = 1 << bit;
    }

    memcpy(cfr->dl_dst, pkey->ethernet.eth_dst, OF_MAC_ADDR_BYTES);
    memcpy(cfr->dl_src, pkey->ethernet.eth_src, OF_MAC_ADDR_BYTES);

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ETHERTYPE)) {
        cfr->dl_type = pkey->ethertype;
        if (ntohs(cfr->dl_type) <= OF_DL_TYPE_NOT_ETH_TYPE) {
            cfr->dl_type = htons(OF_DL_TYPE_NOT_ETH_TYPE);
        }
    } else {
        cfr->dl_type = htons(OF_DL_TYPE_NOT_ETH_TYPE);
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_VLAN)) {
        cfr->dl_vlan = pkey->vlan | htons(VLAN_CFI_BIT);
    } else {
        cfr->dl_vlan = 0;
    }

    memset(cfr->ipv6_src, 0, sizeof(cfr->ipv6_src));
    memset(cfr->ipv6_dst, 0, sizeof(cfr->ipv6_dst));

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_IPV4)) {
        cfr->nw_tos = pkey->ipv4.ipv4_tos;
        cfr->nw_proto = pkey->ipv4.ipv4_proto;
        cfr->nw_src = pkey->ipv4.ipv4_src;
        cfr->nw_dst = pkey->ipv4.ipv4_dst;
    } else if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_IPV6)) {
        cfr->nw_tos = pkey->ipv6.ipv6_tclass;
        cfr->nw_proto = pkey->ipv6.ipv6_proto;
        memcpy(&cfr->ipv6_src, &pkey->ipv6.ipv6_src, OF_IPV6_BYTES);
        memcpy(&cfr->ipv6_dst, &pkey->ipv6.ipv6_dst, OF_IPV6_BYTES);
        cfr->nw_src = 0;
        cfr->nw_dst = 0;
        /* TODO flow label */
    } else if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ARP)) {
        cfr->nw_tos = 0;
        cfr->nw_proto = ntohs(pkey->arp.arp_op) & 0xFF;
        cfr->nw_src = pkey->arp.arp_sip;
        cfr->nw_dst = pkey->arp.arp_tip;
    } else {
        cfr->nw_tos = 0;
        cfr->nw_proto = 0;
        cfr->nw_src = 0;
        cfr->nw_dst = 0;
    }

    if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_TCP)) {
        cfr->tp_src = pkey->tcp.tcp_src;
        cfr->tp_dst = pkey->tcp.tcp_dst;
    } else if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_UDP)) {
        cfr->tp_src = pkey->udp.udp_src;
        cfr->tp_dst = pkey->udp.udp_dst;
    } else if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ICMP)) {
        cfr->tp_src = pkey->icmp.icmp_type << 8;
        cfr->tp_dst = pkey->icmp.icmp_code << 8;
    } else if (ATTR_BITMAP_TEST(pkey->populated, OVS_KEY_ATTR_ICMPV6)) {
        cfr->tp_src = pkey->icmpv6.icmpv6_type << 8;
        cfr->tp_dst = pkey->icmpv6.icmpv6_code << 8;
    } else {
        cfr->tp_src = 0;
        cfr->tp_dst = 0;
    }

    cfr->lag_id = 0;
    cfr->vrf = 0;
    cfr->l3_interface_class_id = 0;
    cfr->l3_src_class_id = 0;
    cfr->l3_dst_class_id = 0;
    cfr->global_vrf_allowed = 0;
    cfr->pad = 0;
}

void
ind_ovs_match_to_cfr(const of_match_t *match,
                     struct ind_ovs_cfr *fields, struct ind_ovs_cfr *masks)
{
    /* TODO support OF 1.1+ match fields */

    memset(fields, 0, sizeof(*fields));
    memset(masks, 0, sizeof(*masks));

    /* input port */
    fields->in_port = match->fields.in_port;
    masks->in_port = match->masks.in_port;

    masks->in_ports[0] = match->masks.bsn_in_ports_128.hi >> 32;
    masks->in_ports[1] = match->masks.bsn_in_ports_128.hi;
    masks->in_ports[2] = match->masks.bsn_in_ports_128.lo >> 32;
    masks->in_ports[3] = match->masks.bsn_in_ports_128.lo;

    /* ether addrs */
    memcpy(fields->dl_dst, &match->fields.eth_dst, OF_MAC_ADDR_BYTES);
    memcpy(fields->dl_src, &match->fields.eth_src, OF_MAC_ADDR_BYTES);
    memcpy(masks->dl_src, &match->masks.eth_src, OF_MAC_ADDR_BYTES);
    memcpy(masks->dl_dst, &match->masks.eth_dst, OF_MAC_ADDR_BYTES);

    /* ether type */
    fields->dl_type = htons(match->fields.eth_type);
    masks->dl_type = htons(match->masks.eth_type);

    /* vlan & pcp are combined, with CFI bit indicating tagged */
    if (match->version == OF_VERSION_1_0) {
        if (match->masks.vlan_vid == 0) {
            /* wildcarded */
            fields->dl_vlan = 0;
            masks->dl_vlan = 0;
        } else if (match->fields.vlan_vid == (uint16_t)-1) {
            /* untagged */
            fields->dl_vlan = 0;
            masks->dl_vlan = 0xffff;
        } else {
            /* tagged */
            fields->dl_vlan = htons(VLAN_CFI_BIT | VLAN_TCI(match->fields.vlan_vid, match->fields.vlan_pcp));
            masks->dl_vlan = htons(VLAN_CFI_BIT | VLAN_TCI(match->masks.vlan_vid, match->masks.vlan_pcp));
        }
    } else if (match->version == OF_VERSION_1_1) {
        NYI(0);
    } else {
        /* CFI bit indicating 'present' is included in the VID match field */
        fields->dl_vlan = htons(VLAN_TCI_WITH_CFI(match->fields.vlan_vid, match->fields.vlan_pcp));
        masks->dl_vlan = htons(VLAN_TCI_WITH_CFI(match->masks.vlan_vid, match->masks.vlan_pcp));
    }

    if (match->version < OF_VERSION_1_2) {
        fields->nw_proto = match->fields.ip_proto;
        masks->nw_proto = match->masks.ip_proto;

        fields->nw_tos = match->fields.ip_dscp & 0xFC;
        masks->nw_tos = match->masks.ip_dscp & 0xFC;

        fields->nw_src = htonl(match->fields.ipv4_src);
        fields->nw_dst = htonl(match->fields.ipv4_dst);
        masks->nw_src = htonl(match->masks.ipv4_src);
        masks->nw_dst = htonl(match->masks.ipv4_dst);

        fields->tp_src = htons(match->fields.tcp_src);
        fields->tp_dst = htons(match->fields.tcp_dst);
        masks->tp_src = htons(match->masks.tcp_src);
        masks->tp_dst = htons(match->masks.tcp_dst);
    } else {
        /* subsequent fields are type dependent */
        if (match->fields.eth_type == ETH_P_IP
            || match->fields.eth_type == ETH_P_IPV6) {
            fields->nw_proto = match->fields.ip_proto;
            masks->nw_proto = match->masks.ip_proto;

            fields->nw_tos = ((match->fields.ip_dscp & 0x3f) << 2) | (match->fields.ip_ecn & 0x3);
            masks->nw_tos = ((match->masks.ip_dscp & 0x3f) << 2) | (match->masks.ip_ecn & 0x3);

            if (match->fields.eth_type == ETH_P_IP) {
                fields->nw_src = htonl(match->fields.ipv4_src);
                fields->nw_dst = htonl(match->fields.ipv4_dst);
                masks->nw_src = htonl(match->masks.ipv4_src);
                masks->nw_dst = htonl(match->masks.ipv4_dst);
            } else if (match->fields.eth_type == ETH_P_IPV6) {
                memcpy(&fields->ipv6_src, &match->fields.ipv6_src, OF_IPV6_BYTES);
                memcpy(&fields->ipv6_dst, &match->fields.ipv6_dst, OF_IPV6_BYTES);
                memcpy(&masks->ipv6_src, &match->masks.ipv6_src, OF_IPV6_BYTES);
                memcpy(&masks->ipv6_dst, &match->masks.ipv6_dst, OF_IPV6_BYTES);
            }

            if (match->fields.ip_proto == IPPROTO_TCP) {
                fields->tp_src = htons(match->fields.tcp_src);
                fields->tp_dst = htons(match->fields.tcp_dst);
                masks->tp_src = htons(match->masks.tcp_src);
                masks->tp_dst = htons(match->masks.tcp_dst);
            } else if (match->fields.ip_proto == IPPROTO_UDP) {
                fields->tp_src = htons(match->fields.udp_src);
                fields->tp_dst = htons(match->fields.udp_dst);
                masks->tp_src = htons(match->masks.udp_src);
                masks->tp_dst = htons(match->masks.udp_dst);
            } else if (match->fields.ip_proto == IPPROTO_ICMP) {
                fields->tp_src = htons(match->fields.icmpv4_type);
                fields->tp_dst = htons(match->fields.icmpv4_code);
                masks->tp_src = htons(match->masks.icmpv4_type);
                masks->tp_dst = htons(match->masks.icmpv4_code);
            } else if (match->fields.ip_proto == IPPROTO_ICMPV6) {
                fields->tp_src = htons(match->fields.icmpv6_type);
                fields->tp_dst = htons(match->fields.icmpv6_code);
                masks->tp_src = htons(match->masks.icmpv6_type);
                masks->tp_dst = htons(match->masks.icmpv6_code);
            }
        } else if (match->fields.eth_type == ETH_P_ARP) {
            fields->nw_proto = match->fields.arp_op & 0xff;
            masks->nw_proto = match->masks.arp_op & 0xff;

            fields->nw_src = htonl(match->fields.arp_spa);
            fields->nw_dst = htonl(match->fields.arp_tpa);
            masks->nw_src = htonl(match->masks.arp_spa);
            masks->nw_dst = htonl(match->masks.arp_tpa);
        }
    }

    /* Metadata */
    fields->lag_id = match->fields.bsn_lag_id;
    masks->lag_id = match->masks.bsn_lag_id;
    fields->vrf = match->fields.bsn_vrf;
    masks->vrf = match->masks.bsn_vrf;
    fields->l3_interface_class_id = match->fields.bsn_l3_interface_class_id;
    masks->l3_interface_class_id = match->masks.bsn_l3_interface_class_id;
    fields->l3_src_class_id = match->fields.bsn_l3_src_class_id;
    masks->l3_src_class_id = match->masks.bsn_l3_src_class_id;
    fields->l3_dst_class_id = match->fields.bsn_l3_dst_class_id;
    masks->l3_dst_class_id = match->masks.bsn_l3_dst_class_id;
    fields->global_vrf_allowed = match->fields.bsn_global_vrf_allowed & 1;
    masks->global_vrf_allowed = match->masks.bsn_global_vrf_allowed & 1;
    fields->pad = 0;
    masks->pad = 0;

    /* normalize the flow entry */
    int i;
    char *f = (char *)fields;
    char *m = (char *)masks;
    for (i = 0; i < sizeof (struct ind_ovs_cfr); i++) {
        f[i] &= m[i];
    }
}
