/*
 * preprocessor.c
 *
 * The preprocessor scans a source file for macro definitions and
 * produces an expanded set of lines.  Macro definitions are delimited
 * by the directives `mcro <name>` and `mcroend`.  Macro names may
 * consist of alphanumeric characters (letters or digits) and must not
 * begin with a period.  They must also not collide with instruction
 * names or existing directives.  When a macro is invoked in the
 * source (i.e. its name appears as the first token on a line), the
 * macro body lines are inserted into the output.  The original line
 * number of the invocation is associated with all expanded lines.
 */

#define _GNU_SOURCE /* for getline */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "preprocessor.h"

/* Forward declaration of helper */
static char *strdup_safe(const char *s);

/* Helper to trim leading whitespace */
static char *ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

/* Helper to trim trailing whitespace in place.  Returns the original
 * pointer for convenience. */
static char *rtrim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

/* Check whether a token matches an assembler instruction. */
static int is_instruction(const char *tok) {
    static const char *ops[] = {
        "mov", "cmp", "add", "sub", "not", "clr",
        "lea", "inc", "dec", "jmp", "bne", "red",
        "prn", "jsr", "rts", "stop", NULL
    };
    for (const char **p = ops; *p != NULL; p++) {
        if (strcmp(*p, tok) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Check whether a token matches a directive name. */
static int is_directive(const char *tok) {
    static const char *dirs[] = {
        ".data", ".string", ".mat", ".entry", ".extern",
        "mcro", "mcroend", NULL
    };
    for (const char **p = dirs; *p != NULL; p++) {
        if (strcmp(*p, tok) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Duplicate a string with error checking. */
static char *strdup_safe(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "Error: memory allocation failure\n");
        exit(EXIT_FAILURE);
    }
    memcpy(copy, s, len + 1);
    return copy;
}

/* Free preprocessed lines */
void pp_free_lines(PreprocessedLine *lines, size_t count) {
    if (!lines) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(lines[i].text);
    }
    free(lines);
}

/* Process a source file and expand macros */
int pp_process_file(const char *filename, MacroTable *macro_table,
                    PreprocessedLine **out_lines, size_t *out_count) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: unable to open source file '%s'\n", filename);
        return 1;
    }
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    size_t cap = 64;
    size_t count = 0;
    PreprocessedLine *lines = (PreprocessedLine *)malloc(cap * sizeof(PreprocessedLine));
    if (!lines) {
        fprintf(stderr, "Error: out of memory in preprocessor\n");
        fclose(fp);
        return 1;
    }

    int inside_macro = 0;
    char macro_name[MAX_LABEL_LEN + 1];
    char **macro_body = NULL;
    size_t macro_body_cap = 0;
    size_t macro_body_len = 0;
    int line_num = 0;
    while ((nread = getline(&line, &len, fp)) != -1) {
        line_num++;
        /* Strip newline */
        if (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[--nread] = '\0';
            /* Remove possible CR before newline */
            if (nread > 0 && line[nread - 1] == '\r') {
                line[--nread] = '\0';
            }
        }
        char *orig = line;
        /* Trim leading/trailing whitespace for macro processing */
        char *trim = ltrim(orig);
        rtrim(trim);
        /* Skip completely empty lines when inside macro; they are still part of macro body. */
        if (inside_macro) {
            if (trim[0] != '\0' && strncmp(trim, "mcroend", 7) == 0 &&
                (trim[7] == '\0' || isspace((unsigned char)trim[7]))) {
                /* End of macro */
                inside_macro = 0;
                /* Add macro to table */
                if (mt_add(macro_table, macro_name, macro_body, macro_body_len) != 0) {
                    fprintf(stderr, "Error: duplicate macro '%s' on line %d\n", macro_name, line_num);
                    /* free macro_body because not added */
                    for (size_t i = 0; i < macro_body_len; i++) {
                        free(macro_body[i]);
                    }
                    free(macro_body);
                    macro_body = NULL;
                }
                macro_body = NULL;
                macro_body_cap = 0;
                macro_body_len = 0;
                continue;
            } else {
                /* Append to macro body */
                if (macro_body_len >= macro_body_cap) {
                    size_t new_cap = macro_body_cap ? macro_body_cap * 2 : 4;
                    char **tmp = (char **)realloc(macro_body, new_cap * sizeof(char *));
                    if (!tmp) {
                        fprintf(stderr, "Error: out of memory storing macro body\n");
                        exit(EXIT_FAILURE);
                    }
                    macro_body = tmp;
                    macro_body_cap = new_cap;
                }
                macro_body[macro_body_len++] = strdup_safe(trim);
                continue;
            }
        }

        /* Not inside a macro definition */
        /* If line is empty or comment, copy as is */
        if (*trim == '\0' || *trim == ';') {
            /* Add to output lines */
            if (count >= cap) {
                size_t newcap = cap * 2;
                PreprocessedLine *tmp = (PreprocessedLine *)realloc(lines, newcap * sizeof(PreprocessedLine));
                if (!tmp) {
                    fprintf(stderr, "Error: out of memory while expanding lines\n");
                    exit(EXIT_FAILURE);
                }
                lines = tmp;
                cap = newcap;
            }
            lines[count].text = strdup_safe(orig);
            lines[count].orig_line = line_num;
            count++;
            continue;
        }
        /* Parse first token (identifier) */
        char token[MAX_LABEL_LEN + 1] = {0};
        size_t idx = 0;
        char *ptr = trim;
        while (*ptr && !isspace((unsigned char)*ptr)) {
            if (idx < MAX_LABEL_LEN) {
                token[idx++] = *ptr;
            }
            ptr++;
        }
        token[idx] = '\0';
        /* Check for macro definition */
        if (strcmp(token, "mcro") == 0) {
            /* Start of macro definition */
            /* Skip whitespace */
            while (*ptr && isspace((unsigned char)*ptr)) ptr++;
            /* Read macro name */
            char macname[MAX_LABEL_LEN + 1] = {0};
            size_t ni = 0;
            while (*ptr && !isspace((unsigned char)*ptr)) {
                if (ni < MAX_LABEL_LEN) {
                    macname[ni++] = *ptr;
                }
                ptr++;
            }
            macname[ni] = '\0';
            if (macname[0] == '\0') {
                fprintf(stderr, "Error: missing macro name on line %d\n", line_num);
                /* ignore this macro definition */
            } else {
                /* Validate macro name: must not match instruction or directive or start with '.' */
                if (macname[0] == '.' || is_instruction(macname) || is_directive(macname) || mt_find(macro_table, macname)) {
                    fprintf(stderr, "Error: invalid or duplicate macro name '%s' on line %d\n", macname, line_num);
                    /* ignore macro definition but still consume until mcroend */
                    inside_macro = 1;
                    /* allocate dummy body that will be freed later */
                    macro_body = NULL;
                    macro_body_cap = 0;
                    macro_body_len = 0;
                    strcpy(macro_name, "");
                    continue;
                }
                inside_macro = 1;
                strcpy(macro_name, macname);
                macro_body = NULL;
                macro_body_cap = macro_body_len = 0;
            }
            continue;
        }
        /* Check for macro invocation */
        MacroDef *macdef = mt_find(macro_table, token);
        if (macdef) {
            /* Expand macro: copy each line of body to output */
            for (size_t i = 0; i < macdef->count; i++) {
                if (count >= cap) {
                    size_t newcap = cap * 2;
                    PreprocessedLine *tmp = (PreprocessedLine *)realloc(lines, newcap * sizeof(PreprocessedLine));
                    if (!tmp) {
                        fprintf(stderr, "Error: out of memory while expanding macro lines\n");
                        exit(EXIT_FAILURE);
                    }
                    lines = tmp;
                    cap = newcap;
                }
                lines[count].text = strdup_safe(macdef->body[i]);
                lines[count].orig_line = line_num;
                count++;
            }
            continue;
        }
        /* Normal line: copy as is */
        if (count >= cap) {
            size_t newcap = cap * 2;
            PreprocessedLine *tmp = (PreprocessedLine *)realloc(lines, newcap * sizeof(PreprocessedLine));
            if (!tmp) {
                fprintf(stderr, "Error: out of memory while storing lines\n");
                exit(EXIT_FAILURE);
            }
            lines = tmp;
            cap = newcap;
        }
        lines[count].text = strdup_safe(orig);
        lines[count].orig_line = line_num;
        count++;
    }
    free(line);
    fclose(fp);
    *out_lines = lines;
    *out_count = count;
    return 0;
}