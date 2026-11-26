/*
 * file_crypt.c — Keys and 3DES-CBC encryption for config and field-map files
 */

#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#ifdef ALLTRAX_HAVE_CRYPTO
#include <openssl/evp.h>
#endif
#include "file_crypt.h"
#include "file_io.h"

/* ------------------------------------------------------------------ */
/* Built-in 3DES keys and IVs (make EMBEDDED_KEYS=0 omits them)       */
/* ------------------------------------------------------------------ */

#ifdef ALLTRAX_EMBEDDED_KEYS
static const cli_key embedded_config_key = {
    .encrypt = true,
    .key = {
        248, 184, 42, 77, 161, 89, 116, 236, 75, 162,
        82, 159, 46, 236, 212, 89, 30, 199, 23, 166,
        168, 153, 90, 182
    },
    .iv = {
        250, 217, 74, 37, 158, 170, 24, 231
    },
};
static const cli_key embedded_field_map_key = {
    .encrypt = true,
    .key = {
        73, 45, 205, 127, 225, 133, 145, 178, 174, 203,
        252, 8, 253, 19, 8, 108, 120, 51, 118, 17,
        160, 85, 62, 188
    },
    .iv = {
        48, 60, 181, 207, 29, 128, 123, 143
    },
};
#define EMBEDDED_CONFIG_KEY (&embedded_config_key)
#define EMBEDDED_FIELD_MAP_KEY (&embedded_field_map_key)
#else
#define EMBEDDED_CONFIG_KEY NULL
#define EMBEDDED_FIELD_MAP_KEY NULL
#endif

const cli_key_source cli_config_key_source = {
    "config", "ALLTRAX_CONFIG_KEY", EMBEDDED_CONFIG_KEY,
};
const cli_key_source cli_field_map_key_source = {
    "field-map", "ALLTRAX_FIELDMAP_KEY", EMBEDDED_FIELD_MAP_KEY,
};

/* ------------------------------------------------------------------ */
/* Key resolution                                                      */
/* ------------------------------------------------------------------ */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char* text, unsigned char* out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int hi = hex_digit(text[2 * i]), lo = hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return true;
}

/* Parse "<48 hex digits>:<16 hex digits>". */
static bool parse_key(const char* text, cli_key* out)
{
    const char* colon = strchr(text, ':');
    if (!colon || colon - text != 2 * (long)sizeof(out->key) ||
        strlen(colon + 1) != 2 * sizeof(out->iv))
        return false;
    out->encrypt = true;
    return parse_hex(text, out->key, sizeof(out->key)) &&
           parse_hex(colon + 1, out->iv, sizeof(out->iv));
}

int cli_resolve_key(const char* command, const char* value,
    const cli_key_source* source, cli_key* out)
{
    memset(out, 0, sizeof(*out));
    if (value && strcmp(value, "none") == 0)
        return 0;
#ifndef ALLTRAX_HAVE_CRYPTO
    fprintf(stderr, "Error: %s: this build reads and writes only unencrypted files "
            "(built without libcrypto). Use --key none.\n", command);
    return 1;
#endif
    if (value && strcmp(value, "auto") != 0) {
        if (parse_key(value, out)) return 0;
        fprintf(stderr, "Error: %s: --key must be auto, none, or "
                "<48 hex digits>:<16 hex digits>\n", command);
        return 1;
    }

    const char* text = getenv(source->env);
    if (text && *text) {
        if (parse_key(text, out)) return 0;
        fprintf(stderr, "Error: %s: %s must hold <48 hex digits>:<16 hex digits>\n",
                command, source->env);
        return 1;
    }
    if (source->embedded) {
        *out = *source->embedded;
        out->encrypt = true;
        return 0;
    }
    fprintf(stderr, "Error: %s: no key is built in. Use --key <48 hex digits>:<16 hex digits>, "
            "set %s, or use --key none for an unencrypted file\n",
            command, source->env);
    return 1;
}

void cli_print_key_status(FILE* out, const cli_key_source* source)
{
#ifdef ALLTRAX_HAVE_CRYPTO
    fprintf(out, "\nBuilt-in %s key: %s. %s=<key>:<iv> supplies a key for auto.\n",
            source->what, source->embedded ? "yes" : "no", source->env);
#else
    fprintf(out, "\nEncrypted %s files: unavailable (built without libcrypto). "
            "Use --key none.\n", source->what);
#endif
}

#ifdef ALLTRAX_HAVE_CRYPTO

/* ------------------------------------------------------------------ */
/* 3DES-CBC encrypt / decrypt                                          */
/* ------------------------------------------------------------------ */

static unsigned char* des3_encrypt(const cli_key* key, const char* plain,
    size_t plain_len, size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    unsigned char* out = malloc(plain_len + 8);  /* +block for padding */
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_EncryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key->key, key->iv))
        goto fail;
    if (!EVP_EncryptUpdate(ctx, out, &len,
                         (const unsigned char*)plain, (int)plain_len))
        goto fail;
    total = len;
    if (!EVP_EncryptFinal_ex(ctx, out + total, &len))
        goto fail;
    total += len;

    EVP_CIPHER_CTX_free(ctx);
    *out_len = (size_t)total;
    return out;

/* OpenSSL failures release both the context and the partial ciphertext. */
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}

static char* des3_decrypt(const cli_key* key, const unsigned char* cipher,
    size_t cipher_len, size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    char* out = malloc(cipher_len + 1);
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_DecryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key->key, key->iv))
        goto fail;
    if (!EVP_DecryptUpdate(ctx, (unsigned char*)out, &len,
                         cipher, (int)cipher_len))
        goto fail;
    total = len;
    if (!EVP_DecryptFinal_ex(ctx, (unsigned char*)out + total, &len))
        goto fail;
    total += len;

    EVP_CIPHER_CTX_free(ctx);
    out[total] = '\0';
    *out_len = (size_t)total;
    return out;

/* OpenSSL failures release both the context and the partial plaintext. */
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}
#endif /* ALLTRAX_HAVE_CRYPTO */

/* ------------------------------------------------------------------ */
/* Encrypted file read / write                                         */
/* ------------------------------------------------------------------ */

char* cli_read_file(const char* path, const cli_key* key, size_t* len)
{
    char* data = cli_read_input(path, len);
    if (!data)
        return NULL;
#ifdef ALLTRAX_HAVE_CRYPTO
    if (key->encrypt) {
        char* plain = des3_decrypt(key, (const unsigned char*)data, *len, len);
        bool looks_plain = data[0] == '<' || data[0] == 'F';
        free(data);
        if (!plain)
            fprintf(stderr, "Error: 3DES decryption failed (wrong key or corrupt file)%s\n",
                    looks_plain ? "; the file looks unencrypted, so use --key none" : "");
        return plain;
    }
#else
    (void)key;
#endif
    return data;
}

int cli_write_file(const char* path, const cli_key* key, const char* data,
    size_t len)
{
#ifdef ALLTRAX_HAVE_CRYPTO
    if (key->encrypt) {
        size_t cipher_len;
        unsigned char* cipher = des3_encrypt(key, data, len, &cipher_len);
        if (!cipher) {
            fprintf(stderr, "Error: 3DES encryption failed; no file written\n");
            return 1;
        }
        int rc = cli_write_output(path, cipher, cipher_len);
        free(cipher);
        return rc;
    }
#else
    (void)key;
#endif
    return cli_write_output(path, data, len);
}
