/*
 * assembler.c
 *
 * This module implements a two‑pass assembler for the custom
 * instruction set described in the project specification.  It reads
 * preprocessed source lines, builds a symbol table and machine code
 * image and writes the resulting object, entry and external files.
 *
 * The assembler performs the following steps:
 *
 * 1. First pass: scan through all lines, handling directives that
 *    allocate data (.data, .string, .mat) and creating symbols for
 *    labels.  It encodes the main word of each instruction and any
 *    immediate or register operands, leaving unresolved references to
 *    labels for the second pass.  It counts the number of words
 *    generated in the code and data sections (IC and DC).
 *
 * 2. After the first pass: compute the total length of the code
 *    section (ICF) and update all data symbol addresses by adding
 *    ICF.  All symbol addresses are treated as offsets relative to
 *    the start of the program; later we add 100 when writing the
 *    final object file.
 *
 * 3. Second pass: rescan the source lines, processing .entry
 *    directives to mark entry symbols and resolving all references to
 *    symbols in instruction operands.  Unresolved references are
 *    patched into the code image and external references are recorded.
 *
 * 4. Output generation: if no errors occurred, write the .ob file
 *    containing the lengths of code and data sections followed by the
 *    encoded machine words with their absolute addresses (offset
 *    starting at 100).  Optionally write .ent and .ext files for
 *    entry and external symbols.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "assembler.h"

/* Data structure representing a machine word (10 bits).  The value
 * field holds the 10 least significant bits of the encoded word. */
typedef struct {
    unsigned int value; /* only bottom 10 bits are used */
} MachineWord;

/* Linked list of unresolved references.  Each unresolved reference
 * records the index in the code image where the address must be
 * patched and the name of the symbol being referenced.  The orig_line
 * field is used for error reporting. */
typedef struct Unresolved {
    size_t index; /* code_image index */
    char name[MAX_LABEL_LEN + 1];
    int orig_line;
    struct Unresolved *next;
} Unresolved;

/* Linked list of external references.  Each entry records an
 * occurrence of an external symbol and the absolute address of the
 * word referencing it. */
typedef struct ExtRef {
    char name[MAX_LABEL_LEN + 1];
    unsigned int address; /* absolute memory address */
    struct ExtRef *next;
} ExtRef;

/* Local function prototypes */
static int is_blank_or_comment(const char *s);
static int parse_label(const char *line, char *label, size_t *pos, int *error);
static int is_valid_label(const char *label);
static int parse_number(const char *s, long *out_value);
static int parse_operands(const char *s, char operands[2][MAX_LABEL_LEN + 32], int *operand_count);
static int identify_addr_mode(const char *operand, int *mode,
                              long *imm_value, char *label, int *reg1, int *reg2);
static void ensure_code_capacity(MachineWord **arr, size_t *cap, size_t required);
static void ensure_data_capacity(MachineWord **arr, size_t *cap, size_t required);

/* Maximum number of machine words that can be loaded into memory.  The
 * specification states that the fictitious machine contains 256 words
 * of memory, so a program that generates more than this total size
 * (code plus data) should be rejected. */
#define MAX_MEM_WORDS 256

/* Unique base‑4 conversion table */
static const char ub4_table[4] = { 'a', 'b', 'c', 'd' };

/* Convert 10‑bit value to unique base‑4 string of length 5. */
void to_unique_base4(unsigned int value, char out[6]) {
    /* Use only bottom 10 bits */
    value &= 0x3FF;
    unsigned int divisor = 1;
    /* divisor will be 4^4 = 256 */
    for (int i = 0; i < 4; i++) divisor *= 4;
    for (int i = 0; i < 5; i++) {
        unsigned int digit = (value / divisor) & 0x3;
        out[i] = ub4_table[digit];
        value %= divisor;
        divisor /= 4;
    }
    out[5] = '\0';
}

/* Convert address (0‑255) to unique base‑4 string. */
void address_to_unique_base4(unsigned int address, char *out) {
    /* Compute digits; we allow leading zeros trimmed except one digit */
    char buf[5];
    int i = 0;
    do {
        buf[i++] = ub4_table[address & 0x3];
        address >>= 2;
    } while (address != 0 && i < 4);
    /* Reverse digits */
    int pos = 0;
    while (i > 0) {
        out[pos++] = buf[--i];
    }
    out[pos] = '\0';
}

/* Check if a line is blank or a comment */
static int is_blank_or_comment(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return (*s == '\0' || *s == ';');
}

/* Parse a label at the beginning of the line.  On success store the
 * label in `label` and update `pos` to point after the colon.  Returns
 * 1 if a label was found or 0 otherwise.  Sets *error non‑zero on
 * syntax error. */
static int parse_label(const char *line, char *label, size_t *pos, int *error) {
    size_t start = 0;
    /* Skip leading whitespace */
    while (line[start] && isspace((unsigned char)line[start])) start++;
    /* Read up to colon or whitespace */
    size_t p = start;
    while (line[p] && !isspace((unsigned char)line[p]) && line[p] != ':') p++;
    if (line[p] == ':') {
        /* Extract label */
        size_t len = p - start;
        if (len > MAX_LABEL_LEN) {
            len = MAX_LABEL_LEN;
        }
        strncpy(label, line + start, len);
        label[len] = '\0';
        if (!is_valid_label(label)) {
            *error = 1;
            return 0;
        }
        /* Move pos after colon */
        p++; /* skip ':' */
        *pos = p;
        return 1;
    }
    return 0;
}

/* Validate that a label conforms to the rules: begins with an
 * alphabetic character, followed by up to 30 alphanumeric characters,
 * and does not start with a period. */
static int is_valid_label(const char *label) {
    if (!label || !isalpha((unsigned char)label[0])) {
        return 0;
    }
    size_t len = strlen(label);
    if (len > MAX_LABEL_LEN) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        /* Allow underscore in addition to alphanumeric characters */
        if (!isalnum((unsigned char)label[i]) && label[i] != '_') {
            return 0;
        }
    }
    return 1;
}

/* Parse an integer number in decimal.  Accepts optional +/‑ sign and
 * digits.  Returns 1 on success and stores the value in out_value,
 * else 0 on failure. */
static int parse_number(const char *s, long *out_value) {
    char *end;
    long val = strtol(s, &end, 10);
    if (end == s || (*end != '\0' && !isspace((unsigned char)*end) && *end != ',' && *end != ']' && *end != '[')) {
        return 0;
    }
    *out_value = val;
    return 1;
}

/* Parse operands separated by comma.  Stores up to 2 operands as
 * strings in `operands` array and writes the count to operand_count.
 * Leading and trailing whitespace around operands is removed.  Returns
 * 1 on success, 0 on syntax error. */
static int parse_operands(const char *s, char operands[2][MAX_LABEL_LEN + 32], int *operand_count) {
    *operand_count = 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') {
        *operand_count = 0;
        return 1;
    }
    int count = 0;
    while (*s) {
        /* Skip leading whitespace */
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '\0') break;
        /* Read operand until comma or end */
        size_t k = 0;
        int bracket_depth = 0;
        while (*s && !(bracket_depth == 0 && (*s == ',')) ) {
            /* track brackets to allow commas within matrix indexes */
            if (*s == '[') bracket_depth++;
            else if (*s == ']') {
                if (bracket_depth > 0) bracket_depth--;
            }
            if (k < MAX_LABEL_LEN + 31) {
                operands[count][k++] = *s;
            }
            s++;
        }
        /* trim trailing whitespace */
        while (k > 0 && isspace((unsigned char)operands[count][k - 1])) {
            k--;
        }
        operands[count][k] = '\0';
        count++;
        if (count > 2) {
            return 0;
        }
        if (*s == ',') {
            s++; /* skip comma */
        }
    }
    *operand_count = count;
    return 1;
}

/* Identify addressing mode of an operand.  Returns 1 on success and
 * sets mode (0..3) accordingly.  For immediate mode the value is
 * stored in *imm_value.  For direct and matrix modes the label name
 * (without indices) is copied into *label.  For matrix mode, reg1 and
 * reg2 hold the row and column register numbers (0..7).  For register
 * mode, reg1 holds the register number.  On failure returns 0. */
static int identify_addr_mode(const char *operand, int *mode,
                              long *imm_value, char *label, int *reg1, int *reg2) {
    /* Immediate: starts with '#' followed by number */
    if (operand[0] == '#') {
        long val;
        if (!parse_number(operand + 1, &val)) {
            return 0;
        }
        *mode = 0;
        *imm_value = val;
        return 1;
    }
    /* Matrix: contains '[' and ']' twice: label[rX][rY] */
    const char *open1 = strchr(operand, '[');
    if (open1) {
        const char *close1 = strchr(open1, ']');
        if (!close1) return 0;
        const char *open2 = strchr(close1 + 1, '[');
        const char *close2 = open2 ? strchr(open2, ']') : NULL;
        if (!open2 || !close2) return 0;
        /* extract label name before first '[' */
        size_t len = open1 - operand;
        if (len == 0 || len > MAX_LABEL_LEN) return 0;
        strncpy(label, operand, len);
        label[len] = '\0';
        if (!is_valid_label(label)) return 0;
        /* parse reg1 inside first [] */
        char regname1[6];
        size_t rlen = close1 - (open1 + 1);
        if (rlen >= sizeof(regname1)) return 0;
        strncpy(regname1, open1 + 1, rlen);
        regname1[rlen] = '\0';
        /* parse reg2 inside second [] */
        char regname2[6];
        size_t rlen2 = close2 - (open2 + 1);
        if (rlen2 >= sizeof(regname2)) return 0;
        strncpy(regname2, open2 + 1, rlen2);
        regname2[rlen2] = '\0';
        /* check extra text after second ] */
        const char *after = close2 + 1;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after != '\0') return 0;
        /* convert reg names */
        if (regname1[0] != 'r' || !isdigit((unsigned char)regname1[1]) || regname1[2] != '\0') return 0;
        if (regname2[0] != 'r' || !isdigit((unsigned char)regname2[1]) || regname2[2] != '\0') return 0;
        int r1 = regname1[1] - '0';
        int r2 = regname2[1] - '0';
        if (r1 < 0 || r1 > 7 || r2 < 0 || r2 > 7) return 0;
        *reg1 = r1;
        *reg2 = r2;
        *mode = 2;
        return 1;
    }
    /* Register direct: rN */
    if (operand[0] == 'r' && operand[1] && !operand[2]) {
        if (isdigit((unsigned char)operand[1])) {
            int r = operand[1] - '0';
            if (r >= 0 && r <= 7) {
                *mode = 3;
                *reg1 = r;
                return 1;
            }
        }
        return 0;
    }
    /* Direct label */
    /* trim whitespace (should already be trimmed) */
    if (!is_valid_label(operand)) {
        return 0;
    }
    strcpy(label, operand);
    *mode = 1;
    return 1;
}

/* Ensure capacity for code image */
static void ensure_code_capacity(MachineWord **arr, size_t *cap, size_t required) {
    if (*cap < required) {
        size_t newcap = *cap ? *cap * 2 : 64;
        if (newcap < required) newcap = required;
        MachineWord *tmp = (MachineWord *)realloc(*arr, newcap * sizeof(MachineWord));
        if (!tmp) {
            fprintf(stderr, "Error: memory allocation failure for code image\n");
            exit(EXIT_FAILURE);
        }
        *arr = tmp;
        *cap = newcap;
    }
}

/* Ensure capacity for data image */
static void ensure_data_capacity(MachineWord **arr, size_t *cap, size_t required) {
    if (*cap < required) {
        size_t newcap = *cap ? *cap * 2 : 64;
        if (newcap < required) newcap = required;
        MachineWord *tmp = (MachineWord *)realloc(*arr, newcap * sizeof(MachineWord));
        if (!tmp) {
            fprintf(stderr, "Error: memory allocation failure for data image\n");
            exit(EXIT_FAILURE);
        }
        *arr = tmp;
        *cap = newcap;
    }
}

/* Map opcode string to its numeric code (0‑15).  Returns -1 if
 * unknown. */
static int opcode_of(const char *op) {
    /* The order must match the enumeration from the spec */
    static const char *ops[] = {
        "mov", "cmp", "add", "sub", "not", "clr",
        "lea", "inc", "dec", "jmp", "bne", "red",
        "prn", "jsr", "rts", "stop", NULL
    };
    for (int i = 0; ops[i] != NULL; i++) {
        if (strcmp(ops[i], op) == 0) return i;
    }
    return -1;
}

/* Allowed addressing modes per opcode (source and dest) according to spec.
 * Each row corresponds to an opcode; columns 0 and 1 are bitmasks of
 * legal modes for source (index 0) and dest (index 1).  Bit 0
 * corresponds to immediate, bit 1 to direct, bit 2 to matrix, bit 3
 * to register.  An opcode that has no source operand has 0 for the
 * source mask. */
static const unsigned char legal_addr_modes[16][2] = {
    /* mov */ {0x0F, 0x0E}, /* src: any, dest: no immediate */
    /* cmp */ {0x0F, 0x0F}, /* any */
    /* add */ {0x0F, 0x0E},
    /* sub */ {0x0F, 0x0E},
    /* not */ {0x00, 0x0E},
    /* clr */ {0x00, 0x0E},
    /* lea */ {0x06, 0x0E}, /* src: direct/matrix, dest: no immediate */
    /* inc */ {0x00, 0x0E},
    /* dec */ {0x00, 0x0E},
    /* jmp */ {0x00, 0x0E},
    /* bne */ {0x00, 0x0E},
    /* red */ {0x00, 0x0E},
    /* prn */ {0x00, 0x0F}, /* dest: allow immediate for prn */
    /* jsr */ {0x00, 0x0E},
    /* rts */ {0x00, 0x00},
    /* stop*/ {0x00, 0x00}
};

/* Determine expected operand count for opcode */
static int expected_operands(int opcode) {
    switch (opcode) {
        case 0: case 1: case 2: case 3: case 6:
            return 2;
        case 4: case 5: case 7: case 8:
        case 9: case 10: case 11: case 12: case 13:
            return 1;
        case 14: case 15:
            return 0;
        default:
            return -1;
    }
}

/* Main assembly function */
int assemble_file(const char *basename, PreprocessedLine *lines, size_t count) {
    SymbolTable symtab;
    st_init(&symtab);
    /* dynamic arrays for code and data images */
    MachineWord *code_image = NULL;
    size_t code_cap = 0;
    size_t IC = 0;
    MachineWord *data_image = NULL;
    size_t data_cap = 0;
    size_t DC = 0;
    /* Unresolved references list */
    Unresolved *unresolved_head = NULL;
    Unresolved *unresolved_tail = NULL;
    /* External references list */
    ExtRef *ext_head = NULL;
    ExtRef *ext_tail = NULL;
    int error_count = 0;

    /* First pass */
    for (size_t i = 0; i < count; i++) {
        const char *text = lines[i].text;
        int orig_line = lines[i].orig_line;
        /* Skip blank and comment lines */
        if (is_blank_or_comment(text)) {
            continue;
        }
        /* Check line length (excluding newline) */
        if (strlen(text) > 80) {
            fprintf(stdout, "Error on line %d: line too long\n", orig_line);
            error_count++;
            continue;
        }
        /* Detect and parse label */
        char label[MAX_LABEL_LEN + 1];
        label[0] = '\0';
        size_t pos = 0;
        int label_error = 0;
        int has_label = parse_label(text, label, &pos, &label_error);
        if (label_error) {
            fprintf(stdout, "Error on line %d: invalid label\n", orig_line);
            error_count++;
            continue;
        }
        /* After label, skip whitespace */
        const char *p = text + pos;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') {
            fprintf(stdout, "Error on line %d: label without statement\n", orig_line);
            error_count++;
            continue;
        }
        /* Determine directive or instruction */
        if (*p == '.') {
            /* directive */
            /* read directive name */
            const char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t dlen = p - start;
            char directive[16];
            if (dlen >= sizeof(directive)) dlen = sizeof(directive) - 1;
            strncpy(directive, start, dlen);
            directive[dlen] = '\0';
            /* skip whitespace */
            while (*p && isspace((unsigned char)*p)) p++;
            if (strcmp(directive, ".data") == 0) {
                /* Handle .data */
                if (has_label) {
                    if (st_add(&symtab, label, (int)DC, ATTR_DATA) != 0) {
                        fprintf(stdout, "Error on line %d: duplicate symbol '%s'\n", orig_line, label);
                        error_count++;
                        continue;
                    }
                }
                /* parse numbers separated by commas */
                while (*p) {
                    /* skip whitespace */
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == '\0') {
                        break;
                    }
                    long val;
                    /* find end of number token */
                    const char *numstart = p;
                    while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
                    char token[64];
                    size_t toklen = p - numstart;
                    if (toklen >= sizeof(token)) toklen = sizeof(token) - 1;
                    strncpy(token, numstart, toklen);
                    token[toklen] = '\0';
                    if (!parse_number(token, &val)) {
                        fprintf(stdout, "Error on line %d: invalid number '%s'\n", orig_line, token);
                        error_count++;
                        break;
                    }
                    /* store 10‑bit two's complement */
                    long min_val = -(1 << 9);
                    long max_val = (1 << 9) - 1;
                    if (val < min_val || val > max_val) {
                        fprintf(stdout, "Error on line %d: data value out of range\n", orig_line);
                        error_count++;
                        val = 0;
                    }
                    ensure_data_capacity(&data_image, &data_cap, DC + 1);
                    data_image[DC].value = (unsigned int)(val & 0x3FF);
                    DC++;
                    /* skip whitespace */
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == ',') {
                        p++;
                    } else {
                        break;
                    }
                }
            } else if (strcmp(directive, ".string") == 0) {
                /* Handle .string */
                if (has_label) {
                    if (st_add(&symtab, label, (int)DC, ATTR_DATA) != 0) {
                        fprintf(stdout, "Error on line %d: duplicate symbol '%s'\n", orig_line, label);
                        error_count++;
                        continue;
                    }
                }
                /* Expect a quoted string */
                if (*p != '"') {
                    fprintf(stdout, "Error on line %d: missing opening quote for string\n", orig_line);
                    error_count++;
                    continue;
                }
                p++; /* skip opening quote */
                const char *startstr = p;
                /* find closing quote */
                while (*p && *p != '"') p++;
                if (*p != '"') {
                    fprintf(stdout, "Error on line %d: missing closing quote in string\n", orig_line);
                    error_count++;
                    continue;
                }
                size_t slen = p - startstr;
                for (size_t j = 0; j < slen; j++) {
                    unsigned char c = (unsigned char)startstr[j];
                    ensure_data_capacity(&data_image, &data_cap, DC + 1);
                    data_image[DC].value = (unsigned int)c & 0x3FF;
                    DC++;
                }
                /* Append NUL terminator */
                ensure_data_capacity(&data_image, &data_cap, DC + 1);
                data_image[DC].value = 0;
                DC++;
                p++; /* skip closing quote */
                /* ensure no extra tokens besides whitespace */
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p != '\0') {
                    fprintf(stdout, "Error on line %d: extraneous characters after string\n", orig_line);
                    error_count++;
                }
            } else if (strcmp(directive, ".mat") == 0) {
                /* Handle .mat [rows][cols] values */
                if (has_label) {
                    if (st_add(&symtab, label, (int)DC, ATTR_DATA) != 0) {
                        fprintf(stdout, "Error on line %d: duplicate symbol '%s'\n", orig_line, label);
                        error_count++;
                        continue;
                    }
                }
                /* Expect [rows][cols] */
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p != '[') {
                    fprintf(stdout, "Error on line %d: missing [rows] in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                p++;
                /* parse rows number */
                char numbuf[32];
                size_t n = 0;
                while (*p && *p != ']') {
                    if (n < sizeof(numbuf) - 1) numbuf[n++] = *p;
                    p++;
                }
                numbuf[n] = '\0';
                if (*p != ']') {
                    fprintf(stdout, "Error on line %d: missing closing ] for rows in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                p++; /* skip ']' */
                long rows;
                if (!parse_number(numbuf, &rows) || rows <= 0) {
                    fprintf(stdout, "Error on line %d: invalid rows count in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                /* skip whitespace */
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p != '[') {
                    fprintf(stdout, "Error on line %d: missing [cols] in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                p++;
                n = 0;
                while (*p && *p != ']') {
                    if (n < sizeof(numbuf) - 1) numbuf[n++] = *p;
                    p++;
                }
                numbuf[n] = '\0';
                if (*p != ']') {
                    fprintf(stdout, "Error on line %d: missing closing ] for cols in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                p++; /* skip ']' */
                long cols;
                if (!parse_number(numbuf, &cols) || cols <= 0) {
                    fprintf(stdout, "Error on line %d: invalid cols count in .mat\n", orig_line);
                    error_count++;
                    continue;
                }
                /* skip whitespace */
                while (*p && isspace((unsigned char)*p)) p++;
                long total_cells = rows * cols;
                long values_found = 0;
                if (*p != '\0') {
                    /* parse initialization values separated by comma */
                    while (*p && values_found < total_cells) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p == '\0') break;
                        /* find end of number */
                        const char *numstart = p;
                        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
                        size_t toklen = p - numstart;
                        char tok[64];
                        if (toklen >= sizeof(tok)) toklen = sizeof(tok) - 1;
                        strncpy(tok, numstart, toklen);
                        tok[toklen] = '\0';
                        long val;
                        if (!parse_number(tok, &val)) {
                            fprintf(stdout, "Error on line %d: invalid number '%s' in .mat\n", orig_line, tok);
                            error_count++;
                            break;
                        }
                        long min_val = -(1 << 9);
                        long max_val = (1 << 9) - 1;
                        if (val < min_val || val > max_val) {
                            fprintf(stdout, "Error on line %d: .mat value out of range\n", orig_line);
                            error_count++;
                            val = 0;
                        }
                        ensure_data_capacity(&data_image, &data_cap, DC + 1);
                        data_image[DC].value = (unsigned int)(val & 0x3FF);
                        DC++;
                        values_found++;
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p == ',') p++;
                    }
                    /* skip any trailing numbers beyond expected cells */
                    while (values_found < total_cells) {
                        /* fill with zeros */
                        ensure_data_capacity(&data_image, &data_cap, DC + 1);
                        data_image[DC].value = 0;
                        DC++;
                        values_found++;
                    }
                    /* if more values provided than needed */
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p != '\0' && values_found >= total_cells) {
                        fprintf(stdout, "Error on line %d: too many initializer values in .mat\n", orig_line);
                        error_count++;
                    }
                } else {
                    /* no initialization values, fill with zeros */
                    for (long k = 0; k < total_cells; k++) {
                        ensure_data_capacity(&data_image, &data_cap, DC + 1);
                        data_image[DC].value = 0;
                        DC++;
                    }
                }
            } else if (strcmp(directive, ".entry") == 0) {
                /* .entry handled in second pass */
                /* ensure only one operand (label) and it's valid; otherwise error */
                int opcount;
                char ops[2][MAX_LABEL_LEN + 32];
                if (!parse_operands(p, ops, &opcount) || opcount != 1) {
                    fprintf(stdout, "Error on line %d: invalid .entry usage\n", orig_line);
                    error_count++;
                }
            } else if (strcmp(directive, ".extern") == 0) {
                /* parse one label */
                char ops[2][MAX_LABEL_LEN + 32];
                int opcount;
                if (!parse_operands(p, ops, &opcount) || opcount != 1) {
                    fprintf(stdout, "Error on line %d: invalid .extern usage\n", orig_line);
                    error_count++;
                    continue;
                }
                char *ext_label = ops[0];
                /* ensure valid label */
                if (!is_valid_label(ext_label)) {
                    fprintf(stdout, "Error on line %d: invalid extern label '%s'\n", orig_line, ext_label);
                    error_count++;
                    continue;
                }
                /* add symbol to table if not exists */
                Symbol *existing = st_find(&symtab, ext_label);
                if (existing) {
                    /* if symbol already defined as external, ok; else error */
                    if (!(existing->attributes & ATTR_EXTERN)) {
                        fprintf(stdout, "Error on line %d: symbol '%s' already defined, cannot declare extern\n", orig_line, ext_label);
                        error_count++;
                        continue;
                    }
                } else {
                    st_add(&symtab, ext_label, 0, ATTR_EXTERN);
                }
            } else {
                fprintf(stdout, "Error on line %d: unknown directive '%s'\n", orig_line, directive);
                error_count++;
                continue;
            }
        } else {
            /* instruction */
            /* read opcode */
            const char *startop = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t oplen = p - startop;
            char opname[16];
            if (oplen >= sizeof(opname)) oplen = sizeof(opname) - 1;
            strncpy(opname, startop, oplen);
            opname[oplen] = '\0';
            int opcode = opcode_of(opname);
            if (opcode < 0) {
                fprintf(stdout, "Error on line %d: unknown instruction '%s'\n", orig_line, opname);
                error_count++;
                continue;
            }
            /* If label exists, add to symtab as code with current IC */
            if (has_label) {
                if (st_add(&symtab, label, (int)IC, ATTR_CODE) != 0) {
                    fprintf(stdout, "Error on line %d: duplicate symbol '%s'\n", orig_line, label);
                    error_count++;
                    continue;
                }
            }
            /* skip whitespace */
            while (*p && isspace((unsigned char)*p)) p++;
            /* parse operands */
            char ops[2][MAX_LABEL_LEN + 32];
            int opcount;
            if (!parse_operands(p, ops, &opcount)) {
                fprintf(stdout, "Error on line %d: error parsing operands\n", orig_line);
                error_count++;
                continue;
            }
            int expected = expected_operands(opcode);
            if (expected != opcount) {
                fprintf(stdout, "Error on line %d: wrong number of operands for '%s'\n", orig_line, opname);
                error_count++;
                continue;
            }
            /* Determine addressing modes and encode */
            int src_mode = 0;
            int dst_mode = 0;
            /* Note: immediate values and symbol names are captured by
             * identify_addr_mode into src_imm/dst_imm and src_label/dst_label.
             */
            int reg1, reg2;
            /* Determine number of words needed */
            size_t words_needed = 1; /* main word */
            int src_reg = -1, dst_reg = -1;
            int matrix_src_reg1 = -1, matrix_src_reg2 = -1;
            int matrix_dst_reg1 = -1, matrix_dst_reg2 = -1;
            char src_label[MAX_LABEL_LEN + 1];
            char dst_label[MAX_LABEL_LEN + 1];
            long src_imm = 0;
            long dst_imm = 0;
            /* When there is only one operand, it should be treated as the destination operand.
             * We therefore detect that case up front and perform addressing mode checks against
             * the destination column of the legal addressing mode table.  We also set src_mode
             * equal to dst_mode so that later code which expects src_mode to contain the
             * addressing bits for the single operand (used for dest bits in main word) will
             * still work.  For instructions that expect two operands, the first is source and
             * the second is destination as usual.
             */
            if (opcount == 1) {
                /* Single operand: treat as destination */
                if (!identify_addr_mode(ops[0], &dst_mode, &dst_imm, dst_label, &reg1, &reg2)) {
                    fprintf(stdout, "Error on line %d: invalid operand '%s'\n", orig_line, ops[0]);
                    error_count++;
                    continue;
                }
                /* Validate dest addressing mode */
                if (!(legal_addr_modes[opcode][1] & (1 << dst_mode))) {
                    fprintf(stdout, "Error on line %d: illegal destination addressing mode for '%s'\n", orig_line, opname);
                    error_count++;
                    continue;
                }
                /* For later encoding, copy dest_mode into src_mode so that the main word
                 * encoding (which uses src_mode for the dest bits when opcount==1) will work.
                 */
                src_mode = dst_mode;
                /* Compute words needed for the destination operand */
                switch (dst_mode) {
                    case 0:
                        /* immediate uses an extra word */
                        words_needed += 1;
                        break;
                    case 1:
                        /* direct uses one extra word */
                        words_needed += 1;
                        break;
                    case 2:
                        /* matrix uses two extra words */
                        words_needed += 2;
                        matrix_dst_reg1 = reg1;
                        matrix_dst_reg2 = reg2;
                        break;
                    case 3:
                        /* register direct uses one extra word */
                        words_needed += 1;
                        dst_reg = reg1;
                        break;
                }
            } else {
                /* opcount == 0 or opcount == 2.  If opcount == 2 the first operand is source,
                 * and the second is destination.  If opcount == 0 there are no operands and
                 * nothing to do here.
                 */
                if (opcount > 0) {
                    /* parse source operand */
                    if (!identify_addr_mode(ops[0], &src_mode, &src_imm, src_label, &reg1, &reg2)) {
                        fprintf(stdout, "Error on line %d: invalid operand '%s'\n", orig_line, ops[0]);
                        error_count++;
                        continue;
                    }
                    /* Validate source addressing mode */
                    if (!(legal_addr_modes[opcode][0] & (1 << src_mode))) {
                        fprintf(stdout, "Error on line %d: illegal source addressing mode for '%s'\n", orig_line, opname);
                        error_count++;
                        continue;
                    }
                    /* compute words for source operand */
                    switch (src_mode) {
                        case 0:
                            words_needed += 1;
                            break;
                        case 1:
                            words_needed += 1;
                            break;
                        case 2:
                            words_needed += 2;
                            matrix_src_reg1 = reg1;
                            matrix_src_reg2 = reg2;
                            break;
                        case 3:
                            words_needed += 1;
                            src_reg = reg1;
                            break;
                    }
                }
                if (opcount > 1) {
                    /* parse destination operand */
                    if (!identify_addr_mode(ops[1], &dst_mode, &dst_imm, dst_label, &reg1, &reg2)) {
                        fprintf(stdout, "Error on line %d: invalid operand '%s'\n", orig_line, ops[1]);
                        error_count++;
                        continue;
                    }
                    /* Validate dest addressing mode */
                    if (!(legal_addr_modes[opcode][1] & (1 << dst_mode))) {
                        fprintf(stdout, "Error on line %d: illegal destination addressing mode for '%s'\n", orig_line, opname);
                        error_count++;
                        continue;
                    }
                    /* compute words for destination operand */
                    switch (dst_mode) {
                        case 0:
                            words_needed += 1;
                            break;
                        case 1:
                            words_needed += 1;
                            break;
                        case 2:
                            words_needed += 2;
                            matrix_dst_reg1 = reg1;
                            matrix_dst_reg2 = reg2;
                            break;
                        case 3:
                            words_needed += 1;
                            dst_reg = reg1;
                            break;
                    }
                }
                /* If both operands exist and both are register direct, one word less */
                if (opcount == 2 && src_mode == 3 && dst_mode == 3) {
                    words_needed -= 1;
                }
            }
            /* Ensure capacity */
            ensure_code_capacity(&code_image, &code_cap, IC + words_needed);
            /* Encode main word */
            MachineWord mw;
            unsigned int main_val = 0;
            main_val |= ((unsigned int)opcode & 0xF) << 6;
            /* source addressing bits (4‑5 bits) */
            unsigned int sm = (opcount == 2 ? (unsigned int)src_mode : 0);
            main_val |= (sm & 0x3) << 4;
            /* dest addressing bits (2‑3 bits) */
            unsigned int dm = (opcount >= 1 ? (opcount == 2 ? (unsigned int)dst_mode : (unsigned int)src_mode) : 0);
            /* If only one operand, treat it as dest (bits 2‑3) */
            main_val |= (dm & 0x3) << 2;
            /* A/R/E bits: always absolute (00) for main word */
            main_val |= 0;
            mw.value = main_val & 0x3FF;
            code_image[IC++] = mw;
            /* Build extra words for operands */
            if (opcount == 2) {
                /* Source operand extra words */
                if (src_mode == 0) {
                    /* immediate */
                    long val = src_imm;
                    /* 8‑bit two's complement */
                    long min8 = -(1 << 7);
                    long max8 = (1 << 7) - 1;
                    if (val < min8 || val > max8) {
                        fprintf(stdout, "Error on line %d: immediate value out of range\n", orig_line);
                        error_count++;
                        val = 0;
                    }
                    unsigned int encoded = ((unsigned int)(val & 0xFF) << 2);
                    MachineWord w;
                    w.value = encoded & 0x3FF;
                    code_image[IC++] = w;
                } else if (src_mode == 1) {
                    /* direct; placeholder; will patch later */
                    MachineWord w;
                    w.value = 0; /* A/R/E bits will be set in second pass */
                    code_image[IC] = w;
                    /* record unresolved */
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, src_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                } else if (src_mode == 2) {
                    /* matrix: first word placeholder for address */
                    MachineWord w;
                    w.value = 0;
                    code_image[IC] = w;
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, src_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                    /* second word: encode registers */
                    MachineWord w2;
                    unsigned int val = 0;
                    val |= ((unsigned int)matrix_src_reg1 & 0xF) << 6;
                    val |= ((unsigned int)matrix_src_reg2 & 0xF) << 2;
                    w2.value = val & 0x3FF;
                    code_image[IC++] = w2;
                } else if (src_mode == 3) {
                    /* register */
                    if (dst_mode == 3) {
                        /* will be combined with dest later */
                    } else {
                        MachineWord w;
                        unsigned int val = 0;
                        val |= ((unsigned int)src_reg & 0xF) << 6;
                        w.value = val & 0x3FF;
                        code_image[IC++] = w;
                    }
                }
                /* Destination operand extra words */
                if (dst_mode == 0) {
                    long val = dst_imm;
                    long min8 = -(1 << 7);
                    long max8 = (1 << 7) - 1;
                    if (val < min8 || val > max8) {
                        fprintf(stdout, "Error on line %d: immediate value out of range\n", orig_line);
                        error_count++;
                        val = 0;
                    }
                    unsigned int encoded = ((unsigned int)(val & 0xFF) << 2);
                    MachineWord w;
                    w.value = encoded & 0x3FF;
                    if (src_mode == 3 && dst_mode == 3) {
                        /* This case cannot happen: dest immediate cannot combine with reg */
                        code_image[IC++] = w;
                    } else {
                        code_image[IC++] = w;
                    }
                } else if (dst_mode == 1) {
                    MachineWord w;
                    w.value = 0;
                    code_image[IC] = w;
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, dst_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                } else if (dst_mode == 2) {
                    MachineWord w;
                    w.value = 0;
                    code_image[IC] = w;
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, dst_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                    MachineWord w2;
                    unsigned int val = 0;
                    val |= ((unsigned int)matrix_dst_reg1 & 0xF) << 6;
                    val |= ((unsigned int)matrix_dst_reg2 & 0xF) << 2;
                    w2.value = val & 0x3FF;
                    code_image[IC++] = w2;
                } else if (dst_mode == 3) {
                    if (src_mode == 3) {
                        /* shared word for both */
                        MachineWord w;
                        unsigned int val = 0;
                        val |= ((unsigned int)src_reg & 0xF) << 6;
                        val |= ((unsigned int)dst_reg & 0xF) << 2;
                        w.value = val & 0x3FF;
                        code_image[IC++] = w;
                    } else {
                        MachineWord w;
                        unsigned int val = 0;
                        val |= ((unsigned int)dst_reg & 0xF) << 2;
                        w.value = val & 0x3FF;
                        code_image[IC++] = w;
                    }
                }
            } else if (opcount == 1) {
                /* Only destination operand present (unary instruction).  The variables
                 * dst_mode, dst_imm, dst_label, matrix_dst_reg1/2 and dst_reg were
                 * populated earlier when parsing the operand as dest.  Generate the
                 * appropriate extra word(s) for this operand.
                 */
                if (dst_mode == 0) {
                    /* Immediate value */
                    long val = dst_imm;
                    long min8 = -(1 << 7);
                    long max8 = (1 << 7) - 1;
                    if (val < min8 || val > max8) {
                        fprintf(stdout, "Error on line %d: immediate value out of range\n", orig_line);
                        error_count++;
                        val = 0;
                    }
                    unsigned int encoded = ((unsigned int)(val & 0xFF) << 2);
                    MachineWord w;
                    w.value = encoded & 0x3FF;
                    code_image[IC++] = w;
                } else if (dst_mode == 1) {
                    /* Direct addressing: unresolved symbol to be patched later */
                    MachineWord w;
                    w.value = 0;
                    code_image[IC] = w;
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, dst_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                } else if (dst_mode == 2) {
                    /* Matrix addressing: placeholder for symbol and then register word */
                    MachineWord w;
                    w.value = 0;
                    code_image[IC] = w;
                    Unresolved *ur = (Unresolved *)malloc(sizeof(Unresolved));
                    strncpy(ur->name, dst_label, MAX_LABEL_LEN);
                    ur->name[MAX_LABEL_LEN] = '\0';
                    ur->index = IC;
                    ur->orig_line = orig_line;
                    ur->next = NULL;
                    if (unresolved_tail) {
                        unresolved_tail->next = ur;
                        unresolved_tail = ur;
                    } else {
                        unresolved_head = unresolved_tail = ur;
                    }
                    IC++;
                    MachineWord w2;
                    unsigned int val = 0;
                    val |= ((unsigned int)matrix_dst_reg1 & 0xF) << 6;
                    val |= ((unsigned int)matrix_dst_reg2 & 0xF) << 2;
                    w2.value = val & 0x3FF;
                    code_image[IC++] = w2;
                } else if (dst_mode == 3) {
                    /* Register direct: encode register bits in bits 2-5 */
                    MachineWord w;
                    unsigned int val = 0;
                    val |= ((unsigned int)dst_reg & 0xF) << 2;
                    w.value = val & 0x3FF;
                    code_image[IC++] = w;
                }
            }
        }
    }
    /* end first pass */
    size_t ICF = IC;
    size_t DCF = DC;

    /* Enforce total memory limit.  If the sum of code and data words
     * exceeds the maximum memory size, report an error and skip
     * further processing. */
    if ((ICF + DCF) > MAX_MEM_WORDS) {
        fprintf(stdout, "Error: program uses %zu words, exceeding limit of %d\n", (ICF + DCF), MAX_MEM_WORDS);
        error_count++;
    }
    /* Update data symbols' addresses by adding ICF */
    st_update_data_addresses(&symtab, (int)ICF);
    /* No need to add 100 now; we will add 100 when producing output file. */
    /* Second pass: resolve .entry directives and patch unresolved */
    for (size_t i = 0; i < count; i++) {
        const char *text = lines[i].text;
        int orig_line = lines[i].orig_line;
        if (is_blank_or_comment(text)) continue;
        /* parse label and skip */
        char lbl[MAX_LABEL_LEN + 1];
        lbl[0] = '\0';
        size_t pos = 0;
        int err = 0;
        parse_label(text, lbl, &pos, &err);
        const char *p = text + pos;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '.') {
            /* directive */
            const char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t dlen = p - start;
            char directive[16];
            if (dlen >= sizeof(directive)) dlen = sizeof(directive) - 1;
            strncpy(directive, start, dlen);
            directive[dlen] = '\0';
            while (*p && isspace((unsigned char)*p)) p++;
            if (strcmp(directive, ".entry") == 0) {
                /* parse operand */
                char ops[2][MAX_LABEL_LEN + 32];
                int opcount;
                if (parse_operands(p, ops, &opcount) && opcount == 1) {
                    char *ent_label = ops[0];
                    Symbol *sym = st_find(&symtab, ent_label);
                    if (!sym) {
                        fprintf(stdout, "Error on line %d: entry label '%s' not defined\n", orig_line, ent_label);
                        error_count++;
                    } else {
                        sym->attributes |= ATTR_ENTRY;
                    }
                } else {
                    fprintf(stdout, "Error on line %d: invalid .entry usage\n", orig_line);
                    error_count++;
                }
            }
        }
    }
    /* Patch unresolved references */
    for (Unresolved *ur = unresolved_head; ur != NULL; ur = ur->next) {
        Symbol *sym = st_find(&symtab, ur->name);
        if (!sym) {
            fprintf(stdout, "Error on line %d: undefined symbol '%s'\n", ur->orig_line, ur->name);
            error_count++;
            continue;
        }
        MachineWord *word = &code_image[ur->index];
        unsigned int val = 0;
        unsigned int addr = (unsigned int)sym->address;
        /* Insert address into bits 2‑9 */
        val |= (addr & 0xFF) << 2;
        /* Determine ARE bits */
        unsigned int are_bits = 0; /* absolute by default */
        if (sym->attributes & ATTR_EXTERN) {
            are_bits = 0x1; /* E = 01 */
            addr = 0;
            val &= ~((unsigned int)0xFF << 2); /* value bits zeroed */
        } else {
            are_bits = 0x2; /* R = 10 */
            /* Since addresses stored are offsets relative to start of program,
             * the loader would normally add base address.  In this simplified
             * implementation we treat these addresses as absolute offsets
             * relative to start and mark relocatable.  We leave value
             * unchanged. */
        }
        val |= are_bits;
        word->value = val & 0x3FF;
        if (sym->attributes & ATTR_EXTERN) {
            /* record external reference */
            ExtRef *ext = (ExtRef *)malloc(sizeof(ExtRef));
            strncpy(ext->name, sym->name, MAX_LABEL_LEN);
            ext->name[MAX_LABEL_LEN] = '\0';
            /* absolute memory address: add 100 to word index */
            ext->address = (unsigned int)(ur->index + 100);
            ext->next = NULL;
            if (ext_tail) {
                ext_tail->next = ext;
                ext_tail = ext;
            } else {
                ext_head = ext_tail = ext;
            }
        }
    }
    /* If errors occurred, do not generate output */
    int rc = 0;
    if (error_count > 0) {
        fprintf(stdout, "Assembly terminated with %d error(s).\n", error_count);
        rc = 1;
    } else {
        /* generate output files */
        /* object file name */
        char obname[256];
        snprintf(obname, sizeof(obname), "%s.ob", basename);
        FILE *fob = fopen(obname, "w");
        if (!fob) {
            fprintf(stderr, "Error: cannot open output file '%s'\n", obname);
        } else {
            /* write code and data length in base4 (unique) on first line */
            char len_code[6];
            char len_data[6];
            to_unique_base4((unsigned int)ICF, len_code);
            to_unique_base4((unsigned int)DCF, len_data);
            fprintf(fob, "%s %s\n", len_code, len_data);
            /* write code words */
            for (size_t idx = 0; idx < ICF; idx++) {
                unsigned int addr = (unsigned int)(idx + 100);
                char addrstr[6];
                address_to_unique_base4(addr, addrstr);
                char valstr[6];
                to_unique_base4(code_image[idx].value, valstr);
                fprintf(fob, "%s %s\n", addrstr, valstr);
            }
            /* write data words */
            for (size_t idx = 0; idx < DCF; idx++) {
                unsigned int addr = (unsigned int)(ICF + idx + 100);
                char addrstr[6];
                address_to_unique_base4(addr, addrstr);
                char valstr[6];
                to_unique_base4(data_image[idx].value, valstr);
                fprintf(fob, "%s %s\n", addrstr, valstr);
            }
            fclose(fob);
        }
        /* entry file */
        /* determine if any entries exist */
        int entries_exist = 0;
        /* count entries */
        Symbol *cur = symtab.head;
        while (cur) {
            if ((cur->attributes & ATTR_ENTRY) && !(cur->attributes & ATTR_EXTERN)) {
                entries_exist = 1;
                break;
            }
            cur = cur->next;
        }
        if (entries_exist) {
            char entname[256];
            snprintf(entname, sizeof(entname), "%s.ent", basename);
            FILE *fent = fopen(entname, "w");
            if (!fent) {
                fprintf(stderr, "Error: cannot open entry file '%s'\n", entname);
            } else {
                for (Symbol *it = symtab.head; it != NULL; it = it->next) {
                    if ((it->attributes & ATTR_ENTRY) && !(it->attributes & ATTR_EXTERN)) {
                        char addrstr[6];
                        unsigned int addr = (unsigned int)(it->address + 100);
                        address_to_unique_base4(addr, addrstr);
                        fprintf(fent, "%s %s\n", it->name, addrstr);
                    }
                }
                fclose(fent);
            }
        }
        /* extern file */
        if (ext_head) {
            char extname[256];
            snprintf(extname, sizeof(extname), "%s.ext", basename);
            FILE *fext = fopen(extname, "w");
            if (!fext) {
                fprintf(stderr, "Error: cannot open extern file '%s'\n", extname);
            } else {
                for (ExtRef *ex = ext_head; ex != NULL; ex = ex->next) {
                    char addrstr[6];
                    unsigned int addr = ex->address;
                    address_to_unique_base4(addr, addrstr);
                    fprintf(fext, "%s %s\n", ex->name, addrstr);
                }
                fclose(fext);
            }
        }
    }
    /* Free resources */
    st_free(&symtab);
    free(code_image);
    free(data_image);
    /* free unresolved list */
    while (unresolved_head) {
        Unresolved *next = unresolved_head->next;
        free(unresolved_head);
        unresolved_head = next;
    }
    /* free ext refs */
    while (ext_head) {
        ExtRef *next = ext_head->next;
        free(ext_head);
        ext_head = next;
    }
    return rc;
}