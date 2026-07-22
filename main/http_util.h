#pragma once

#include <stddef.h>
#include "esp_err.h"

/* GET url into caller-provided buffer; NUL-terminates. Follows redirects,
 * uses the mbedTLS cert bundle for https URLs. */
esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len);
