#include "prov_form.h"

#include <string.h>

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int prov_url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (dst == NULL || dst_size == 0) {
        return -1;
    }
    memset(dst, 0, dst_size);
    if (src == NULL) {
        return -1;
    }

    size_t out = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        char decoded;

        if (c == '+') {
            decoded = ' ';
        } else if (c == '%') {
            /* An escape needs two more characters. A body cut short
             * mid-escape is malformed, not a reason to read past the
             * end. */
            if (i + 2 >= src_len) {
                memset(dst, 0, dst_size);
                return -1;
            }
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) {
                memset(dst, 0, dst_size);
                return -1;
            }
            decoded = (char)((hi << 4) | lo);
            i += 2;
        } else {
            decoded = c;
        }

        /* Leave room for the terminator. */
        if (out + 1 >= dst_size) {
            memset(dst, 0, dst_size);
            return -1;
        }
        dst[out++] = decoded;
    }

    dst[out] = '\0';
    return (int)out;
}

bool prov_form_field(const char *body, const char *name, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    memset(out, 0, out_size);
    if (body == NULL || name == NULL || name[0] == '\0') {
        return false;
    }

    size_t name_len = strlen(name);
    const char *p = body;

    while (*p != '\0') {
        /* A match must start a field and be followed by '=', so "ssid"
         * does not match inside "myssid" or at the start of "ssid2". */
        bool at_field_start = (p == body) || (p[-1] == '&');
        if (at_field_start && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *value = p + name_len + 1;
            const char *end = strchr(value, '&');
            size_t len = (end != NULL) ? (size_t)(end - value) : strlen(value);
            return prov_url_decode(out, out_size, value, len) >= 0;
        }
        const char *next = strchr(p, '&');
        if (next == NULL) {
            break;
        }
        p = next + 1;
    }
    return false;
}

bool prov_ct_str_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = (la > lb) ? la : lb;

    /* The lengths differ visibly in timing whatever we do here, but the
     * content must not: compare every position of the longer string. */
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        char ca = (i < la) ? a[i] : '\0';
        char cb = (i < lb) ? b[i] : '\0';
        diff |= (unsigned)((unsigned char)ca ^ (unsigned char)cb);
    }
    return diff == 0;
}
