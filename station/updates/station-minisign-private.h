/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Verify a minisign signature of a message against a minisign public key. The
 * signature is Ed25519, either over the raw message ("Ed") or over its
 * BLAKE2b-512 prehash ("ED", minisign's default). Crypto is vendored (monocypher,
 * no external dependency, works on Android too). Internal; not installed. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* @pubkey: a minisign public key string — the base64 line from a .pub file,
 *   which decodes to "Ed" + keynum[8] + ed25519_pk[32] (42 bytes).
 * @message: the signed content (e.g. the SHA256SUMS bytes).
 * @minisig: the .minisig file contents.
 *
 * Returns TRUE iff @minisig is a valid minisign signature of @message under
 * @pubkey. FALSE + @error on a bad signature or malformed input. */
gboolean station_minisign_verify (const char *pubkey, GBytes *message,
                                  GBytes *minisig, GError **error);

G_END_DECLS
