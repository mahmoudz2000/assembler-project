/*
 * macro_table.c
 *
 * Implementation of a simple macro table.  Each macro definition
 * holds an array of lines that constitute the macro body.  When the
 * macro is invoked, the caller can expand the macro by iterating
 * through its body lines.  The macro table owns the memory for the
 * macro names and bodies and will free them when mt_free() is called.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macro_table.h"

/* Initialise the macro table */
void mt_init(MacroTable *table) {
    if (table) {
        table->head = NULL;
        table->tail = NULL;
    }
}

/* Free all macros and their associated memory */
void mt_free(MacroTable *table) {
    if (!table) {
        return;
    }
    MacroDef *curr = table->head;
    while (curr) {
        MacroDef *next = curr->next;
        /* free body lines */
        if (curr->body) {
            for (size_t i = 0; i < curr->count; i++) {
                free(curr->body[i]);
            }
            free(curr->body);
        }
        free(curr);
        curr = next;
    }
    table->head = table->tail = NULL;
}

/* Add a macro definition */
int mt_add(MacroTable *table, const char *name, char **lines, size_t count) {
    if (!table || !name) {
        return 1;
    }
    /* prevent duplicate macros */
    for (MacroDef *it = table->head; it != NULL; it = it->next) {
        if (strcmp(it->name, name) == 0) {
            return 1;
        }
    }
    MacroDef *mac = (MacroDef *)malloc(sizeof(MacroDef));
    if (!mac) {
        fprintf(stderr, "Error: out of memory allocating macro\n");
        exit(EXIT_FAILURE);
    }
    strncpy(mac->name, name, MAX_LABEL_LEN);
    mac->name[MAX_LABEL_LEN] = '\0';
    mac->body = lines;
    mac->count = count;
    mac->next = NULL;
    if (table->tail) {
        table->tail->next = mac;
        table->tail = mac;
    } else {
        table->head = table->tail = mac;
    }
    return 0;
}

/* Find a macro definition */
MacroDef *mt_find(const MacroTable *table, const char *name) {
    if (!table || !name) {
        return NULL;
    }
    for (MacroDef *it = table->head; it != NULL; it = it->next) {
        if (strcmp(it->name, name) == 0) {
            return it;
        }
    }
    return NULL;
}