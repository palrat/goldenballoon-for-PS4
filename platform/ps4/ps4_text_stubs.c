/*
 * ps4_text_stubs.c — placeholder for the character-text shaping libraries.
 *
 * The engine links HarfBuzz (C++) and SheenBidi (C) for the *modern character*
 * text path. Neither is built for Orbis yet, and the base game does not need
 * them, so this file satisfies the link with objects that never fail a null
 * check and never touch the arguments.
 *
 * Replacing this with the real libraries is a self-contained job: SheenBidi is
 * 33 C files and builds as-is; HarfBuzz is C++ and needs the toolchain's libc++.
 * Until then the modern character captions render unshaped (or not at all),
 * which is visible but harmless — the game itself is unaffected.
 */
#include <stddef.h>
#include <stdint.h>

/* ---- HarfBuzz --------------------------------------------------------- */
/* Complete types: a tentative definition of an incomplete type is an error. */
struct hb_blob_t          { int unused; };
struct hb_face_t          { int unused; };
struct hb_font_t          { int unused; };
struct hb_unicode_funcs_t { int unused; };
typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_unicode_funcs_t hb_unicode_funcs_t;
typedef uint32_t hb_codepoint_t;
typedef int hb_script_t;
typedef int hb_unicode_general_category_t;

static hb_blob_t s_stub_blob;
static hb_face_t s_stub_face;
static hb_font_t s_stub_font;
static hb_unicode_funcs_t s_stub_unicode;

hb_blob_t *hb_blob_create(const char *data, unsigned int length, int mode,
                          void *user_data, void (*destroy)(void *)) {
    (void)data; (void)length; (void)mode; (void)user_data; (void)destroy;
    return &s_stub_blob;
}
hb_blob_t *hb_blob_get_empty(void) { return &s_stub_blob; }
void hb_blob_destroy(hb_blob_t *blob) { (void)blob; }

hb_face_t *hb_face_create(hb_blob_t *blob, unsigned int index) {
    (void)blob; (void)index;
    return &s_stub_face;
}
hb_face_t *hb_face_get_empty(void) { return &s_stub_face; }
void hb_face_destroy(hb_face_t *face) { (void)face; }

hb_font_t *hb_font_create(hb_face_t *face) { (void)face; return &s_stub_font; }
void hb_font_destroy(hb_font_t *font) { (void)font; }

hb_unicode_funcs_t *hb_unicode_funcs_get_default(void) { return &s_stub_unicode; }
hb_script_t hb_unicode_script(hb_unicode_funcs_t *ufuncs, hb_codepoint_t unicode) {
    (void)ufuncs; (void)unicode;
    return 0;
}
hb_unicode_general_category_t hb_unicode_general_category(hb_unicode_funcs_t *ufuncs,
                                                          hb_codepoint_t unicode) {
    (void)ufuncs; (void)unicode;
    return 0;
}

/* ---- SheenBidi -------------------------------------------------------- */
struct SBAlgorithm { int unused; };
struct SBParagraph { int unused; };
struct SBLine      { int unused; };
struct SBRun       { int unused; };
typedef struct SBAlgorithm SBAlgorithm;
typedef struct SBParagraph SBParagraph;
typedef struct SBLine SBLine;
typedef struct SBRun SBRun;

static SBAlgorithm s_stub_algorithm;
static SBParagraph s_stub_paragraph;
static SBLine s_stub_line;

SBAlgorithm *SBAlgorithmCreate(const void *string) { (void)string; return &s_stub_algorithm; }
void SBAlgorithmRelease(SBAlgorithm *algorithm) { (void)algorithm; }

SBParagraph *SBAlgorithmCreateParagraph(SBAlgorithm *algorithm, size_t offset,
                                        int length, int level) {
    (void)algorithm; (void)offset; (void)length; (void)level;
    return &s_stub_paragraph;
}
void SBParagraphRelease(SBParagraph *paragraph) { (void)paragraph; }

SBLine *SBParagraphCreateLine(SBParagraph *paragraph, size_t start, size_t end) {
    (void)paragraph; (void)start; (void)end;
    return &s_stub_line;
}
void SBLineRelease(SBLine *line) { (void)line; }

size_t SBLineGetRunCount(const SBLine *line) { (void)line; return 0; }
const SBRun *SBLineGetRunsPtr(const SBLine *line) { (void)line; return NULL; }

/* ---- HarfBuzz buffer/shape API ---------------------------------------- */
struct hb_buffer_t  { int unused; };
struct hb_language_t_stub { int unused; };
typedef struct hb_buffer_t hb_buffer_t;

typedef struct hb_glyph_info_t {
    hb_codepoint_t codepoint;
    uint32_t mask;
    uint32_t cluster;
    uint32_t var1;
    uint32_t var2;
} hb_glyph_info_t;

typedef struct hb_glyph_position_t {
    int32_t x_advance, y_advance, x_offset, y_offset;
    uint32_t var;
} hb_glyph_position_t;

static hb_buffer_t s_stub_buffer;

hb_buffer_t *hb_buffer_create(void) { return &s_stub_buffer; }
hb_buffer_t *hb_buffer_get_empty(void) { return &s_stub_buffer; }
void hb_buffer_destroy(hb_buffer_t *buffer) { (void)buffer; }
void hb_buffer_add_utf32(hb_buffer_t *buffer, const uint32_t *text,
                         int text_length, unsigned int item_offset,
                         int item_length) {
    (void)buffer; (void)text; (void)text_length; (void)item_offset; (void)item_length;
}
void hb_buffer_set_cluster_level(hb_buffer_t *buffer, int cluster_level) {
    (void)buffer; (void)cluster_level;
}
void hb_buffer_set_direction(hb_buffer_t *buffer, int direction) {
    (void)buffer; (void)direction;
}
void hb_buffer_set_script(hb_buffer_t *buffer, hb_script_t script) {
    (void)buffer; (void)script;
}
void hb_buffer_set_language(hb_buffer_t *buffer, const void *language) {
    (void)buffer; (void)language;
}
void *hb_language_from_string(const char *str, int len) { (void)str; (void)len; return NULL; }
unsigned int hb_face_get_upem(const hb_face_t *face) { (void)face; return 1000u; }
void hb_font_set_scale(hb_font_t *font, int x_scale, int y_scale) {
    (void)font; (void)x_scale; (void)y_scale;
}
hb_font_t *hb_font_get_empty(void) { return &s_stub_font; }
void hb_ot_font_set_funcs(hb_font_t *font) { (void)font; }
void hb_shape(const hb_font_t *font, hb_buffer_t *buffer,
              const void *features, unsigned int num_features) {
    (void)font; (void)buffer; (void)features; (void)num_features;
}
hb_glyph_info_t *hb_buffer_get_glyph_infos(hb_buffer_t *buffer,
                                           unsigned int *length) {
    (void)buffer;
    if (length) *length = 0;
    return NULL;
}
hb_glyph_position_t *hb_buffer_get_glyph_positions(hb_buffer_t *buffer,
                                                   unsigned int *length) {
    (void)buffer;
    if (length) *length = 0;
    return NULL;
}

/* ---- Party / remote-pad ingress ---------------------------------------
 * The launcher owns the transport; a packaged game has no launcher, so the
 * engine-side read path reports "no remote pad" and its rumble requests are
 * dropped. The functions exist because platform_sdl_min.c calls them.
 */
#include <stdbool.h>

bool mdkr_native_remote_pad_info(unsigned port, uint64_t *out_owner,
                                 uint32_t *out_connection_sequence) {
    (void)port;
    if (out_owner) *out_owner = 0;
    if (out_connection_sequence) *out_connection_sequence = 0;
    return false;
}

size_t mdkr_native_remote_pad_pop(unsigned port, uint64_t owner,
                                  uint32_t connection_sequence,
                                  uint8_t *output, size_t capacity) {
    (void)port; (void)owner; (void)connection_sequence; (void)output; (void)capacity;
    return 0;
}

bool mdkr_native_remote_pad_request_rumble(unsigned port, uint16_t strength) {
    (void)port; (void)strength;
    return false;
}

bool mdkr_native_remote_pad_haptics_supported(unsigned port) {
    (void)port;
    return false;
}

/* Opaque here on purpose: the engine passes its snapshot by pointer and the
 * call is a no-op without a launcher-owned transport. */
struct MdkrNativeRaceState;

bool mdkr_native_remote_pad_publish_race_state(unsigned port,
                                              const struct MdkrNativeRaceState *state) {
    (void)port; (void)state;
    return false;
}
