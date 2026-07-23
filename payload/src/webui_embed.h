/*
 * webui_embed.h — symbols provided by webui_embed.c.
 *
 * webui_embed.c is GENERATED AT BUILD TIME by the Makefile from
 * ../webui/dist/index.html (gzip -9 + xxd -i style byte arrays).  The
 * checked-in webui_embed.c is a documented placeholder so the source tree
 * always compiles conceptually even before the React UI has been built;
 * it embeds a tiny gzip'd placeholder page and sets
 * webui_embed_available = 0 so the HTTP server can tell a stub build from
 * a real one (it still serves the placeholder bytes).
 */
#ifndef PS5LM_WEBUI_EMBED_H
#define PS5LM_WEBUI_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

/* gzip-compressed index.html of the embedded single-page app. */
extern const unsigned char webui_index_html_gz[];
extern const unsigned int  webui_index_html_gz_len;

/* 1 when a real webui bundle was embedded at build time, 0 for the stub. */
extern const int webui_embed_available;

#ifdef __cplusplus
}
#endif

#endif /* PS5LM_WEBUI_EMBED_H */
