/*
 * cmd_config.c — alltrax config save/load: Backup and restore settings
 *
 * Reads/writes encrypted XML config files compatible with the Windows
 * Alltrax Toolkit. Uses 3DES-CBC encryption via OpenSSL libcrypto
 * and libxml2 for XML generation/parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
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

static char* build_xml(alltrax_controller* ctrl, size_t* total_vars,
    size_t* total_curves)
{
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    if (!doc) return NULL;

    xmlNodePtr root = xmlNewNode(NULL, BAD_CAST "XCT");
    xmlDocSetRootElement(doc, root);

    size_t count = 0;

    /* Track group nodes so we can append curves to them */
    xmlNodePtr group_nodes[ALLTRAX_VARS_GROUP_COUNT];
    memset(group_nodes, 0, sizeof(group_nodes));

    for (size_t g = 0; g < NUM_SAVE_GROUPS; g++) {
        size_t var_count;
        const alltrax_var_def* defs =
            alltrax_get_var_defs(save_groups[g], &var_count);
        if (!defs || var_count == 0) continue;

        const char* gname = alltrax_var_group_name(save_groups[g]);

        /* Read all variables in this group */
        const alltrax_var_def** ptrs =
            calloc(var_count, sizeof(alltrax_var_def*));
        alltrax_var_value* vals =
            calloc(var_count, sizeof(alltrax_var_value));
        if (!ptrs || !vals) {
            free(ptrs); free(vals); xmlFreeDoc(doc);
            return NULL;
        }

        for (size_t i = 0; i < var_count; i++)
            ptrs[i] = &defs[i];

        alltrax_error err = alltrax_read_vars(ctrl, ptrs, var_count, vals);
        free(ptrs);
        if (err) {
            fprintf(stderr, "Warning: failed to read %s: %s\n",
                    gname, alltrax_strerror(err));
            free(vals);
            continue;
        }

        char node_name[80];
        snprintf(node_name, sizeof(node_name), "Node-%s", gname);
        xmlNodePtr group = xmlNewChild(root, NULL, BAD_CAST node_name, NULL);
        group_nodes[save_groups[g]] = group;

        for (size_t i = 0; i < var_count; i++) {
            char vbuf[32];
            if (defs[i].type == ALLTRAX_TYPE_STRING) {
                xmlNewTextChild(group, NULL,
                    BAD_CAST defs[i].name, BAD_CAST vals[i].raw.str);
            } else {
                int64_t raw = alltrax_var_raw_int64(&vals[i]);
                snprintf(vbuf, sizeof(vbuf), "%" PRId64, raw);
                xmlNewTextChild(group, NULL,
                    BAD_CAST defs[i].name, BAD_CAST vbuf);
            }
            count++;
        }

        free(vals);
    }

    /* Append curve data to existing group nodes */
    size_t curves = 0;
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        xmlNodePtr gnode = group_nodes[curve_xml_map[c].group];
        if (!gnode) continue;

        const alltrax_curve_def* def =
            alltrax_find_curve(curve_xml_map[c].curve_type);
        if (!def) continue;

        alltrax_curve_data data;
        alltrax_error err = alltrax_read_curve(ctrl, def, &data);
        if (err) {
            fprintf(stderr, "Warning: failed to read curve %s: %s\n",
                    curve_xml_map[c].curve_type, alltrax_strerror(err));
            continue;
        }

        char csv[128];

        format_curve_csv(data.x, def->x_scale, csv, sizeof(csv));
        xmlNewTextChild(gnode, NULL,
            BAD_CAST curve_xml_map[c].pointer_name, BAD_CAST csv);

        format_curve_csv(data.y, def->y_scale, csv, sizeof(csv));
        xmlNewTextChild(gnode, NULL,
            BAD_CAST curve_xml_map[c].data_name, BAD_CAST csv);

        curves++;
    }

    /* Serialize to string without XML declaration */
    xmlBufferPtr buf = xmlBufferCreate();
    if (!buf) { xmlFreeDoc(doc); return NULL; }

    xmlNodeDump(buf, doc, root, 0, 1);

    size_t xml_len = (size_t)xmlBufferLength(buf);
    char* result = malloc(xml_len + 1);
    if (result) {
        memcpy(result, xmlBufferContent(buf), xml_len);
        result[xml_len] = '\0';
    }

    xmlBufferFree(buf);
    xmlFreeDoc(doc);

    *total_vars = count;
    *total_curves = curves;
    return result;
}

/* ------------------------------------------------------------------ */
/* XML parsing (load)                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const alltrax_var_def* var;
    double display_value;
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
        long val = strtol(p, &endptr, 10);
        if (endptr == p) return false;
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
                        fprintf(stderr,
                            "  Skipping %s: bad curve data\n", name);
                        (*out_skipped)++;
                    }
                    is_curve = true;
                    break;
                }
                if (strcmp(name, curve_xml_map[c].data_name) == 0) {
                    if (parse_curve_csv(value, cdef->y_scale,
                                        curves_out[c].y)) {
                        curves_out[c].has_y = true;
                    } else {
                        fprintf(stderr,
                            "  Skipping %s: bad curve data\n", name);
                        (*out_skipped)++;
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
            if (!var->writable || var->type == ALLTRAX_TYPE_STRING) {
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

            double display = ((double)raw - var->offset) * var->scale;

            if (count >= cap) {
                cap *= 2;
                config_entry* tmp =
                    realloc(entries, cap * sizeof(config_entry));
                if (!tmp) { free(entries); xmlFreeDoc(doc); return NULL; }
                entries = tmp;
            }

            entries[count].var = var;
            entries[count].display_value = display;
            count++;
        }
    }

    /* Count complete curves (both X and Y present) */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        if (curves_out[c].has_x && curves_out[c].has_y)
            (*curve_count)++;
    }

    xmlFreeDoc(doc);
    *out_count = count;
    return entries;
}

/* ------------------------------------------------------------------ */
/* alltrax config save <file>                                          */
/* ------------------------------------------------------------------ */

static int do_save(int argc, char** argv)
{
    cli_flags flags;
    int first_arg = cli_parse_flags(argc, argv, &flags);

    if (first_arg >= argc) {
        fprintf(stderr,
            "Usage: alltrax config save [flags] <file>\n"
            "\n"
            "Flags:\n"
            "  --no-crypt       Write plain XML (not Toolkit-compatible)\n");
        return 1;
    }
    const char* filepath = argv[first_arg];

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    size_t total_vars = 0, total_curves = 0;
    char* xml = build_xml(ctrl, &total_vars, &total_curves);
    alltrax_close(ctrl);

    if (!xml) {
        fprintf(stderr, "Error: failed to build config XML\n");
        return 1;
    }

    FILE* f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create file: %s\n", filepath);
        free(xml);
        return 1;
    }

    size_t written;
    if (flags.no_crypt) {
        size_t xml_len = strlen(xml);
        written = fwrite(xml, 1, xml_len, f);
        fclose(f);
        free(xml);
        if (written != xml_len) {
            fprintf(stderr, "Error: short write to %s\n", filepath);
            return 1;
        }
    } else {
        size_t cipher_len;
        unsigned char* cipher = des3_encrypt(xml, strlen(xml), &cipher_len);
        free(xml);
        if (!cipher) {
            fclose(f);
            fprintf(stderr, "Error: 3DES encryption failed\n");
            return 1;
        }
        written = fwrite(cipher, 1, cipher_len, f);
        fclose(f);
        free(cipher);
        if (written != cipher_len) {
            fprintf(stderr, "Error: short write to %s\n", filepath);
            return 1;
        }
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
    int first_arg = cli_parse_flags(argc, argv, &flags);

    if (first_arg >= argc) {
        fprintf(stderr,
            "Usage: alltrax config load [flags] <file>\n"
            "\n"
            "Flags:\n"
            "  --no-crypt       Read plain XML (not encrypted)\n"
            "  --no-cal         Skip CAL/RUN mode bracket\n"
            "  --no-verify      Skip read-back verification\n"
            "  --no-goodset     Skip GoodSet pre-check\n"
            "  --no-fw-version  Skip firmware version check\n"
            "  --reset          Reboot controller after write\n");
        return 1;
    }
    const char* filepath = argv[first_arg];

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

    /* Open device with write access */
    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) {
        free(entries);
        return 1;
    }

    /* Build arrays for alltrax_write_vars */
    const alltrax_var_def** var_ptrs = NULL;
    double* values = NULL;
    int ret = 0;

    if (count > 0) {
        var_ptrs = calloc(count, sizeof(alltrax_var_def*));
        values = calloc(count, sizeof(double));
        if (!var_ptrs || !values) {
            fprintf(stderr, "Error: out of memory\n");
            ret = 1;
            goto done;
        }
        for (size_t i = 0; i < count; i++) {
            var_ptrs[i] = entries[i].var;
            values[i] = entries[i].display_value;
        }
    }

    alltrax_write_opts write_opts = {
        .skip_cal = flags.no_cal,
        .skip_verify = flags.no_verify,
        .skip_goodset = flags.no_goodset,
        .skip_fw_check = flags.no_fw_version,
    };

    if (count > 0) {
        err = alltrax_write_vars(ctrl, var_ptrs, values, count, &write_opts);
        if (err) {
            cli_error(ctrl, err, "writing variables");
            ret = 1;
            goto done;
        }

        for (size_t i = 0; i < count; i++)
            printf("  Wrote %s\n", entries[i].var->name);
    }

    /* Write curves */
    for (size_t c = 0; c < NUM_CURVE_XML; c++) {
        if (!curves[c].has_x || !curves[c].has_y || !curves[c].def)
            continue;

        alltrax_curve_data cdata;
        cdata.def = curves[c].def;
        memcpy(cdata.x, curves[c].x, sizeof(cdata.x));
        memcpy(cdata.y, curves[c].y, sizeof(cdata.y));

        err = alltrax_write_curve(ctrl, curves[c].def, &cdata, &write_opts);
        if (err) {
            cli_error(ctrl, err, "writing curve");
            ret = 1;
            goto done;
        }
        printf("  Wrote curve: %s (%d points)\n",
               curves[c].def->name, ALLTRAX_CURVE_POINTS);
    }

    if (flags.reset) {
        err = alltrax_reset_device(ctrl);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            ret = 1;
            goto done;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

done:
    free(var_ptrs);
    free(values);
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
            "  --no-fw-version  Skip firmware version check\n"
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
