/*
 * macro_table.h
 *
 * The macro table holds definitions of macros encountered during the
 * pre–processing phase.  Each macro definition consists of a name and
 * a list of lines constituting the macro body.  When a macro is
 * invoked in the source, its lines are inserted in place of the macro
 * name.  Macro names are limited to the same length as labels and
 * must not clash with instruction names or directives.
 */

#ifndef MACRO_TABLE_H
#define MACRO_TABLE_H

#include <stddef.h>

#include "symbol_table.h"

/* Representation of a macro definition.  The name is stored as a
 * NUL‑terminated string.  The body is stored as an array of strings,
 * each representing a line of source code.  The count field records
 * the number of lines in the body.  Memory for both the name and the
 * body lines is allocated by the macro table; the caller should not
 * free the returned pointers directly.
 */
typedef struct MacroDef {
    char name[MAX_LABEL_LEN + 1];
    char **body;     /* array of lines */
    size_t count;    /* number of lines */
    struct MacroDef *next;
} MacroDef;

/* Macro table type.  Implemented as singly linked list. */
typedef struct {
    MacroDef *head;
    MacroDef *tail;
} MacroTable;

/* Initialise a macro table. */
void mt_init(MacroTable *table);

/* Free all macros and associated memory. */
void mt_free(MacroTable *table);

/* Add a new macro to the table.  Takes ownership of the body array
 * pointed to by `lines` (which should be dynamically allocated) and
 * stores the number of lines in `count`.  Returns 0 on success or 1
 * if a macro with the same name already exists.  Macro names are
 * treated case–sensitively.
 */
int mt_add(MacroTable *table, const char *name, char **lines, size_t count);

/* Find a macro definition by name.  Returns pointer to the macro or
 * NULL if not found. */
MacroDef *mt_find(const MacroTable *table, const char *name);

#endif /* MACRO_TABLE_H */