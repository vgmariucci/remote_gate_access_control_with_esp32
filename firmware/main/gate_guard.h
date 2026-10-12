#ifndef GATE_GUARD_H
#define GATE_GUARD_H

#include <stdbool.h>

/* Checks every secret in admin_credentials.h.
 *
 * Returns true when all are fit for deployment. On a development build
 * returns false after complaining; on a production build it does not
 * return at all. */
bool gate_guard_check(void);

#endif /* GATE_GUARD_H */
