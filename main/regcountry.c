#include "regcountry.h"

#include <stddef.h>
#include <string.h>

/* Longest-prefix match; ordered so longer prefixes come first. */
static const struct {
    const char *prefix, *iso;
} k_reg[] = {
    { "SP-", "PL" }, { "SN-", "PL" },
    { "D-", "DE" },  { "G-", "GB" },  { "F-", "FR" },  { "I-", "IT" },
    { "EC-", "ES" }, { "CS-", "PT" }, { "PH-", "NL" }, { "OO-", "BE" },
    { "LX-", "LU" }, { "OE-", "AT" }, { "HB-", "CH" }, { "OK-", "CZ" },
    { "OM-", "SK" }, { "HA-", "HU" }, { "YR-", "RO" }, { "LZ-", "BG" },
    { "SX-", "GR" }, { "9H-", "MT" }, { "5B-", "CY" }, { "9A-", "HR" },
    { "S5-", "SI" }, { "OY-", "DK" }, { "SE-", "SE" }, { "LN-", "NO" },
    { "OH-", "FI" }, { "TF-", "IS" }, { "EI-", "IE" }, { "YL-", "LV" },
    { "LY-", "LT" }, { "ES-", "EE" }, { "UR-", "UA" }, { "EW-", "BY" },
    { "RA-", "RU" }, { "TC-", "TR" }, { "4X-", "IL" }, { "A6-", "AE" },
    { "A7-", "QA" }, { "HZ-", "SA" }, { "9V-", "SG" }, { "HS-", "TH" },
    { "JA", "JP" },  { "HL", "KR" },  { "B-", "CN" },  { "VT-", "IN" },
    { "VH-", "AU" }, { "ZK-", "NZ" }, { "C-", "CA" },  { "N", "US" },
    { "PP-", "BR" }, { "PT-", "BR" }, { "PR-", "BR" }, { "XA-", "MX" },
    { "CC-", "CL" }, { "LV-", "AR" }, { "ZS-", "ZA" }, { "ET-", "ET" },
    { "SU-", "EG" }, { "CN-", "MA" }, { "7T-", "DZ" }, { "TS-", "TN" },
    { "4L-", "GE" }, { "EK-", "AM" }, { "AP-", "PK" }, { "9M-", "MY" },
    { "PK-", "ID" }, { "RP-", "PH" }, { "VN-", "VN" }, { "B-1", "CN" },
};

const char *reg_country(const char *registration)
{
    if (registration == NULL || registration[0] == '\0') {
        return NULL;
    }
    /* two-pass: prefer longer prefixes */
    for (int want = 3; want >= 1; want--) {
        for (size_t i = 0; i < sizeof(k_reg) / sizeof(k_reg[0]); i++) {
            size_t n = strlen(k_reg[i].prefix);
            if ((int)n == want && strncmp(registration, k_reg[i].prefix, n) == 0) {
                return k_reg[i].iso;
            }
        }
    }
    return NULL;
}
