/* ANSI-C code produced by gperf version 3.1 */
/* Command-line: gperf --output-file=http_headers.c http_headers.gperf  */
/* Computed positions: -k'5,10-11,22,$' */

#if !((' ' == 32) && ('!' == 33) && ('"' == 34) && ('#' == 35) \
      && ('%' == 37) && ('&' == 38) && ('\'' == 39) && ('(' == 40) \
      && (')' == 41) && ('*' == 42) && ('+' == 43) && (',' == 44) \
      && ('-' == 45) && ('.' == 46) && ('/' == 47) && ('0' == 48) \
      && ('1' == 49) && ('2' == 50) && ('3' == 51) && ('4' == 52) \
      && ('5' == 53) && ('6' == 54) && ('7' == 55) && ('8' == 56) \
      && ('9' == 57) && (':' == 58) && (';' == 59) && ('<' == 60) \
      && ('=' == 61) && ('>' == 62) && ('?' == 63) && ('A' == 65) \
      && ('B' == 66) && ('C' == 67) && ('D' == 68) && ('E' == 69) \
      && ('F' == 70) && ('G' == 71) && ('H' == 72) && ('I' == 73) \
      && ('J' == 74) && ('K' == 75) && ('L' == 76) && ('M' == 77) \
      && ('N' == 78) && ('O' == 79) && ('P' == 80) && ('Q' == 81) \
      && ('R' == 82) && ('S' == 83) && ('T' == 84) && ('U' == 85) \
      && ('V' == 86) && ('W' == 87) && ('X' == 88) && ('Y' == 89) \
      && ('Z' == 90) && ('[' == 91) && ('\\' == 92) && (']' == 93) \
      && ('^' == 94) && ('_' == 95) && ('a' == 97) && ('b' == 98) \
      && ('c' == 99) && ('d' == 100) && ('e' == 101) && ('f' == 102) \
      && ('g' == 103) && ('h' == 104) && ('i' == 105) && ('j' == 106) \
      && ('k' == 107) && ('l' == 108) && ('m' == 109) && ('n' == 110) \
      && ('o' == 111) && ('p' == 112) && ('q' == 113) && ('r' == 114) \
      && ('s' == 115) && ('t' == 116) && ('u' == 117) && ('v' == 118) \
      && ('w' == 119) && ('x' == 120) && ('y' == 121) && ('z' == 122) \
      && ('{' == 123) && ('|' == 124) && ('}' == 125) && ('~' == 126))
/* The character set is not based on ISO-646.  */
#error "gperf generated tables don't work with this execution character set. Please report a bug to <bug-gperf@gnu.org>."
#endif

#line 1 "http_headers.gperf"

/*
 * Adapted for Ark from redbean's HTTP parser, part of Cosmopolitan:
 * https://github.com/jart/cosmopolitan (net/http/)
 *
 * Copyright 2020 Justine Alexandra Roberts Tunney
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Finds the id of a known header name, ignoring case, with a perfect hash.
 * http_headers.c is generated from this file; after changing it, run in
 * src/http:
 *
 *     gperf --output-file=http_headers.c http_headers.gperf
 */

#include <ark/http_parser.h>
#line 40 "http_headers.gperf"
struct HttpHeaderSlot { const char *name; int code; };

#define TOTAL_KEYWORDS 93
#define MIN_WORD_LENGTH 2
#define MAX_WORD_LENGTH 32
#define MIN_HASH_VALUE 3
#define MAX_HASH_VALUE 181
/* maximum key range = 179, duplicates = 0 */

#ifndef GPERF_DOWNCASE
#define GPERF_DOWNCASE 1
static unsigned char gperf_downcase[256] =
  {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
     15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
     30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
     45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
     60,  61,  62,  63,  64,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106,
    107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
    122,  91,  92,  93,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224,
    225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255
  };
#endif

#ifndef GPERF_CASE_STRNCMP
#define GPERF_CASE_STRNCMP 1
static int
gperf_case_strncmp (register const char *s1, register const char *s2, register size_t n)
{
  for (; n > 0;)
    {
      unsigned char c1 = gperf_downcase[(unsigned char)*s1++];
      unsigned char c2 = gperf_downcase[(unsigned char)*s2++];
      if (c1 != 0 && c1 == c2)
        {
          n--;
          continue;
        }
      return (int)c1 - (int)c2;
    }
  return 0;
}
#endif

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
static unsigned int
http_header_hash (register const char *str, register size_t len)
{
  static const unsigned char asso_values[] =
    {
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182,  35, 182, 182, 182, 182,
      182, 182, 182,   5, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182,  15, 182,  30,  60,  10,
        5,  40,   0,  40, 182,  80,  40,  90,   0,  45,
        5,   0,  45,   0,   0,   0, 182,   0, 182,  45,
      182, 182, 182, 182, 182, 182, 182,  15, 182,  30,
       60,  10,   5,  40,   0,  40, 182,  80,  40,  90,
        0,  45,   5,   0,  45,   0,   0,   0, 182,   0,
      182,  45, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
      182, 182, 182, 182, 182, 182
    };
  register unsigned int hval = len;

  switch (hval)
    {
      default:
        hval += asso_values[(unsigned char)str[21]];
      /*FALLTHROUGH*/
      case 21:
      case 20:
      case 19:
      case 18:
      case 17:
      case 16:
      case 15:
      case 14:
      case 13:
      case 12:
      case 11:
        hval += asso_values[(unsigned char)str[10]];
      /*FALLTHROUGH*/
      case 10:
        hval += asso_values[(unsigned char)str[9]];
      /*FALLTHROUGH*/
      case 9:
      case 8:
      case 7:
      case 6:
      case 5:
        hval += asso_values[(unsigned char)str[4]];
      /*FALLTHROUGH*/
      case 4:
      case 3:
      case 2:
        break;
    }
  return hval + asso_values[(unsigned char)str[len - 1]];
}

const struct HttpHeaderSlot *
http_lookup_header (register const char *str, register size_t len)
{
  static const struct HttpHeaderSlot wordlist[] =
    {
      {"",-1}, {"",-1}, {"",-1},
#line 55 "http_headers.gperf"
      {"DNT",                               ARK_HTTP_HEADER_DNT},
#line 42 "http_headers.gperf"
      {"Host",                              ARK_HTTP_HEADER_HOST},
#line 74 "http_headers.gperf"
      {"Allow",                             ARK_HTTP_HEADER_ALLOW},
      {"",-1}, {"",-1},
#line 101 "http_headers.gperf"
      {"Location",                          ARK_HTTP_HEADER_LOCATION},
      {"",-1}, {"",-1},
#line 45 "http_headers.gperf"
      {"Accept",                            ARK_HTTP_HEADER_ACCEPT},
#line 108 "http_headers.gperf"
      {"TE",                                ARK_HTTP_HEADER_TE},
#line 86 "http_headers.gperf"
      {"Age",                               ARK_HTTP_HEADER_AGE},
#line 68 "http_headers.gperf"
      {"Date",                              ARK_HTTP_HEADER_DATE},
      {"",-1}, {"",-1}, {"",-1},
#line 114 "http_headers.gperf"
      {"Via",                               ARK_HTTP_HEADER_VIA},
      {"",-1},
#line 44 "http_headers.gperf"
      {"Connection",                        ARK_HTTP_HEADER_CONNECTION},
      {"",-1}, {"",-1},
#line 95 "http_headers.gperf"
      {"If-Match",                          ARK_HTTP_HEADER_IF_MATCH},
      {"",-1},
#line 64 "http_headers.gperf"
      {"Range",                             ARK_HTTP_HEADER_RANGE},
#line 120 "http_headers.gperf"
      {"X-XSS-Protection",                  ARK_HTTP_HEADER_X_XSS_PROTECTION},
      {"",-1},
#line 78 "http_headers.gperf"
      {"Access-Control-Allow-Headers",      ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_HEADERS},
#line 83 "http_headers.gperf"
      {"Access-Control-RequestHeaders",     ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_HEADERS},
#line 85 "http_headers.gperf"
      {"Access-Control-Request-Methods",    ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_METHODS},
#line 81 "http_headers.gperf"
      {"Access-Control-MaxAge",             ARK_HTTP_HEADER_ACCESS_CONTROL_MAX_AGE},
#line 111 "http_headers.gperf"
      {"Upgrade",                           ARK_HTTP_HEADER_UPGRADE},
#line 97 "http_headers.gperf"
      {"If-Range",                          ARK_HTTP_HEADER_IF_RANGE},
#line 65 "http_headers.gperf"
      {"Content-Length",                    ARK_HTTP_HEADER_CONTENT_LENGTH},
#line 116 "http_headers.gperf"
      {"X-Frame-Options",                   ARK_HTTP_HEADER_X_FRAME_OPTIONS},
#line 94 "http_headers.gperf"
      {"Expect",                            ARK_HTTP_HEADER_EXPECT},
#line 118 "http_headers.gperf"
      {"Alt-Svc",                           ARK_HTTP_HEADER_ALT_SVC},
      {"",-1},
#line 89 "http_headers.gperf"
      {"Content-Description",               ARK_HTTP_HEADER_CONTENT_DESCRIPTION},
      {"",-1},
#line 113 "http_headers.gperf"
      {"WWW-Authenticate",                  ARK_HTTP_HEADER_WWW_AUTHENTICATE},
      {"",-1}, {"",-1},
#line 73 "http_headers.gperf"
      {"ETag",                              ARK_HTTP_HEADER_ETAG},
#line 48 "http_headers.gperf"
      {"User-Agent",                        ARK_HTTP_HEADER_USER_AGENT},
#line 51 "http_headers.gperf"
      {"Origin",                            ARK_HTTP_HEADER_ORIGIN},
#line 88 "http_headers.gperf"
      {"Content-Base",                      ARK_HTTP_HEADER_CONTENT_BASE},
#line 75 "http_headers.gperf"
      {"Content-Range",                     ARK_HTTP_HEADER_CONTENT_RANGE},
#line 67 "http_headers.gperf"
      {"Vary",                              ARK_HTTP_HEADER_VARY},
#line 52 "http_headers.gperf"
      {"Upgrade-Insecure-Requests",         ARK_HTTP_HEADER_UPGRADE_INSECURE_REQUESTS},
#line 91 "http_headers.gperf"
      {"Content-Language",                  ARK_HTTP_HEADER_CONTENT_LANGUAGE},
#line 70 "http_headers.gperf"
      {"Expires",                           ARK_HTTP_HEADER_EXPIRES},
#line 134 "http_headers.gperf"
      {"CDN-Loop",                          ARK_HTTP_HEADER_CDN_LOOP},
#line 123 "http_headers.gperf"
      {"Sec-CH-UA",                         ARK_HTTP_HEADER_SEC_CH_UA},
      {"",-1},
#line 54 "http_headers.gperf"
      {"Cookie",                            ARK_HTTP_HEADER_COOKIE},
#line 117 "http_headers.gperf"
      {"X-Content-Type-Options",            ARK_HTTP_HEADER_X_CONTENT_TYPE_OPTIONS},
#line 121 "http_headers.gperf"
      {"Accept-Ranges",                     ARK_HTTP_HEADER_ACCEPT_RANGES},
#line 63 "http_headers.gperf"
      {"Save-Data",                         ARK_HTTP_HEADER_SAVE_DATA},
#line 122 "http_headers.gperf"
      {"Set-Cookie",                        ARK_HTTP_HEADER_SET_COOKIE},
#line 69 "http_headers.gperf"
      {"Server",                            ARK_HTTP_HEADER_SERVER},
#line 77 "http_headers.gperf"
      {"Access-Control-Allow-Credentials",  ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_CREDENTIALS},
      {"",-1},
#line 126 "http_headers.gperf"
      {"Sec-Fetch-Site",                    ARK_HTTP_HEADER_SEC_FETCH_SITE},
#line 99 "http_headers.gperf"
      {"Keep-Alive",                        ARK_HTTP_HEADER_KEEP_ALIVE},
#line 130 "http_headers.gperf"
      {"CF-RAY",                            ARK_HTTP_HEADER_CF_RAY},
#line 110 "http_headers.gperf"
      {"Transfer-Encoding",                 ARK_HTTP_HEADER_TRANSFER_ENCODING},
      {"",-1},
#line 90 "http_headers.gperf"
      {"Content-Disposition",               ARK_HTTP_HEADER_CONTENT_DISPOSITION},
#line 46 "http_headers.gperf"
      {"Accept-Language",                   ARK_HTTP_HEADER_ACCEPT_LANGUAGE},
#line 105 "http_headers.gperf"
      {"Proxy-Connection",                  ARK_HTTP_HEADER_PROXY_CONNECTION},
#line 80 "http_headers.gperf"
      {"Access-Control-Allow-Origin",       ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN},
#line 96 "http_headers.gperf"
      {"If-None-Match",                     ARK_HTTP_HEADER_IF_NONE_MATCH},
#line 98 "http_headers.gperf"
      {"If-Unmodified-Since",               ARK_HTTP_HEADER_IF_UNMODIFIED_SINCE},
      {"",-1},
#line 106 "http_headers.gperf"
      {"Public",                            ARK_HTTP_HEADER_PUBLIC},
#line 56 "http_headers.gperf"
      {"Sec-GPC",                           ARK_HTTP_HEADER_SEC_GPC},
      {"",-1},
#line 76 "http_headers.gperf"
      {"Accept-Charset",                    ARK_HTTP_HEADER_ACCEPT_CHARSET},
      {"",-1},
#line 82 "http_headers.gperf"
      {"Access-Control-Method",             ARK_HTTP_HEADER_ACCESS_CONTROL_METHOD},
#line 66 "http_headers.gperf"
      {"Content-Type",                      ARK_HTTP_HEADER_CONTENT_TYPE},
#line 103 "http_headers.gperf"
      {"Proxy-Authenticate",                ARK_HTTP_HEADER_PROXY_AUTHENTICATE},
#line 100 "http_headers.gperf"
      {"Link",                              ARK_HTTP_HEADER_LINK},
      {"",-1},
#line 59 "http_headers.gperf"
      {"X-Requested-With",                  ARK_HTTP_HEADER_X_REQUESTED_WITH},
#line 112 "http_headers.gperf"
      {"Warning",                           ARK_HTTP_HEADER_WARNING},
      {"",-1},
#line 84 "http_headers.gperf"
      {"Access-Control-Request-Method",     ARK_HTTP_HEADER_ACCESS_CONTROL_REQUEST_METHOD},
      {"",-1},
#line 93 "http_headers.gperf"
      {"Content-MD5",                       ARK_HTTP_HEADER_CONTENT_MD5},
#line 109 "http_headers.gperf"
      {"Trailer",                           ARK_HTTP_HEADER_TRAILER},
      {"",-1},
#line 57 "http_headers.gperf"
      {"From",                              ARK_HTTP_HEADER_FROM},
      {"",-1},
#line 71 "http_headers.gperf"
      {"Content-Encoding",                  ARK_HTTP_HEADER_CONTENT_ENCODING},
#line 49 "http_headers.gperf"
      {"Referer",                           ARK_HTTP_HEADER_REFERER},
#line 87 "http_headers.gperf"
      {"Authorization",                     ARK_HTTP_HEADER_AUTHORIZATION},
#line 128 "http_headers.gperf"
      {"Sec-Fetch-User",                    ARK_HTTP_HEADER_SEC_FETCH_USER},
      {"",-1},
#line 92 "http_headers.gperf"
      {"Content-Location",                  ARK_HTTP_HEADER_CONTENT_LOCATION},
      {"",-1}, {"",-1}, {"",-1}, {"",-1},
#line 132 "http_headers.gperf"
      {"CF-Connecting-IP",                  ARK_HTTP_HEADER_CF_CONNECTING_IP},
#line 133 "http_headers.gperf"
      {"CF-IPCountry",                      ARK_HTTP_HEADER_CF_IPCOUNTRY},
#line 43 "http_headers.gperf"
      {"Cache-Control",                     ARK_HTTP_HEADER_CACHE_CONTROL},
#line 104 "http_headers.gperf"
      {"Proxy-Authorization",               ARK_HTTP_HEADER_PROXY_AUTHORIZATION},
      {"",-1},
#line 53 "http_headers.gperf"
      {"Pragma",                            ARK_HTTP_HEADER_PRAGMA},
      {"",-1}, {"",-1},
#line 129 "http_headers.gperf"
      {"Sec-Fetch-Dest",                    ARK_HTTP_HEADER_SEC_FETCH_DEST},
      {"",-1}, {"",-1}, {"",-1},
#line 79 "http_headers.gperf"
      {"Access-Control-Allow-Methods",      ARK_HTTP_HEADER_ACCESS_CONTROL_ALLOW_METHODS},
      {"",-1}, {"",-1}, {"",-1},
#line 102 "http_headers.gperf"
      {"Max-Forwards",                      ARK_HTTP_HEADER_MAX_FORWARDS},
      {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1},
      {"",-1}, {"",-1},
#line 60 "http_headers.gperf"
      {"X-Forwarded-Host",                  ARK_HTTP_HEADER_X_FORWARDED_HOST},
      {"",-1}, {"",-1}, {"",-1},
#line 47 "http_headers.gperf"
      {"Accept-Encoding",                   ARK_HTTP_HEADER_ACCEPT_ENCODING},
      {"",-1}, {"",-1}, {"",-1}, {"",-1},
#line 131 "http_headers.gperf"
      {"CF-Visitor",                        ARK_HTTP_HEADER_CF_VISITOR},
      {"",-1},
#line 58 "http_headers.gperf"
      {"If-Modified-Since",                 ARK_HTTP_HEADER_IF_MODIFIED_SINCE},
      {"",-1}, {"",-1}, {"",-1}, {"",-1},
#line 62 "http_headers.gperf"
      {"X-CSRF-Token",                      ARK_HTTP_HEADER_X_CSRF_TOKEN},
      {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1},
#line 72 "http_headers.gperf"
      {"Last-Modified",                     ARK_HTTP_HEADER_LAST_MODIFIED},
#line 127 "http_headers.gperf"
      {"Sec-Fetch-Mode",                    ARK_HTTP_HEADER_SEC_FETCH_MODE},
#line 119 "http_headers.gperf"
      {"Referrer-Policy",                   ARK_HTTP_HEADER_REFERRER_POLICY},
#line 107 "http_headers.gperf"
      {"Retry-After",                       ARK_HTTP_HEADER_RETRY_AFTER},
      {"",-1}, {"",-1}, {"",-1},
#line 115 "http_headers.gperf"
      {"Strict-Transport-Security",         ARK_HTTP_HEADER_STRICT_TRANSPORT_SECURITY},
      {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1},
      {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1}, {"",-1},
      {"",-1}, {"",-1},
#line 50 "http_headers.gperf"
      {"X-Forwarded-For",                   ARK_HTTP_HEADER_X_FORWARDED_FOR},
      {"",-1},
#line 61 "http_headers.gperf"
      {"X-Forwarded-Proto",                 ARK_HTTP_HEADER_X_FORWARDED_PROTO},
#line 125 "http_headers.gperf"
      {"Sec-CH-UA-Platform",                ARK_HTTP_HEADER_SEC_CH_UA_PLATFORM},
      {"",-1}, {"",-1},
#line 124 "http_headers.gperf"
      {"Sec-CH-UA-Mobile",                  ARK_HTTP_HEADER_SEC_CH_UA_MOBILE}
    };

  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      register unsigned int key = http_header_hash (str, len);

      if (key <= MAX_HASH_VALUE)
        {
          register const char *s = wordlist[key].name;

          if ((((unsigned char)*str ^ (unsigned char)*s) & ~32) == 0 && !gperf_case_strncmp (str, s, len) && s[len] == '\0')
            return &wordlist[key];
        }
    }
  return 0;
}
#line 135 "http_headers.gperf"


int ark_http_header_id(const unsigned char *name, size_t length) {
    const struct HttpHeaderSlot *slot = http_lookup_header((const char *)name, length);

    return slot != NULL ? slot->code : -1;
}
