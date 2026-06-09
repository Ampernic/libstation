/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Updates"
#include "station-minisign-private.h"
#include "vendor/monocypher.h"

#include <gio/gio.h>
#include <string.h>

#define MINISIGN_FAIL(...) \
  G_STMT_START { g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, __VA_ARGS__); \
                 return FALSE; } G_STMT_END

/* Base64-decode a NUL-terminated, whitespace-trimmed line; require @want bytes.
 * Returns a g_malloc'd buffer (caller frees) or NULL. */
static guchar *
decode_fixed (const char *b64, gsize want, gsize *out_len)
{
  char *trimmed = g_strstrip (g_strdup (b64));
  gsize n = 0;
  guchar *raw = g_base64_decode (trimmed, &n);
  g_free (trimmed);
  if (raw == NULL || n != want)
    { g_free (raw); return NULL; }
  *out_len = n;
  return raw;
}

gboolean
station_minisign_verify (const char *pubkey, GBytes *message,
                         GBytes *minisig, GError **error)
{
  g_return_val_if_fail (pubkey != NULL && message != NULL && minisig != NULL, FALSE);

  /* --- public key: "Ed" + keynum[8] + ed25519_pk[32] = 42 bytes --- */
  gsize pk_len = 0;
  guchar *pk_raw = decode_fixed (pubkey, 42, &pk_len);
  if (pk_raw == NULL)
    MINISIGN_FAIL ("invalid minisign public key");
  if (pk_raw[0] != 'E' || pk_raw[1] != 'd')
    { g_free (pk_raw); MINISIGN_FAIL ("unsupported public-key algorithm"); }
  guchar pk[32];
  memcpy (pk, pk_raw + 10, 32);
  g_free (pk_raw);

  /* --- signature file: the 2nd line is base64 of sig_alg[2]+keynum[8]+sig[64]
   *     = 74 bytes (line 1 is "untrusted comment:", lines 3-4 are the trusted
   *     comment + its global signature, which we don't need). --- */
  gsize slen = 0;
  const char *sdata = g_bytes_get_data (minisig, &slen);
  char *text = g_strndup (sdata, slen);
  char **lines = g_strsplit (text, "\n", -1);
  g_free (text);

  /* The signature is the first non-comment, non-empty line. */
  const char *sigline = NULL;
  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *l = g_strstrip (lines[i]);
      if (*l == '\0' || g_str_has_prefix (l, "untrusted comment:")
          || g_str_has_prefix (l, "trusted comment:"))
        continue;
      sigline = l;
      break;
    }
  gsize sig_len = 0;
  guchar *sig_raw = sigline != NULL ? decode_fixed (sigline, 74, &sig_len) : NULL;
  g_strfreev (lines);
  if (sig_raw == NULL)
    MINISIGN_FAIL ("malformed minisign signature");

  gboolean prehashed;
  if (sig_raw[0] == 'E' && sig_raw[1] == 'D')      prehashed = TRUE;   /* BLAKE2b-512 */
  else if (sig_raw[0] == 'E' && sig_raw[1] == 'd') prehashed = FALSE;  /* raw message */
  else { g_free (sig_raw); MINISIGN_FAIL ("unsupported signature algorithm"); }

  guchar sig[64];
  memcpy (sig, sig_raw + 10, 64);
  g_free (sig_raw);

  /* --- verify Ed25519 over the (optionally BLAKE2b-512 prehashed) message --- */
  gsize msg_len = 0;
  const guchar *msg = g_bytes_get_data (message, &msg_len);
  int ok;
  if (prehashed)
    {
      guint8 h[64];
      crypto_blake2b (h, sizeof h, msg, msg_len);
      ok = crypto_eddsa_check (sig, pk, h, sizeof h);
    }
  else
    ok = crypto_eddsa_check (sig, pk, msg, msg_len);

  if (ok != 0)
    MINISIGN_FAIL ("signature does not verify");
  return TRUE;
}
