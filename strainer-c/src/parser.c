#include "parser.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>


static bool validate_ip_str(const char *ip_start, const char *ip_end) {
    int octet  = 0;
    int digits = 0;
    int dots   = 0;

    for (const char *p = ip_start; p < ip_end; p++) {
        if (*p == '.') {
            if (digits == 0 || octet > 255) return false;
            dots++;
            octet  = 0;
            digits = 0;
        } else if (isdigit((unsigned char)*p)) {
            octet  = octet * 10 + (*p - '0');
            digits++;
            if (digits > 3 || octet > 255) return false;
        } else {
            return false;
        }
    }

    if (digits == 0 || octet > 255) return false;

    return (dots == 3);
}

static bool validate_port_str(const char *port_start, const char *port_end) {
    if (port_start >= port_end) return false;

    long port = 0;
    for (const char *p = port_start; p < port_end; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        port = port * 10 + (*p - '0');
        if (port > 65535) return false;
    }

    return (port >= 1);
}

static bool validate_hostport(const char *ptr, const char *end) {
    const char *at = NULL;
    for (const char *p = ptr; p < end; p++) {
        if (*p == '@') { at = p; break; }
    }
    if (at) ptr = at + 1;

    const char *colon = NULL;
    for (const char *p = ptr; p < end; p++) {
        if (*p == ':') colon = p;
    }
    if (!colon || colon == ptr || colon + 1 == end) return false;

    return validate_ip_str(ptr, colon) && validate_port_str(colon + 1, end);
}

bool is_valid_proxy_syntax(const char *proxy) {
    if (!proxy || *proxy == '\0') return false;

    const char *end = proxy + strlen(proxy);

    const char *scheme_sep = strstr(proxy, "://");
    if (scheme_sep) {
        size_t scheme_len = (size_t)(scheme_sep - proxy);

        char scheme[16] = {0};
        if (scheme_len == 0 || scheme_len >= sizeof(scheme)) return false;
        memcpy(scheme, proxy, scheme_len);

        bool scheme_ok = (strcmp(scheme, "http")   == 0 ||
                          strcmp(scheme, "https")  == 0 ||
                          strcmp(scheme, "socks4") == 0 ||
                          strcmp(scheme, "socks5") == 0);
        if (!scheme_ok) return false;

        const char *host_start = scheme_sep + 3;
        return validate_hostport(host_start, end);
    }

    return validate_hostport(proxy, end);
}
