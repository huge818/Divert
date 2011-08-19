/*
 * netfilter.c
 * (C) 2011, all rights reserved,
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * DESCRIPTION:
 * This is a simple traffic filter
 *
 * usage: netfilter.exe divert-filter
 *
 * Any traffic that matches the divert-filter will be blocked using one of
 * the following methods:
 * - TCP: send a TCP RST to the packet's source.
 * - UDP: send a ICMP(v6) "destination unreachable" to the packet's source.
 * - ICMP/ICMPv6: Drop the packet.
 *
 * This program is similar to Linux's iptables with the "-j REJECT" target.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "divert.h"

#define MAXBUF  2048

/*
 * Pre-fabricated packets.
 */
typedef struct
{
    DIVERT_PACKET divert;
    DIVERT_IPHDR  ip;
} PACKET, *PPACKET;

typedef struct
{
    DIVERT_PACKET  divert;
    DIVERT_IPV6HDR ipv6;
} PACKETV6, *PPACKETV6;

typedef struct
{
    PACKET header;
    DIVERT_TCPHDR tcp;
} TCPPACKET, *PTCPPACKET;

typedef struct
{
    PACKETV6 header;
    DIVERT_TCPHDR tcp;
} TCPV6PACKET, *PTCPV6PACKET;

typedef struct
{
    PACKET header;
    DIVERT_ICMPHDR icmp;
    UINT8 data[];
} ICMPPACKET, *PICMPPACKET;

typedef struct
{
    PACKETV6 header;
    DIVERT_ICMPV6HDR icmpv6;
    UINT8 data[];
} ICMPV6PACKET, *PICMPV6PACKET;

/*
 * Prototypes.
 */
static void PacketIpInit(PPACKET packet);
static void PacketIpTcpInit(PTCPPACKET packet);
static void PacketIpIcmpInit(PICMPPACKET packet);
static void PacketIpv6Init(PPACKETV6 packet);
static void PacketIpv6TcpInit(PTCPV6PACKET packet);
static void PacketIpv6Icmpv6Init(PICMPV6PACKET packet);

/*
 * Entry.
 */
int main(int argc, char **argv)
{
    HANDLE handle, console;
    size_t slen, flen;
    UINT i;
    char filter[MAXBUF];
    char packet[MAXBUF];
    PDIVERT_PACKET ppacket = (PDIVERT_PACKET)packet;
    UINT ppacket_len;
    PDIVERT_IPHDR ip_header;
    PDIVERT_IPV6HDR ipv6_header;
    PDIVERT_ICMPHDR icmp_header;
    PDIVERT_ICMPV6HDR icmpv6_header;
    PDIVERT_TCPHDR tcp_header;
    PDIVERT_UDPHDR udp_header;
    UINT payload_len;
    
    TCPPACKET reset0;
    PTCPPACKET reset = &reset0;
    UINT8 dnr0[sizeof(ICMPPACKET) + 0x0F*sizeof(UINT32) + 8 + 1];
    PICMPPACKET dnr = (PICMPPACKET)dnr0;

    TCPV6PACKET resetv6_0;
    PTCPV6PACKET resetv6 = &resetv6_0;
    UINT8 dnrv6_0[sizeof(ICMPV6PACKET) + sizeof(DIVERT_IPV6HDR) +
        sizeof(DIVERT_TCPHDR)];
    PICMPV6PACKET dnrv6 = (PICMPV6PACKET)dnrv6_0;

    // Concat all command line args into a filter string.
    flen = 0;
    for (i = 1; (int)i < argc; i++)
    {
        slen = strlen(argv[i]);
        if (flen + slen + 1 >= MAXBUF)
        {
            fprintf(stderr, "error: filter too long\n");
            exit(EXIT_FAILURE);
        }
        strcpy(filter+flen, argv[i]);
        flen += slen;
        filter[flen] = ' ';
        flen++;
    }
    filter[flen] = '\0';

    // Initialize all packets.
    PacketIpTcpInit(reset);
    reset->tcp.Rst = 1;
    reset->tcp.Ack = 1;
    PacketIpIcmpInit(dnr);
    dnr->icmp.Type = 3;         // Destination not reachable.
    dnr->icmp.Code = 3;         // Port not reachable.
    PacketIpv6TcpInit(resetv6);
    resetv6->tcp.Rst = 1;
    resetv6->tcp.Ack = 1;
    PacketIpv6Icmpv6Init(dnrv6);
    dnrv6->header.ipv6.Length = htons(sizeof(DIVERT_ICMPV6HDR) + 4 +
        sizeof(DIVERT_IPV6HDR) + sizeof(DIVERT_TCPHDR));
    dnrv6->icmpv6.Type = 1;     // Destination not reachable.
    dnrv6->icmpv6.Code = 4;     // Port not reachable.

    // Get console for pretty colors.
    console = GetStdHandle(STD_OUTPUT_HANDLE);

    // Divert traffic matching the filter:
    handle = DivertOpen(filter);
    if (handle == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
        {
            fprintf(stderr, "error: filter syntax error\n");
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "error: failed to open Divert device (%d)\n",
            GetLastError());
        exit(EXIT_FAILURE);
    }

    // Main loop:
    while (TRUE)
    {
        // Read a matching packet.
        if (!DivertRecv(handle, ppacket, sizeof(packet), &ppacket_len))
        {
            fprintf(stderr, "warning: failed to read packet\n");
            continue;
        }
       
        // Print info about the matching packet.
        DivertHelperParse(ppacket, ppacket_len, &ip_header, &ipv6_header,
            &icmp_header, &icmpv6_header, &tcp_header, &udp_header, NULL,
            &payload_len);
        if (ip_header == NULL && ipv6_header == NULL)
        {
            continue;
        }

        // Dump packet info: 
        SetConsoleTextAttribute(console, FOREGROUND_RED);
        fputs("BLOCK ", stdout);
        SetConsoleTextAttribute(console,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        if (ip_header != NULL)
        {
            UINT8 *src_addr = (UINT8 *)&ip_header->SrcAddr;
            UINT8 *dst_addr = (UINT8 *)&ip_header->DstAddr;
            printf("ip.SrcAddr=%u.%u.%u.%u ip.DstAddr=%u.%u.%u.%u ",
                src_addr[0], src_addr[1], src_addr[2], src_addr[3],
                dst_addr[0], dst_addr[1], dst_addr[2], dst_addr[3]);
        }
        if (ipv6_header != NULL)
        {
            UINT16 *src_addr = (UINT16 *)&ipv6_header->SrcAddr;
            UINT16 *dst_addr = (UINT16 *)&ipv6_header->DstAddr;
            fputs("ipv6.SrcAddr=", stdout);
            for (i = 0; i < 8; i++)
            {
                printf("%x%c", ntohs(src_addr[i]), (i == 7? ' ': ':'));
            } 
            fputs(" ipv6.DstAddr=", stdout);
            for (i = 0; i < 8; i++)
            {
                printf("%x%c", ntohs(dst_addr[i]), (i == 7? ' ': ':'));
            }
            putchar(' ');
        }
        if (icmp_header != NULL)
        {
            printf("icmp.Type=%u icmp.Code=%u ",
                icmp_header->Type, icmp_header->Code);
            // Simply drop ICMP
        }
        if (icmpv6_header != NULL)
        {
            printf("icmpv6.Type=%u icmpv6.Code=%u ",
                icmpv6_header->Type, icmpv6_header->Code);
            // Simply drop ICMPv6
        }
        if (tcp_header != NULL)
        {
            printf("tcp.SrcPort=%u tcp.DstPort=%u tcp.Flags=",
                ntohs(tcp_header->SrcPort), ntohs(tcp_header->DstPort));
            if (tcp_header->Fin)
            {
                fputs("[FIN]", stdout);
            }
            if (tcp_header->Rst)
            {
                fputs("[RST]", stdout);
            }
            if (tcp_header->Urg)
            {
                fputs("[URG]", stdout);
            }
            if (tcp_header->Syn)
            {
                fputs("[SYN]", stdout);
            }
            if (tcp_header->Psh)
            {
                fputs("[PSH]", stdout);
            }
            if (tcp_header->Ack)
            {
                fputs("[ACK]", stdout);
            }
            putchar(' ');


            if (ip_header != NULL)
            {
                reset->header.divert.IfIdx = ppacket->IfIdx;
                reset->header.divert.SubIfIdx = ppacket->SubIfIdx;
                reset->header.divert.Direction = !ppacket->Direction;
                reset->header.ip.SrcAddr = ip_header->DstAddr;
                reset->header.ip.DstAddr = ip_header->SrcAddr;
                reset->tcp.SrcPort = tcp_header->DstPort;
                reset->tcp.DstPort = tcp_header->SrcPort;
                reset->tcp.SeqNum = 
                    (tcp_header->Ack? tcp_header->AckNum: 0);
                reset->tcp.AckNum =
                    (tcp_header->Syn?
                        htonl(ntohl(tcp_header->SeqNum) + 1):
                        htonl(ntohl(tcp_header->SeqNum) + payload_len));

                DivertHelperCalcChecksums((PDIVERT_PACKET)reset,
                    sizeof(TCPPACKET), 0);
                if (!DivertSend(handle, (PDIVERT_PACKET)reset,
                    sizeof(TCPPACKET), NULL))
                {
                    fprintf(stderr, "warning: failed to send TCP reset (%d)\n",
                        GetLastError());
                }
            }

            if (ipv6_header != NULL)
            {
                resetv6->header.divert.IfIdx = ppacket->IfIdx;
                resetv6->header.divert.SubIfIdx = ppacket->SubIfIdx;
                resetv6->header.divert.Direction = !ppacket->Direction;
                memcpy(resetv6->header.ipv6.SrcAddr, ipv6_header->DstAddr,
                    sizeof(resetv6->header.ipv6.SrcAddr));
                memcpy(resetv6->header.ipv6.DstAddr, ipv6_header->SrcAddr,
                    sizeof(resetv6->header.ipv6.DstAddr));
                resetv6->tcp.SrcPort = tcp_header->DstPort;
                resetv6->tcp.DstPort = tcp_header->SrcPort;
                resetv6->tcp.SeqNum =
                    (tcp_header->Ack? tcp_header->AckNum: 0);
                resetv6->tcp.AckNum =
                    (tcp_header->Syn?
                        htonl(ntohl(tcp_header->SeqNum) + 1):
                        htonl(ntohl(tcp_header->SeqNum) + payload_len));

                DivertHelperCalcChecksums((PDIVERT_PACKET)resetv6,
                    sizeof(TCPV6PACKET), 0);
                if (!DivertSend(handle, (PDIVERT_PACKET)resetv6,
                    sizeof(TCPV6PACKET), NULL))
                {
                    fprintf(stderr, "warning: failed to send TCP (IPV6) "
                        "reset (%d)\n", GetLastError());
                }
            }
        }
        if (udp_header != NULL)
        {
            printf("udp.SrcPort=%u udp.DstPort=%u ",
                ntohs(udp_header->SrcPort), ntohs(udp_header->DstPort));
        
            if (ip_header != NULL)
            {
                // NOTE: For some ICMP error messages, WFP does not seem to
                //       support INBOUND injection.  As a work-around, we
                //       always inject OUTBOUND.
                UINT icmp_length = ip_header->HdrLength*sizeof(UINT32) + 8;
                memcpy(dnr->data, ip_header, icmp_length);
                icmp_length += sizeof(ICMPPACKET);
                dnr->header.divert.IfIdx = ppacket->IfIdx;
                dnr->header.divert.SubIfIdx = ppacket->SubIfIdx;
                dnr->header.divert.Direction =
                    DIVERT_PACKET_DIRECTION_OUTBOUND;
                dnr->header.ip.Length =
                    htons(icmp_length - sizeof(DIVERT_PACKET));
                dnr->header.ip.SrcAddr = ip_header->DstAddr;
                dnr->header.ip.DstAddr = ip_header->SrcAddr;
                DivertHelperCalcChecksums((PDIVERT_PACKET)dnr, icmp_length, 0);
                if (!DivertSend(handle, (PDIVERT_PACKET)dnr, icmp_length,
                    NULL))
                {
                    fprintf(stderr, "warning: failed to send ICMP message "
                        "(%d)\n", GetLastError());
                }
            }
        
            if (ipv6_header != NULL)
            {
                UINT icmpv6_length = sizeof(DIVERT_IPV6HDR) +
                    sizeof(DIVERT_TCPHDR);
                memcpy(dnrv6->data, ipv6_header, icmpv6_length);
                icmpv6_length += sizeof(ICMPV6PACKET);
                dnrv6->header.divert.IfIdx = ppacket->IfIdx;
                dnrv6->header.divert.SubIfIdx = ppacket->SubIfIdx;
                dnrv6->header.divert.Direction = 
                    DIVERT_PACKET_DIRECTION_OUTBOUND;
                memcpy(dnrv6->header.ipv6.SrcAddr, ipv6_header->DstAddr,
                    sizeof(dnrv6->header.ipv6.SrcAddr));
                memcpy(dnrv6->header.ipv6.DstAddr, ipv6_header->SrcAddr,
                    sizeof(dnrv6->header.ipv6.DstAddr));
                DivertHelperCalcChecksums((PDIVERT_PACKET)dnrv6, icmpv6_length,
                    0);
                if (!DivertSend(handle, (PDIVERT_PACKET)dnrv6, icmpv6_length,
                    NULL))
                {
                    fprintf(stderr, "warning: failed to send ICMPv6 message "
                        "(%d)\n", GetLastError());
                }
            }
        }
        putchar('\n');
    }
}

/*
 * Initialize a PACKET.
 */
static void PacketIpInit(PPACKET packet)
{
    memset(packet, 0, sizeof(PACKET));
    packet->ip.Version = 4;
    packet->ip.HdrLength = sizeof(DIVERT_IPHDR) / sizeof(UINT32);
    packet->ip.Id = ntohs(0xDEAD);
    packet->ip.TTL = 64;
}

/*
 * Initialize a TCPPACKET.
 */
static void PacketIpTcpInit(PTCPPACKET packet)
{
    memset(packet, 0, sizeof(TCPPACKET));
    PacketIpInit(&packet->header);
    packet->header.ip.Length = htons(sizeof(TCPPACKET) -
        sizeof(DIVERT_PACKET));
    packet->header.ip.Protocol = IPPROTO_TCP;
    packet->tcp.HdrLength = sizeof(DIVERT_TCPHDR) / sizeof(UINT32);
}

/*
 * Initialize an ICMPPACKET.
 */
static void PacketIpIcmpInit(PICMPPACKET packet)
{
    memset(packet, 0, sizeof(ICMPPACKET));
    PacketIpInit(&packet->header);
    packet->header.ip.Protocol = IPPROTO_ICMP;
}

/*
 * Initialize a PACKETV6.
 */
static void PacketIpv6Init(PPACKETV6 packet)
{
    memset(packet, 0, sizeof(PACKETV6));
    packet->ipv6.Version = 6;
    packet->ipv6.HopLimit = 64;
}

/*
 * Initialize a TCPV6PACKET.
 */
static void PacketIpv6TcpInit(PTCPV6PACKET packet)
{
    memset(packet, 0, sizeof(TCPV6PACKET));
    PacketIpv6Init(&packet->header);
    packet->header.ipv6.Length = htons(sizeof(DIVERT_TCPHDR));
    packet->header.ipv6.NextHdr = IPPROTO_TCP;
    packet->tcp.HdrLength = sizeof(DIVERT_TCPHDR) / sizeof(UINT32);
}

/*
 * Initialize an ICMP PACKET.
 */
static void PacketIpv6Icmpv6Init(PICMPV6PACKET packet)
{
    memset(packet, 0, sizeof(ICMPV6PACKET));
    PacketIpv6Init(&packet->header);
    packet->header.ipv6.NextHdr = IPPROTO_ICMPV6;
}

