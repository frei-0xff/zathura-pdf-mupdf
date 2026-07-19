/* SPDX-License-Identifier: Zlib */

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <glib-2.0/glib.h>

#include "plugin.h"
#include <girara/log.h>
#include <girara/utils.h>

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))

/* ───────────────────────────────────────────────
   Custom font loader
   ─────────────────────────────────────────────── */
static fz_font *custom_font_regular    = NULL;
static fz_font *custom_font_bold       = NULL;
static fz_font *custom_font_italic     = NULL;
static fz_font *custom_font_bolditalic = NULL;

#define CUSTOM_FONT_PATH "/home/frei/Downloads/Fonts/BookerlyLCD"

static fz_font *
load_custom_font(fz_context *ctx, const char *name, int bold, int italic,
                   int needs_exact_metrics)
{
  /* Replace only the built‑in “serif” family */
  if (strcmp(name, "serif") == 0) {
    fz_font *font = NULL;

    if (bold && italic) {
      if (!custom_font_bolditalic)
        custom_font_bolditalic = fz_new_font_from_file(ctx, NULL,
          CUSTOM_FONT_PATH "-BoldItalic.ttf", 0, 0);
      font = custom_font_bolditalic;
    } else if (bold) {
      if (!custom_font_bold)
        custom_font_bold = fz_new_font_from_file(ctx, NULL,
          CUSTOM_FONT_PATH "-Bold.ttf", 0, 0);
      font = custom_font_bold;
    } else if (italic) {
      if (!custom_font_italic)
        custom_font_italic = fz_new_font_from_file(ctx, NULL,
          CUSTOM_FONT_PATH "-Italic.ttf", 0, 0);
      font = custom_font_italic;
    } else {
      if (!custom_font_regular)
        custom_font_regular = fz_new_font_from_file(ctx, NULL,
          CUSTOM_FONT_PATH "-Regular.ttf", 0, 0);
      font = custom_font_regular;
    }

    if (font) {
      fz_keep_font(ctx, font);
      return font;
    }
  }

  /* For any other family name (sans‑serif, monospace) let MuPDF fall back
     to its own system font loader by returning NULL. */
  return NULL;
}
/* ─────────────────────────────────────────────── */

/* route mupdf warnings to the girara log instead of raw stderr */
static void mupdf_warning_callback(void* GIRARA_UNUSED(user), const char* message) {
  girara_debug("mupdf: %s", message);
}

/* route mupdf errors to the girara log instead of raw stderr */
static void mupdf_error_callback(void* GIRARA_UNUSED(user), const char* message) {
  girara_error("mupdf: %s", message);
}

zathura_error_t pdf_document_open(zathura_document_t* document) {
  zathura_error_t error = ZATHURA_ERROR_OK;
  if (document == NULL) {
    error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = calloc(1, sizeof(mupdf_document_t));
  if (mupdf_document == NULL) {
    error = ZATHURA_ERROR_OUT_OF_MEMORY;
    goto error_ret;
  }

  g_mutex_init(&mupdf_document->mutex);

  mupdf_document->ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  if (mupdf_document->ctx == NULL) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  fz_set_warning_callback(mupdf_document->ctx, mupdf_warning_callback, NULL);
  fz_set_error_callback(mupdf_document->ctx, mupdf_error_callback, NULL);

  /* open document */
  const char* path     = zathura_document_get_path(document);
  const char* password = zathura_document_get_password(document);

  fz_try(mupdf_document->ctx) {
    fz_register_document_handlers(mupdf_document->ctx);

    /* ----------- install our custom font loader ------------- */
    fz_install_load_system_font_funcs(mupdf_document->ctx,
                                      load_custom_font,
                                      NULL,   /* no CJK override */
                                      NULL);  /* no fallback override */
    /* -------------------------------------------------------- */

    gchar* user_css = NULL;
    gboolean css_loaded = FALSE;

    /* Try <document_path>.css */
    gchar* specific_css_path = g_strconcat(path, ".css", NULL);
    if (g_file_get_contents(specific_css_path, &user_css, NULL, NULL) == TRUE) {
      fz_set_user_css(mupdf_document->ctx, user_css);
      g_free(user_css);
      css_loaded = TRUE;
    }
    g_free(specific_css_path);

    /* Fall back to global epub.css (or hardcoded default) */
    if (!css_loaded) {
      char* xdg_path = girara_get_xdg_path(XDG_CONFIG);
      if (xdg_path != NULL) {
        char* css_path = g_build_filename(xdg_path, "zathura", "epub.css", NULL);
        if (g_file_get_contents(css_path, &user_css, NULL, NULL) == TRUE) {
          fz_set_user_css(mupdf_document->ctx, user_css);
          g_free(user_css);
        } else {
          const char *extra_css = "body {font-size: 0.95em;line-height: 1.4;}";
          fz_set_user_css(mupdf_document->ctx, extra_css);
        }
        g_free(css_path);
        g_free(xdg_path);
      }
    }

    mupdf_document->document = fz_open_document(mupdf_document->ctx, path);
  }
  fz_catch(mupdf_document->ctx) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  if (mupdf_document->document == NULL) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  /* authenticate if password is required and given */
  fz_try(mupdf_document->ctx) {
    if (fz_needs_password(mupdf_document->ctx, mupdf_document->document) != 0) {
      if (password == NULL ||
          fz_authenticate_password(mupdf_document->ctx,
                                   mupdf_document->document, password) == 0) {
        error = ZATHURA_ERROR_INVALID_PASSWORD;
      }
    }
  }
  fz_catch(mupdf_document->ctx) {
    error = ZATHURA_ERROR_UNKNOWN;
  }
  if (error != ZATHURA_ERROR_OK) {
    goto error_free;
  }

  fz_try(mupdf_document->ctx) {
    zathura_document_set_number_of_pages(
        document,
        fz_count_pages(mupdf_document->ctx, mupdf_document->document));
  }
  fz_catch(mupdf_document->ctx) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }
  zathura_document_set_data(document, mupdf_document);

  return ZATHURA_ERROR_OK;

error_free:

  if (mupdf_document != NULL) {
    g_mutex_clear(&mupdf_document->mutex);
    if (mupdf_document->document != NULL) {
      fz_drop_document(mupdf_document->ctx, mupdf_document->document);
    }
    if (mupdf_document->ctx != NULL) {
      fz_drop_context(mupdf_document->ctx);
    }

    free(mupdf_document);
  }

  zathura_document_set_data(document, NULL);

error_ret:

  return error;
}

zathura_error_t
pdf_document_free(zathura_document_t* document, void* data)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  g_mutex_lock(&mupdf_document->mutex);

  fz_drop_document(mupdf_document->ctx, mupdf_document->document);
  fz_drop_context(mupdf_document->ctx);

  g_mutex_unlock(&mupdf_document->mutex);
  g_mutex_clear(&mupdf_document->mutex);

  free(mupdf_document);
  zathura_document_set_data(document, NULL);

  return ZATHURA_ERROR_OK;
}

zathura_error_t
pdf_document_save_as(zathura_document_t* document, void* data, const char* path)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL || path == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  g_mutex_lock(&mupdf_document->mutex);
  fz_try(mupdf_document->ctx) {
    pdf_save_document(mupdf_document->ctx,
                      (pdf_document*)mupdf_document->document, path, NULL);
  }
  fz_catch(mupdf_document->ctx) {
    g_mutex_unlock(&mupdf_document->mutex);
    return ZATHURA_ERROR_UNKNOWN;
  }
  g_mutex_unlock(&mupdf_document->mutex);

  return ZATHURA_ERROR_OK;
}

girara_list_t*
pdf_document_get_information(zathura_document_t* document, void* data,
                             zathura_error_t* error)
{
  mupdf_document_t* mupdf_document = data;

  if (document == NULL || mupdf_document == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
  }

  girara_list_t* list = zathura_document_information_entry_list_new();
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  g_mutex_lock(&mupdf_document->mutex);
  fz_try(mupdf_document->ctx) {
    pdf_document* pdf_document =
        pdf_specifics(mupdf_document->ctx, mupdf_document->document);
    if (pdf_document == NULL) {
      girara_list_free(list);
      list = NULL;
      break;
    }

    pdf_obj* trailer   = pdf_trailer(mupdf_document->ctx, pdf_document);
    pdf_obj* info_dict = pdf_dict_get(mupdf_document->ctx, trailer, PDF_NAME(Info));

    /* get string values */
    typedef struct info_value_s {
      const char* property;
      zathura_document_information_type_t type;
    } info_value_t;

    static const info_value_t string_values[] = {
        {"Title",    ZATHURA_DOCUMENT_INFORMATION_TITLE},
        {"Author",   ZATHURA_DOCUMENT_INFORMATION_AUTHOR},
        {"Subject",  ZATHURA_DOCUMENT_INFORMATION_SUBJECT},
        {"Keywords", ZATHURA_DOCUMENT_INFORMATION_KEYWORDS},
        {"Creator",  ZATHURA_DOCUMENT_INFORMATION_CREATOR},
        {"Producer", ZATHURA_DOCUMENT_INFORMATION_PRODUCER},
    };

    for (unsigned int i = 0; i < LENGTH(string_values); i++) {
      pdf_obj* value =
          pdf_dict_gets(mupdf_document->ctx, info_dict, string_values[i].property);
      if (value == NULL) {
        continue;
      }

      const char* str_value = pdf_to_text_string(mupdf_document->ctx, value);
      if (str_value == NULL || strlen(str_value) == 0) {
        continue;
      }

      zathura_document_information_entry_t* entry =
          zathura_document_information_entry_new(string_values[i].type, str_value);

      if (entry != NULL) {
        girara_list_append(list, entry);
      }
    }

    static const info_value_t time_values[] = {
        {"CreationDate", ZATHURA_DOCUMENT_INFORMATION_CREATION_DATE},
        {"ModDate",      ZATHURA_DOCUMENT_INFORMATION_MODIFICATION_DATE},
    };

    for (unsigned int i = 0; i < LENGTH(time_values); i++) {
      pdf_obj* value =
          pdf_dict_gets(mupdf_document->ctx, info_dict, time_values[i].property);
      if (value == NULL) {
        continue;
      }

      const char* str_value = pdf_to_text_string(mupdf_document->ctx, value);
      if (str_value == NULL || strlen(str_value) == 0) {
        continue;
      }

      zathura_document_information_entry_t* entry =
          zathura_document_information_entry_new(
              time_values[i].type,
              str_value // FIXME: Convert to common format
          );

      if (entry != NULL) {
        girara_list_append(list, entry);
      }
    }
  }
  fz_catch(mupdf_document->ctx) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    girara_list_free(list);
    list = NULL;
  }
  g_mutex_unlock(&mupdf_document->mutex);

  return list;
}
