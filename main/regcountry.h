#pragma once

/* ISO country code from an aircraft registration prefix ("SP-LRA" -> "PL"),
 * or NULL when unknown. */
const char *reg_country(const char *registration);
