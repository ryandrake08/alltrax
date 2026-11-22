/*
 * cmd_config.c — alltrax config save/load: Backup and restore settings
 *
 * Reads/writes encrypted XML config files compatible with the Windows
 * Alltrax Toolkit. Uses 3DES-CBC encryption via OpenSSL libcrypto
 * and libxml2 for XML generation/parsing.
 */

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* ------------------------------------------------------------------ */
/* 3DES key and IV                                                    */
/* ------------------------------------------------------------------ */

static const unsigned char DES3_KEY[24] = {
    248, 184, 42, 77, 161, 89, 116, 236, 75, 162,
    82, 159, 46, 236, 212, 89, 30, 199, 23, 166,
    168, 153, 90, 182
};

static const unsigned char DES3_IV[8] = {
    250, 217, 74, 37, 158, 170, 24, 231
};

/* ------------------------------------------------------------------ */
/* 3DES-CBC encrypt / decrypt                                          */
/* ------------------------------------------------------------------ */

static unsigned char* des3_encrypt(const char* plain, size_t plain_len,
    size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    unsigned char* out = malloc(plain_len + 8);  /* +block for padding */
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_EncryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, DES3_KEY, DES3_IV))
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

fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}

static char* des3_decrypt(const unsigned char* cipher, size_t cipher_len,
    size_t* out_len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    char* out = malloc(cipher_len + 1);
    if (!out) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int len = 0, total = 0;

    if (!EVP_DecryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, DES3_KEY, DES3_IV))
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

fail:
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Config save: groups to include                                     */
/* ------------------------------------------------------------------ */

static const alltrax_var_group save_groups[] = {
    ALLTRAX_VARS_VOLTAGE,
    ALLTRAX_VARS_NORMAL_USER,
    ALLTRAX_VARS_USER1,
    ALLTRAX_VARS_USER2,
    ALLTRAX_VARS_OTHER_SETTINGS,
    ALLTRAX_VARS_THROTTLE,
    ALLTRAX_VARS_FIELD,
};
#define NUM_SAVE_GROUPS (sizeof(save_groups) / sizeof(save_groups[0]))

/* ------------------------------------------------------------------ */
/* Curve-to-XML mapping                                               */
/* ------------------------------------------------------------------ */

static const struct {
    const char*       pointer_name;  /* XML element name for X */
    const char*       data_name;     /* XML element name for Y */
    const char*       curve_type;    /* alltrax_find_curve() key */
    alltrax_var_group group;         /* which Node-* to nest under */
} curve_xml_map[] = {
    { "Lin_Table_Pointer", "Lin_Table_Data", "linearization", ALLTRAX_VARS_THROTTLE },
    { "V_Table_Pointer",   "V_Table_Data",   "speed",         ALLTRAX_VARS_THROTTLE },
    { "I_Table_Pointer",   "I_Table_Data",   "torque",        ALLTRAX_VARS_THROTTLE },
    { "F_Table_Pointer",   "F_Table_Data",   "field",         ALLTRAX_VARS_FIELD    },
    { "Gen_Field_Pointer", "Gen_Field_Data", "genfield",      ALLTRAX_VARS_FIELD    },
};
#define NUM_CURVE_XML (sizeof(curve_xml_map) / sizeof(curve_xml_map[0]))

/* ------------------------------------------------------------------ */
/* XML generation (save)                                              */
/* ------------------------------------------------------------------ */

/* Format 16 raw int16 values as comma-separated string into buf.
 * buf must be at least 16*7 = 112 bytes. */
static void format_curve_csv(const double* display, double scale, char* buf,
    size_t bufsize)
{
    size_t pos = 0;
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        int16_t raw = (int16_t)round(display[i] / scale);
        int n = snprintf(buf + pos, bufsize - pos, "%s%d",
                         i > 0 ? "," : "", raw);
        if (n > 0) pos += (size_t)n;
    }
}

static void report_no_memory(void)
{
    fprintf(stderr, "Error: out of memory building config XML\n");
}

static bool saved_settings_valid(alltrax_controller* ctrl)
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Var_GoodSet"), alltrax_find_var("Fact_GoodSet"),
    };
    if (!vars[0] || !vars[1]) {
        fprintf(stderr, "Error: GoodSet variable definitions are missing\n");
        return false;
    }
    alltrax_var_value vals[2];
    alltrax_error err = alltrax_read_vars(ctrl, vars, 2, vals);
    if (err) {
        cli_error(ctrl, err, "reading settings validity");
        return false;
    }
    int64_t user = alltrax_var_raw_int64(&vals[0]);
    int64_t factory = alltrax_var_raw_int64(&vals[1]);
    if (user == 0)
        return true;
    fprintf(stderr,
        "Error: cannot save settings: %s (Var_GoodSet=0x%04X, Fact_GoodSet=0x%04X)\n",
        factory == 0 ? "only factory default values are present on the controller"
                     : "no settings loaded into the controller",
        (unsigned)user, (unsigned)factory);
    return false;
}

/* Read every save group and curve into XML. Any read or XML failure
 * prints an error and returns NULL. */
static char* build_xml(alltrax_controller* ctrl, size_t* total_vars,
    size_t* total_curves)
{
    char* result = NULL;
    xmlBufferPtr buf = NULL;
    size_t count = 0, curves = 0;

    /* Track group nodes so we can append curves to them */
    xmlNodePtr group_nodes[ALLTRAX_VARS_GROUP_COUNT] = {0};

    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr root = doc ? xmlNewNode(NULL, BAD_CAST "XCT") : NULL;
    if (!root) {
        report_no_memory();
        goto cleanup;
    }
    xmlDocSetRootElement(doc, root);

    for (size_t g = 0; g < NUM_SAVE_GROUPS; g++) {
        size_t var_count;
        const alltrax_var_def* defs =
            alltrax_get_var_defs(save_groups[g], &var_count);
        const char* gname = alltrax_var_group_name(save_groups[g]);
        if (!defs || var_count == 0) {
            fprintf(stderr, "Error: no variable definitions for %s\n", gname);
            goto cleanup;
        }

        /* Read all variables in this group */
        const alltrax_var_def** ptrs =
            calloc(var_count, sizeof(alltrax_var_def*));
        alltrax_var_value* vals =
            calloc(var_count, sizeof(alltrax_var_value));
        if (!ptrs || !vals) {
            free(ptrs);
            free(vals);
            report_no_memory();
            goto cleanup;
        }

        for (size_t i = 0; i < var_count; i++)
            ptrs[i] = &defs[i];

        alltrax_error err = alltrax_read_vars(ctrl, ptrs, var_count, vals);
        free(ptrs);
        if (err) {
            free(vals);
            char action[96];
            snprintf(action, sizeof(action), "reading %s settings", gname);
            cli_error(ctrl, err, action);
            goto cleanup;
        }

        char node_name[80];
        snprintf(node_name, sizeof(node_name), "Node-%s", gname);
        xmlNodePtr group = xmlNewChild(root, NULL, BAD_CAST node_name, NULL);
        bool ok = group != NULL;

        for (size_t i = 0; ok && i < var_count; i++) {
            char vbuf[32];
            const char* text = vbuf;
            if (defs[i].type == ALLTRAX_TYPE_STRING) {
                text = vals[i].raw.str;
            } else {
                int64_t raw = alltrax_var_raw_int64(&vals[i]);
                snprintf(vbuf, sizeof(vbuf), "%" PRId64, raw);
            }
            ok = xmlNewTextChild(group, NULL, BAD_CAST defs[i].name,
                                 BAD_CAST text) != NULL;
        }
        free(vals);
        if (!ok) {
            report_no_memory();
            goto cleanup;
        }
        group_nodes[save_groups[g]] = group;
        count += var_count;
    }

    /* Append curve data to existing group nodes */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        xmlNodePtr gnode = group_nodes[curve_xml_map[c].group];
        const alltrax_curve_def* def =
            alltrax_find_curve(curve_xml_map[c].curve_type);
        if (!gnode || !def) {
            fprintf(stderr, "Error: no save group or definition for curve %s\n",
                    curve_xml_map[c].curve_type);
            goto cleanup;
        }

        alltrax_curve_data data;
        alltrax_error err = alltrax_read_curve(ctrl, def, &data);
        if (err) {
            char action[96];
            snprintf(action, sizeof(action), "reading %s curve",
                     curve_xml_map[c].curve_type);
            cli_error(ctrl, err, action);
            goto cleanup;
        }

        char x_csv[128], y_csv[128];
        format_curve_csv(data.x, def->x_scale, x_csv, sizeof(x_csv));
        format_curve_csv(data.y, def->y_scale, y_csv, sizeof(y_csv));
        if (!xmlNewTextChild(gnode, NULL,
                BAD_CAST curve_xml_map[c].pointer_name, BAD_CAST x_csv) ||
            !xmlNewTextChild(gnode, NULL,
                BAD_CAST curve_xml_map[c].data_name, BAD_CAST y_csv)) {
            report_no_memory();
            goto cleanup;
        }
        curves++;
    }

    /* Serialize to string without XML declaration */
    buf = xmlBufferCreate();
    if (!buf || xmlNodeDump(buf, doc, root, 0, 1) < 0) {
        report_no_memory();
        goto cleanup;
    }

    size_t xml_len = (size_t)xmlBufferLength(buf);
    result = malloc(xml_len + 1);
    if (!result) {
        report_no_memory();
        goto cleanup;
    }
    memcpy(result, xmlBufferContent(buf), xml_len);
    result[xml_len] = '\0';
    *total_vars = count;
    *total_curves = curves;

cleanup:
    if (buf) xmlBufferFree(buf);
    if (doc) xmlFreeDoc(doc);
    return result;
}

/* ------------------------------------------------------------------ */
/* XML parsing (load)                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const alltrax_var_def* var;
    double display_value;   /* numeric and bool variables */
    char text[33];          /* string variables (at most 32 characters) */
} config_entry;

typedef struct {
    const alltrax_curve_def* def;
    double x[ALLTRAX_CURVE_POINTS];
    double y[ALLTRAX_CURVE_POINTS];
    bool has_x;
    bool has_y;
} config_curve_entry;

/* Parse comma-separated int16 values into display-unit doubles.
 * Returns true on success (exactly 16 values parsed). */
static bool parse_curve_csv(const char* csv, double scale, double* out)
{
    const char* p = csv;
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        char* endptr;
        errno = 0;
        long val = strtol(p, &endptr, 10);
        if (endptr == p || errno == ERANGE) return false;
        if (val < INT16_MIN || val > INT16_MAX) return false;
        out[i] = (double)(int16_t)val * scale;
        p = endptr;
        if (i < ALLTRAX_CURVE_POINTS - 1) {
            if (*p != ',') return false;
            p++;
        }
    }
    /* Allow trailing whitespace/NUL but not extra values */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0';
}

static config_entry* parse_xml(const char* xml, size_t xml_len,
    size_t* out_count, size_t* out_skipped,
    config_curve_entry* curves_out, size_t* curve_count)
{
    xmlDocPtr doc = xmlReadMemory(xml, (int)xml_len, NULL, NULL, 0);
    if (!doc) {
        fprintf(stderr, "Error: failed to parse config XML\n");
        return NULL;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root ||
        (xmlStrcmp(root->name, BAD_CAST "XCT") != 0 &&
         xmlStrcmp(root->name, BAD_CAST "ALL") != 0)) {
        fprintf(stderr, "Error: config file root element is not <XCT>\n");
        xmlFreeDoc(doc);
        return NULL;
    }

    size_t cap = 128;
    config_entry* entries = calloc(cap, sizeof(config_entry));
    if (!entries) { xmlFreeDoc(doc); return NULL; }

    size_t count = 0;
    *out_skipped = 0;
    *curve_count = 0;

    /* Initialize curve accumulation slots */
    for (size_t i = 0; i < NUM_CURVE_XML; i++) {
        curves_out[i].def = alltrax_find_curve(curve_xml_map[i].curve_type);
        curves_out[i].has_x = false;
        curves_out[i].has_y = false;
    }

    for (xmlNodePtr group = xmlFirstElementChild(root);
         group; group = xmlNextElementSibling(group)) {
        for (xmlNodePtr elem = xmlFirstElementChild(group);
             elem; elem = xmlNextElementSibling(elem)) {

            const char* name = (const char*)elem->name;
            xmlChar* xval = xmlNodeGetContent(elem);
            if (!xval) continue;
            const char* value = (const char*)xval;

            /* Check if this is a curve element */
            bool is_curve = false;
            for (size_t c = 0; c < NUM_CURVE_XML; c++) {
                if (!curves_out[c].def) continue;
                const alltrax_curve_def* cdef = curves_out[c].def;

                if (strcmp(name, curve_xml_map[c].pointer_name) == 0) {
                    if (parse_curve_csv(value, cdef->x_scale,
                                        curves_out[c].x)) {
                        curves_out[c].has_x = true;
                    } else {
                        fprintf(stderr, "Error: %s: bad curve data\n", name);
                        xmlFree(xval);
                        goto invalid_config;
                    }
                    is_curve = true;
                    break;
                }
                if (strcmp(name, curve_xml_map[c].data_name) == 0) {
                    if (parse_curve_csv(value, cdef->y_scale,
                                        curves_out[c].y)) {
                        curves_out[c].has_y = true;
                    } else {
                        fprintf(stderr, "Error: %s: bad curve data\n", name);
                        xmlFree(xval);
                        goto invalid_config;
                    }
                    is_curve = true;
                    break;
                }
            }

            if (is_curve) {
                xmlFree(xval);
                continue;
            }

            const alltrax_var_def* var = alltrax_find_var(name);
            if (!var) {
                fprintf(stderr, "  Skipping unknown variable: %s\n", name);
                (*out_skipped)++;
                xmlFree(xval);
                continue;
            }
            /* Select restorable fields from a backup that also contains
             * read-only metadata. The library enforces write permissions. */
            if (!var->writable) {
                xmlFree(xval);
                continue;
            }

            if (count >= cap) {
                cap *= 2;
                config_entry* tmp =
                    realloc(entries, cap * sizeof(config_entry));
                if (!tmp) { xmlFree(xval); goto invalid_config; }
                entries = tmp;
            }

            /* An invalid string fails the load. */
            if (var->type == ALLTRAX_TYPE_STRING) {
                if (alltrax_validate_var_string(var, value)) {
                    fprintf(stderr, "Error: %s: invalid text (printable "
                            "ASCII, at most %u characters)\n",
                            name, (unsigned)var->size);
                    xmlFree(xval);
                    goto invalid_config;
                }
                entries[count] = (config_entry){ .var = var };
                snprintf(entries[count].text, sizeof(entries[count].text),
                         "%s", value);
                count++;
                xmlFree(xval);
                continue;
            }

            char* endptr;
            long long raw = strtoll(value, &endptr, 10);
            if (*endptr != '\0' && *endptr != ',') {
                fprintf(stderr, "  Skipping %s: bad value \"%s\"\n",
                        name, value);
                (*out_skipped)++;
                xmlFree(xval);
                continue;
            }
            xmlFree(xval);

            entries[count] = (config_entry){
                .var = var,
                .display_value = ((double)raw - var->offset) * var->scale,
            };
            count++;
        }
    }

    /* Validate every imported curve before opening or applying any settings. */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        if (!curves_out[c].has_x && !curves_out[c].has_y)
            continue;
        if (!curves_out[c].has_x || !curves_out[c].has_y) {
            fprintf(stderr, "Error: incomplete %s curve (X and Y required)\n",
                    curves_out[c].def->name);
            goto invalid_config;
        }
        alltrax_curve_data data = { .def = curves_out[c].def };
        memcpy(data.x, curves_out[c].x, sizeof(data.x));
        memcpy(data.y, curves_out[c].y, sizeof(data.y));
        if (alltrax_validate_curve(data.def, &data)) {
            fprintf(stderr, "Error: invalid %s curve in config\n", data.def->name);
            goto invalid_config;
        }
        (*curve_count)++;
    }

    xmlFreeDoc(doc);
    *out_count = count;
    return entries;

invalid_config:
    free(entries);
    xmlFreeDoc(doc);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* alltrax config save <file>                                          */
/* ------------------------------------------------------------------ */

static int do_save(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "config save", CLI_OPT_NO_CRYPT, 1, 1,
                       &flags) < 0) {
        fprintf(stderr,
            "Usage: alltrax config save [flags] <file>\n"
            "\n"
            "Flags:\n"
            "  --no-crypt       Write plain XML (not Toolkit-compatible)\n");
        return 1;
    }
    const char* filepath = argv[1];

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    /* Every read completes before the destination is opened. */
    size_t total_vars = 0, total_curves = 0;
    char* xml = saved_settings_valid(ctrl)
        ? build_xml(ctrl, &total_vars, &total_curves) : NULL;
    alltrax_close(ctrl);

    if (!xml) {
        fprintf(stderr, "Error: config save: no file written\n");
        return 1;
    }

    /* Encrypt before opening the destination so a failure leaves it intact. */
    const void* data = xml;
    size_t data_len = strlen(xml);
    unsigned char* cipher = NULL;
    if (!flags.no_crypt) {
        cipher = des3_encrypt(xml, data_len, &data_len);
        if (!cipher) {
            free(xml);
            fprintf(stderr, "Error: 3DES encryption failed; no file written\n");
            return 1;
        }
        data = cipher;
    }

    FILE* f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create file %s: %s\n",
                filepath, strerror(errno));
        free(xml);
        free(cipher);
        return 1;
    }

    /* Buffered write errors such as a full disk surface at fclose(). */
    struct stat st;
    bool regular = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode);
    bool failed = fwrite(data, 1, data_len, f) != data_len;
    int write_errno = failed ? errno : 0;
    if (fclose(f) != 0 && !failed) {
        failed = true;
        write_errno = errno;
    }
    free(xml);
    free(cipher);
    if (failed) {
        /* Only a regular file is removed; device and pipe paths are kept. */
        bool removed = regular && remove(filepath) == 0;
        fprintf(stderr, "Error: writing %s failed: %s%s\n", filepath,
                write_errno ? strerror(write_errno) : "short write",
                removed ? "; partial file removed" : "");
        return 1;
    }

    printf("Saved %zu variables + %zu curves to %s\n",
           total_vars, total_curves, filepath);
    return 0;
}

/* ------------------------------------------------------------------ */
/* alltrax config load [flags] <file>                                  */
/* ------------------------------------------------------------------ */

static int do_load(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "config load",
                       CLI_OPT_NO_CRYPT | CLI_OPT_NO_CAL | CLI_OPT_NO_VERIFY |
                       CLI_OPT_NO_GOODSET | CLI_OPT_NO_FW_VERSION |
                       CLI_OPT_RESET, 1, 1, &flags) < 0) {
        fprintf(stderr,
            "Usage: alltrax config load [flags] <file>\n"
            "\n"
            "Flags:\n"
            "  --no-crypt       Read plain XML (not encrypted)\n"
            "  --no-cal         Skip CAL/RUN mode bracket\n"
            "  --no-verify      Skip read-back verification\n"
            "  --no-goodset     Skip GoodSet pre-check\n"
            "  --no-fw-version  Allow unvalidated firmware within V0.001-V5.999\n"
            "  --reset          Reboot controller after write\n");
        return 1;
    }
    const char* filepath = argv[1];

    /* Read file */
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open file: %s\n", filepath);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 1024 * 1024) {
        fprintf(stderr, "Error: invalid file size\n");
        fclose(f);
        return 1;
    }

    unsigned char* file_data = malloc((size_t)file_size);
    if (!file_data) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(f);
        return 1;
    }

    size_t nread = fread(file_data, 1, (size_t)file_size, f);
    fclose(f);

    if (nread != (size_t)file_size) {
        fprintf(stderr, "Error: short read from %s\n", filepath);
        free(file_data);
        return 1;
    }

    /* Decrypt if needed */
    size_t xml_len;
    char* xml;

    if (flags.no_crypt) {
        xml = (char*)file_data;
        xml_len = nread;
    } else {
        xml = des3_decrypt(file_data, nread, &xml_len);
        free(file_data);
        if (!xml) {
            fprintf(stderr,
                "Error: 3DES decryption failed (wrong key or corrupt file)\n");
            return 1;
        }
    }

    /* Parse XML into writable variable list + curves */
    size_t count, skipped, n_curves;
    config_curve_entry curves[NUM_CURVE_XML];
    config_entry* entries = parse_xml(xml, xml_len, &count, &skipped,
                                      curves, &n_curves);
    free(xml);

    if (!entries)
        return 1;

    if (count == 0 && n_curves == 0) {
        fprintf(stderr, "No writable variables found in config file\n");
        free(entries);
        return 1;
    }

    printf("Loaded %zu writable variables + %zu curves from %s",
           count, n_curves, filepath);
    if (skipped > 0)
        printf(" (%zu skipped)", skipped);
    printf("\n");

    /* Settings, strings, and curves go to the device in one batch: one
     * CAL/RUN bracket and one settings page cycle. */
    alltrax_curve_data curve_data[NUM_CURVE_XML];
    alltrax_assignment* items = calloc(count + n_curves, sizeof(*items));
    if (!items) {
        fprintf(stderr, "Error: out of memory\n");
        free(entries);
        return 1;
    }
    size_t n_items = 0;
    for (size_t i = 0; i < count; i++) {
        const alltrax_var_def* var = entries[i].var;
        items[n_items++] = var->type == ALLTRAX_TYPE_STRING
            ? (alltrax_assignment){ .var = var, .text = entries[i].text }
            : (alltrax_assignment){ .var = var, .number = entries[i].display_value };
    }
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        if (!curves[c].has_x || !curves[c].has_y || !curves[c].def)
            continue;
        curve_data[c].def = curves[c].def;
        memcpy(curve_data[c].x, curves[c].x, sizeof(curve_data[c].x));
        memcpy(curve_data[c].y, curves[c].y, sizeof(curve_data[c].y));
        items[n_items++] = (alltrax_assignment){
            .curve = curves[c].def, .curve_data = &curve_data[c],
        };
    }

    /* Open device with write access */
    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) {
        free(items);
        free(entries);
        return 1;
    }

    int ret = 0;
    alltrax_write_opts write_opts = {
        .skip_cal = flags.no_cal,
        .skip_verify = flags.no_verify,
        .skip_goodset = flags.no_goodset,
        .skip_fw_check = flags.no_fw_version,
    };

    err = alltrax_write_assignments(ctrl, items, n_items, &write_opts);
    if (err) {
        cli_error(ctrl, err, "writing settings");
        ret = 1;
        goto done;
    }
    for (size_t i = 0; i < n_items; i++) {
        if (items[i].curve)
            printf("  Wrote curve: %s (%d points)\n",
                   items[i].curve->name, ALLTRAX_CURVE_POINTS);
        else
            printf("  Wrote %s\n", items[i].var->name);
    }

    if (flags.reset) {
        err = alltrax_reset_device(ctrl, &write_opts);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            ret = 1;
            goto done;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

done:
    free(items);
    free(entries);
    alltrax_close(ctrl);
    return ret;
}

/* ------------------------------------------------------------------ */
/* alltrax config <save|load>                                          */
/* ------------------------------------------------------------------ */

int cmd_config(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax config <save|load> [flags] <file>\n"
            "\n"
            "  save [flags] <file>  Read settings, write config file\n"
            "  load [flags] <file>  Load config file, write settings to device\n"
            "\n"
            "Common flags:\n"
            "  --no-crypt       Plain XML instead of encrypted (not Toolkit-compatible)\n"
            "\n"
            "Load flags:\n"
            "  --no-cal         Skip CAL/RUN mode bracket\n"
            "  --no-verify      Skip read-back verification\n"
            "  --no-goodset     Skip GoodSet pre-check\n"
            "  --no-fw-version  Allow unvalidated firmware within V0.001-V5.999\n"
            "  --reset          Reboot controller after write\n");
        return 1;
    }

    const char* subcmd = argv[1];

    if (strcmp(subcmd, "save") == 0)
        return do_save(argc - 1, argv + 1);
    if (strcmp(subcmd, "load") == 0)
        return do_load(argc - 1, argv + 1);

    fprintf(stderr, "Unknown config subcommand: %s\n", subcmd);
    return 1;
}
