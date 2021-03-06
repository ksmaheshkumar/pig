/*
 *                                Copyright (C) 2015 by Rafael Santiago
 *
 * This is a free software. You can redistribute it and/or modify under
 * the terms of the GNU General Public License version 2.
 *
 */
#include "pigsty.h"
#include "memory.h"
#include "lists.h"
#include "to_int.h"
#include "to_voidp.h"
#include "to_str.h"
#include <stdio.h>
#include <string.h>

#define is_pigsty_blank(c) ( (c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r' )

#define is_pigsty_comment(c) ( (c) == '#' )

static char *get_pigsty_file_data(const char *filepath);

static int compile_next_buffered_pigsty_entry(char *buffer, char **next);

static int compile_pigsty_buffer(const char *data);

static char *skip_pigsty_comment(char *buffer);

static char *skip_pigsty_blank(char *buffer);

static char *get_next_pigsty_word(char *buffer, char **next);

struct signature_fields {
    const char *label;
    const pig_field_t index;
    int (*verifier)(const char *data);
};

static int verify_ip_version(const char *buffer);

static int verify_string(const char *buffer);

static int verify_u1(const char *buffer);

static int verify_u3(const char *buffer);

static int verify_u4(const char *buffer);

static int verify_u6(const char *buffer);

static int verify_u8(const char *buffer);

static int verify_u13(const char *buffer);

static int verify_u16(const char *buffer);

static int verify_u32(const char *buffer);

static int get_pigsty_field_index(const char *field);

static int verify_int(const char *buffer);

static int verify_hex(const char *buffer);

static pigsty_entry_ctx *mk_pigsty_entry_from_compiled_buffer(pigsty_entry_ctx *entries, char *buffer, char **next);

static pigsty_entry_ctx *make_pigsty_data_from_loaded_data(pigsty_entry_ctx *entry, const char *data);

static int verify_required_fields(pigsty_entry_ctx *entry);

static int verify_required_fields_ipv4(pigsty_conf_set_ctx *ip4_set);

static int verify_required_fields_ipv6(pigsty_conf_set_ctx *ip6_set);

static int verify_required_fields_tcp(pigsty_conf_set_ctx *tcp_set);

static int verify_required_fields_udp(pigsty_conf_set_ctx *udp_set);

static int verify_required_datagram_fields(pigsty_conf_set_ctx *set, const int *fields, const size_t fields_size);

static struct signature_fields SIGNATURE_FIELDS[] = {
    {   "ip.version",  kIpv4_version, verify_ip_version},
    {       "ip.ihl",      kIpv4_ihl,         verify_u4},
    {       "ip.tos",      kIpv4_tos,         verify_u8},
    {      "ip.tlen",     kIpv4_tlen,        verify_u16},
    {        "ip.id",       kIpv4_id,        verify_u16},
    {     "ip.flags",    kIpv4_flags,         verify_u3},
    {    "ip.offset",   kIpv4_offset,        verify_u13},
    {       "ip.ttl",      kIpv4_ttl,         verify_u8},
    {  "ip.protocol", kIpv4_protocol,         verify_u8},
    {  "ip.checksum", kIpv4_checksum,        verify_u16},
    {       "ip.src",      kIpv4_src,  verify_ipv4_addr},
    {       "ip.dst",      kIpv4_dst,  verify_ipv4_addr},
    {   "ip.payload",  kIpv4_payload,     verify_string},
    {      "tcp.src",       kTcp_src,        verify_u16},
    {      "tcp.dst",       kTcp_dst,        verify_u16},
    {    "tcp.seqno",       kTcp_seq,        verify_u32},
    {    "tcp.ackno",     kTcp_ackno,        verify_u32},
    {     "tcp.size",      kTcp_size,         verify_u4},
    {   "tcp.reserv",    kTcp_reserv,         verify_u6},
    {      "tcp.urg",       kTcp_urg,         verify_u1},
    {      "tcp.ack",       kTcp_ack,         verify_u1},
    {      "tcp.psh",       kTcp_psh,         verify_u1},
    {      "tcp.rst",       kTcp_rst,         verify_u1},
    {      "tcp.syn",       kTcp_syn,         verify_u1},
    {      "tcp.fin",       kTcp_fin,         verify_u1},
    {    "tcp.wsize",     kTcp_wsize,        verify_u16},
    { "tcp.checksum",  kTcp_checksum,        verify_u16},
    {     "tcp.urgp",      kTcp_urgp,        verify_u16},
    {  "tcp.payload",   kTcp_payload,     verify_string},
    {      "udp.src",       kUdp_src,        verify_u16},
    {      "udp.dst",       kUdp_dst,        verify_u16},
    {     "udp.size",      kUdp_size,        verify_u16},
    { "udp.checksum",  kUdp_checksum,        verify_u16},
    {  "udp.payload",   kUdp_payload,     verify_string},
    {    "icmp.type",     kIcmp_type,         verify_u8},
    {    "icmp.code",     kIcmp_code,         verify_u8},
    {"icmp.checksum", kIcmp_checksum,        verify_u16},
    { "icmp.payload",  kIcmp_payload,     verify_string},
    {    "signature",     kSignature,     verify_string}
};

static const size_t SIGNATURE_FIELDS_SIZE = sizeof(SIGNATURE_FIELDS) / sizeof(SIGNATURE_FIELDS[0]);

pigsty_entry_ctx *load_pigsty_data_from_file(pigsty_entry_ctx *entry, const char *filepath) {
    char *data = get_pigsty_file_data(filepath);
    if (data != NULL) {
        if (!compile_pigsty_buffer(data)) {
            printf("pig PANIC: invalid signature detected, fix it and try again.\n");
            del_pigsty_entry(entry);
            free(data);
            return NULL;
        }
        entry = make_pigsty_data_from_loaded_data(entry, data);
        free(data);
    } else {
        printf("pig PANIC: some i/o error happened.\n");
        del_pigsty_entry(entry);
        return NULL;
    }
    if (!verify_required_fields(entry)) {
        del_pigsty_entry(entry);
        entry = NULL;
    }
    return entry;
}

static pigsty_entry_ctx *make_pigsty_data_from_loaded_data(pigsty_entry_ctx *entry, const char *buffer) {
    char *data = (char *) buffer;
    char *next_data = NULL;
    entry = mk_pigsty_entry_from_compiled_buffer(entry, data, &next_data);
    while (*next_data != 0 && entry != NULL) {
        data = next_data;
        entry = mk_pigsty_entry_from_compiled_buffer(entry, data, &next_data);
    }
    return entry;
}

static char *get_pigsty_file_data(const char *filepath) {
    char *retval = NULL;
    FILE *fp = fopen(filepath, "rb");
    long file_size = 0;
    if (fp == NULL) {
        printf("pig i/o PANIC: unable to open file \"%s\".\n", filepath);
        return NULL;
    }
    if (fseek(fp, 0L, SEEK_END) != -1) {
        file_size = ftell(fp);
        fseek(fp, 0L, SEEK_SET);
    } else {
        printf("pig i/o PANIC: unable to get some file informations from \"%s\".\n", filepath);
        fclose(fp);
        return NULL;
    }
    retval = (char *) pig_newseg(file_size + 1);
    memset(retval, 0, file_size + 1);
    if (fread(retval, 1, file_size, fp) == -1) {
        printf("pig i/o PANIC: unable to load data from file \"%s\".\n", filepath);
        free(retval);
        retval = NULL;
    }
    fclose(fp);
    return retval;
}

static char *skip_pigsty_comment(char *buffer) {
    char *b = buffer;
    while (*b != '\n' && *b != 0) {
        b++;
    }
    return b;
}

static char *skip_pigsty_blank(char *buffer) {
    char *b = buffer;
    while (is_pigsty_blank(*b)) {
        b++;
        if (is_pigsty_comment(*b)) b = skip_pigsty_comment(b);
    }
    return b;
}

static char *get_next_pigsty_word(char *buffer, char **next) {
    char *bp = buffer;
    char *end_bp = NULL;
    char *retval = NULL;
    bp = end_bp = skip_pigsty_blank(bp);
    if (*bp != '=' && *bp != ','  && *bp != '[') {
        while (!is_pigsty_blank(*end_bp) && *end_bp != 0) {
            if (*end_bp == '\"') {
                end_bp++;
                while (*end_bp != '\"' && *end_bp != 0) {
                    end_bp++;
                    if (*end_bp == '\\') {
                        end_bp += 2;
                    }
                }
                end_bp++;
                break;
            } else {
                end_bp++;
                if (*end_bp == '=' || *end_bp == ',' || *end_bp == ']') {
                    break;
                }
            }
        }
    } else {
        end_bp = bp + 1;
    }
    *next = end_bp;
    retval = (char *) pig_newseg(end_bp - bp + 1);
    memset(retval, 0, end_bp - bp + 1);
    memcpy(retval, bp, end_bp - bp);
    return retval;
}

static pigsty_entry_ctx *mk_pigsty_entry_from_compiled_buffer(pigsty_entry_ctx *entries, char *buffer, char **next) {
    char *token = NULL;
    char *tmp_buffer = buffer;
    char *signature_name = NULL;
    char *data = NULL;
    void *fmt_data = NULL;
    size_t fmt_dsize = 0;
    pigsty_entry_ctx *entry_p = NULL;
    int field_index = 0;
    size_t sz = 0;
    token = get_next_pigsty_word(tmp_buffer, next);
    while (**next != 0 && signature_name == NULL) {
        if (strcmp(token, "signature") == 0) {
            tmp_buffer = *next;
            signature_name = get_next_pigsty_word(tmp_buffer, next); //  =
            free(signature_name);
            tmp_buffer = *next;
            token = get_next_pigsty_word(tmp_buffer, next);
            signature_name = to_str(token, &sz);
            if (get_pigsty_entry_signature_name(signature_name, entries) != NULL) {
                printf("pig PANIC: packet signature \"%s\" redeclared.\n", signature_name);
                free(signature_name);
                free(token);
                del_pigsty_entry(entries);
                return NULL;
            }
        }
        tmp_buffer = *next;
        free(token);
        if (signature_name == NULL) {
            token = get_next_pigsty_word(tmp_buffer, next);
        }
    }
    if (signature_name != NULL) {
        entries = add_signature_to_pigsty_entry(entries, signature_name);
        free(signature_name);
        entry_p = get_pigsty_entry_tail(entries);
        tmp_buffer = buffer;
        token = get_next_pigsty_word(tmp_buffer, next);
        while (**next != 0) {
            if ((field_index = get_pigsty_field_index(token)) > -1 && field_index != kSignature) {
                tmp_buffer = *next;
                free(token);
                token = get_next_pigsty_word(tmp_buffer, next); //  =
                free(token);
                token = NULL;
                tmp_buffer = *next;
                data = get_next_pigsty_word(tmp_buffer, next);
                if (data != NULL) {
                    if (verify_int(data) || verify_hex(data)) {
                        fmt_data = int_to_voidp(data, &fmt_dsize);
                    } else if (verify_ipv4_addr(data)) {
                        fmt_data = ipv4_to_voidp(data, &fmt_dsize);
                    } else if (verify_string(data)) {
                        fmt_data = str_to_voidp(data, &fmt_dsize);
                    }
                    entry_p->conf = add_conf_to_pigsty_conf_set(entry_p->conf, field_index, fmt_data, fmt_dsize);
                    free(fmt_data);
                    fmt_data = NULL;
                }
                free(data);
                data = NULL;
            }
            tmp_buffer = *next;
            free(token);
            token = get_next_pigsty_word(tmp_buffer, next);
            if (*token == ']') {
                break;
            }
        }
        free(token);
    } else {
        printf("pig PANIC: signature field missing.\n");
    }
    return entries;
}

static int compile_next_buffered_pigsty_entry(char *buffer, char **next) {
    int all_ok = 1;
    char *token = get_next_pigsty_word(buffer, next);
    int state = 0;
    unsigned char field_map[SIGNATURE_FIELDS_SIZE];
    int field_index = 0;
    memset(field_map, 0, sizeof(field_map));
    if (*token == 0) {
        return 1;
    }
    if (*token != '[') {
        printf("pig PANIC: signature not well opened.\n");
        free(token);
        return 0;
    }
    free(token);
    buffer = *next;
    token = get_next_pigsty_word(buffer, next);
    while (all_ok && **next != 0 && token != NULL) {
        switch (state) {
            case 0:  //  field existence verifying
                field_index = get_pigsty_field_index(token);
                if (field_index == -1) {
                    printf("pig PANIC: unknown field \"%s\".\n", token);
                    return 0;
                }
                if (field_map[field_index] == 1) {
                    free(token);
                    printf("pig PANIC: field \"%s\" redeclared.\n", SIGNATURE_FIELDS[field_index].label);
                    return 0;
                }
                field_map[field_index] = 1;
                state = 1;
                break;

            case 1:
                all_ok = (strcmp(token, "=") == 0);
                if (!all_ok) {
                    printf("pig PANIC: expecting \"=\" token.\n");
                    free(token);
                    return 0;
                }
                state = 2;
                break;

            case 2:  //  field data verifying
                if (SIGNATURE_FIELDS[field_index].verifier != NULL) {
                    all_ok = SIGNATURE_FIELDS[field_index].verifier(token);
                    if (!all_ok) {
                        printf("pig PANIC: field \"%s\" has invalid data (\"%s\").\n", SIGNATURE_FIELDS[field_index].label, token);
                        free(token);
                        return 0;
                    }
                }
                state = 3;
                break;

            case 3:  //  next or end verifying
                all_ok = (*token == ',' || *token == ']');
                state = 0;
                if (!all_ok) {
                    printf("pig PANIC: missing \",\" or \"]\".\n");
                    all_ok = 0;
                }
                break;
        }
        if (*token != ']') {
            free(token);
            buffer = *next;
            token = get_next_pigsty_word(buffer, next);
        } else {
            free(token);
            token = NULL;
        }
    }
    return all_ok;
}

static int compile_pigsty_buffer(const char *buffer) {
    int all_ok = 1;
    char *data = (char *) buffer;
    char *next_data = NULL;
    all_ok = compile_next_buffered_pigsty_entry(data, &next_data);
    while (all_ok && *next_data != 0) {
        data = next_data;
        all_ok = compile_next_buffered_pigsty_entry(data, &next_data);
    }
    return all_ok;
}

static int verify_ip_version(const char *buffer) {
    return ((verify_u3(buffer)  || verify_u4(buffer) ||
             verify_u8(buffer)  || verify_u13(buffer) ||
             verify_u16(buffer) || verify_u32(buffer)) && to_int(buffer) == 4);
}

static int verify_string(const char *buffer) {
    return (buffer != NULL && (*buffer == '\"' && buffer[strlen(buffer) - 1] == '\"'));
}

static int verify_u1(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval == 0x0 || retval == 0x1);
}

static int verify_u3(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0x7);
}

static int verify_u4(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0xf);
}

static int verify_u6(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0x3f);
}

static int verify_u8(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0xff);
}

static int verify_u13(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0x1fff);
}

static int verify_u16(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0xffff);
}

static int verify_u32(const char *buffer) {
    int retval = -1;
    if (verify_hex(buffer)) {
        retval = strtoul(buffer + 2, NULL, 16);
    } else if (verify_int(buffer)) {
        retval = atoi(buffer);
    }
    return (retval >= 0x0 && retval <= 0xffffffff);
}

int verify_ipv4_addr(const char *buffer) {
    int retval = 1;
    const char *b = buffer;
    int dots_nr = 0;
    char oct[255];
    size_t o = 0;
    if (strcmp(buffer, "north-american-ip") == 0 ||
        strcmp(buffer, "south-american-ip") == 0 ||
        strcmp(buffer, "asian-ip") == 0          ||
        strcmp(buffer, "european-ip") == 0       ||
        strcmp(buffer, "user-defined-ip") == 0) {
        return 1;
    }
    memset(oct, 0, sizeof(oct));
    for (b = buffer; *b != 0 && retval; b++) {
        if (*b != '.' && !isdigit(*b)) {
            return 0;
        }
        if (*b == '.' || *(b + 1) == 0) {
            if (*(b + 1) == 0) {
                oct[o] = *b;
            }
            if (*b == '.') {
                dots_nr++;
            }
            retval = (atoi(oct) >= 0 && atoi(oct) <= 255);
            o = -1;
            memset(oct, 0, sizeof(oct));
        } else {
            oct[o] = *b;
        }
        o = (o + 1) % sizeof(oct);
    }
    return (retval && dots_nr == 3);
}

static int get_pigsty_field_index(const char *field) {
    size_t f;
    for (f = 0; f < SIGNATURE_FIELDS_SIZE; f++) {
        if (strcmp(SIGNATURE_FIELDS[f].label, field) == 0) {
            return SIGNATURE_FIELDS[f].index;
        }
    }
    return -1;
}

static int verify_int(const char *buffer) {
    const char *b;
    if (buffer == NULL) {
        return 0;
    }
    for (b = buffer; *b != 0; b++) {
        if (!isdigit(*b)) {
            return 0;
        }
    }
    return 1;
}

static int verify_hex(const char *buffer) {
    const char *b = buffer;
    if (b == NULL) {
        return 0;
    }
    if (*b != '0') {
        return 0;
    }
    b++;
    if (*b != 'x') {
        return 0;
    }
    b++;
    if (*b == 0) {
        return 0;
    }
    for (; *b != 0; b++) {
        if (!isxdigit(*b)) {
            return 0;
        }
    }
    return 1;
}

static int verify_required_datagram_fields(pigsty_conf_set_ctx *set, const int *fields, const size_t fields_size) {
    size_t f;
    int retval = 1;
    int present_fields[SIGNATURE_FIELDS_SIZE];
    pigsty_conf_set_ctx *sp;
    memset(present_fields, 0, sizeof(present_fields));
    for (sp = set; sp != NULL; sp = sp->next) {
        present_fields[sp->field->index] = 1;
    }
    for (f = 0; f < fields_size && retval == 1; f++) {
        retval = (present_fields[fields[f]] == 1);
        if (retval == 0) {
            printf("pig error: field \"%s\" is required.\n", SIGNATURE_FIELDS[fields[f]].label);
        }
    }
    return retval;
}

static int verify_required_fields_ipv4(pigsty_conf_set_ctx *ip4_set) {
    int ip4_required_fields[] = { kIpv4_src, kIpv4_dst, kIpv4_protocol };
    return verify_required_datagram_fields(ip4_set, ip4_required_fields, sizeof(ip4_required_fields) / sizeof(ip4_required_fields[0]));
}

static int verify_required_fields_ipv6(pigsty_conf_set_ctx *ip6_set) {
    return 0;
}

static int verify_required_fields_tcp(pigsty_conf_set_ctx *tcp_set) {
    int tcp_required_fields[] = { kTcp_src, kTcp_dst };
    return verify_required_datagram_fields(tcp_set, tcp_required_fields, sizeof(tcp_required_fields) / sizeof(tcp_required_fields[0]));
}

static int verify_required_fields_udp(pigsty_conf_set_ctx *udp_set) {
    int udp_required_fields[] = { kUdp_src, kUdp_dst };
    return verify_required_datagram_fields(udp_set, udp_required_fields, sizeof(udp_required_fields) / sizeof(udp_required_fields[0]));
}

static int verify_required_fields(pigsty_entry_ctx *entry) {
    pigsty_entry_ctx *ep;
    pigsty_conf_set_ctx *cp;
    int retval = 1;
    int ip_version = 0;
    int transport_layer = 0;
    for (ep = entry; ep != NULL && retval == 1; ep = ep->next) {
        //  INFO(Santiago): verifying the IP mandatory fields.
        ip_version = 0;
        for (cp = ep->conf; cp != NULL && ip_version == 0; cp = cp->next) {
            if (cp->field->index == kIpv4_version && cp->field->data != NULL) {
                ip_version = *(int *)cp->field->data;
            }
        }
        if (ip_version == 0) {
            printf("pig PANIC: signature %s: ip.version missing.\n", ep->signature_name);
            retval = 0;
        }
        if (retval == 1) {
            switch(ip_version) {
                case 4:
                    retval = verify_required_fields_ipv4(ep->conf);
                    break;

                //case 6:
                //    retval = verify_required_fields_ipv6(cp);
                //    break;

                default:
                    retval = 0; //  INFO(Santiago): in practice it should not happen.
                    break;
            }
        }
        if (retval == 0) {
            printf("pig PANIC: on signature \"%s\".\n", ep->signature_name);
            continue;
        }
        //  INFO(Santiago): verifying the transport layer mandatory fields.
        transport_layer = -1;
        for (cp = ep->conf; cp != NULL && transport_layer == -1; cp = cp->next) {
            if (cp->field->index == kIpv4_protocol && cp->field->data != NULL) {
                transport_layer = *(int *)cp->field->data;
            }
        }
        if (transport_layer > -1) {
            switch (transport_layer) {
                default:
                    retval = 1; //  INFO(Santiago): just skipping.
                    break;
            }
            if (retval == 0) {
                printf("pig PANIC: signature %s: required field missing.\n", ep->signature_name);
            }
        } else {
            for (cp = ep->conf; cp != NULL && retval == 1; cp = cp->next) {
                retval = !(cp->field->index == kTcp_src       ||
                           cp->field->index == kTcp_dst       ||
                           cp->field->index == kTcp_seq       ||
                           cp->field->index == kTcp_ackno     ||
                           cp->field->index == kTcp_size      ||
                           cp->field->index == kTcp_reserv    ||
                           cp->field->index == kTcp_urg       ||
                           cp->field->index == kTcp_ack       ||
                           cp->field->index == kTcp_psh       ||
                           cp->field->index == kTcp_rst       ||
                           cp->field->index == kTcp_syn       ||
                           cp->field->index == kTcp_fin       ||
                           cp->field->index == kTcp_wsize     ||
                           cp->field->index == kTcp_checksum  ||
                           cp->field->index == kTcp_urgp      ||
                           cp->field->index == kTcp_payload   ||
                           cp->field->index == kUdp_src       ||
                           cp->field->index == kUdp_dst       ||
                           cp->field->index == kUdp_size      ||
                           cp->field->index == kUdp_checksum  ||
                           cp->field->index == kUdp_payload   ||
                           cp->field->index == kUdp_src       ||
                           cp->field->index == kIcmp_type     ||
                           cp->field->index == kIcmp_code     ||
                           cp->field->index == kIcmp_checksum ||
                           cp->field->index == kIcmp_payload);
            }
            if (retval == 0) {
                printf("pig PANIC: signature %s: tcp/udp/icmp fields informed in a non tcp, udp or icmp packet.\n", ep->signature_name);
            }
        }
    }
    return retval;
}
