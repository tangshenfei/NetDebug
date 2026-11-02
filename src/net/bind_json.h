#ifndef ND_BIND_JSON_H
#define ND_BIND_JSON_H

#include "net/planner.h"

#include "cJSON.h"

#include <stddef.h>

cJSON *nd_bind_to_json(const nd_bind_spec *b);
int    nd_bind_from_json(cJSON *obj, nd_bind_spec *b, char *err, size_t errlen);

#endif /* ND_BIND_JSON_H */
