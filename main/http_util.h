#pragma once

#include <stddef.h>
#include "esp_err.h"

/* GET url into caller-provided buffer; NUL-terminates. Follows redirects,
 * uses the mbedTLS cert bundle for https URLs. */
esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len);

/* Same, with one extra request header (e.g. an API key). */
esp_err_t http_get_to_buffer_hdr(const char *url, char *buf, size_t buf_size, size_t *out_len,
                                 const char *hdr_key, const char *hdr_val);

/* POST a small text body (used for ntfy notifications). */
esp_err_t http_post_text(const char *url, const char *body,
                         const char *hdr_key, const char *hdr_val);
