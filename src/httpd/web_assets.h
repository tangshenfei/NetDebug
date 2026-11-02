#ifndef ND_WEB_ASSETS_H
#define ND_WEB_ASSETS_H

#include <stddef.h>

typedef struct nd_web_asset {
    const char *path;
    const unsigned char *data;
    size_t len;
} nd_web_asset;

extern const nd_web_asset nd_web_assets[];
extern const size_t nd_web_assets_count;

#endif /* ND_WEB_ASSETS_H */
