#pragma once

/* Push notification via ntfy.sh. No-op when no topic is configured.
 * Blocking; call from a network task. Title should be plain ASCII. */
void notify_send(const char *title, const char *message);
