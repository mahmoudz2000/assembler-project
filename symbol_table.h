/*
 * symbol_table.c
 *
 * Implementation of a simple symbol table for the assembler.  The
 * table uses a singly linked list to store symbols.  Each symbol has
 * a name, an address and a bitmask of attributes.  Duplicate names
 * are not allowed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symbol_table.h"

/* Initialise an empty symbol table. */
void st_init(SymbolTable *table) {
    if (table) {
        table->head = NULL;
        table->tail = NULL;
    }
}

/* Free all symbols in the table. */
void st_free(SymbolTable *table) {
    if (!table) {
        return;
    }
    Symbol *curr = table->head;
    while (curr) {
        Symbol *next = curr->next;
        free(curr);
        curr = next;
    }
    table->head = table->tail = NULL;
}

/* Insert a new symbol.  Returns 0 on success, 1 on duplicate name. */
int st_add(SymbolTable *table, const char *name, int address, int attributes) {
    if (!table || !name) {
        return 1;
    }
    /* Check for duplicate */
    for (Symbol *it = table->head; it != NULL; it = it->next) {
        if (strcmp(it->name, name) == 0) {
            /* Duplicate name */
            return 1;
        }
    }
    Symbol *sym = (Symbol *)malloc(sizeof(Symbol));
    if (!sym) {
        fprintf(stderr, "Error: memory allocation failure in st_add\n");
        exit(EXIT_FAILURE);
    }
    strncpy(sym->name, name, MAX_LABEL_LEN);
    sym->name[MAX_LABEL_LEN] = '\0';
    sym->address = address;
    sym->attributes = attributes;
    sym->next = NULL;
    /* Append to list */
    if (table->tail) {
        table->tail->next = sym;
        table->tail = sym;
    } else {
        table->head = table->tail = sym;
    }
    return 0;
}

/* Find a symbol by name. */
Symbol *st_find(const SymbolTable *table, const char *name) {
    if (!table || !name) {
        return NULL;
    }
    for (Symbol *it = table->head; it != NULL; it = it->next) {
        if (strcmp(it->name, name) == 0) {
            return it;
        }
    }
    return NULL;
}

/* Update all data symbols by adding offset to their addresses. */
int st_update_data_addresses(SymbolTable *table, int offset) {
    int updated = 0;
    if (!table) {
        return 0;
    }
    for (Symbol *it = table->head; it != NULL; it = it->next) {
        if (it->attributes & ATTR_DATA && !(it->attributes & ATTR_EXTERN)) {
            it->address += offset;
            updated++;
        }
    }
    return updated;
}