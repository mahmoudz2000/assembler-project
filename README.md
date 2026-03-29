/*
 * symbol_table.h
 *
 * Declaration of the symbol table API used by the assembler.  The
 * symbol table maps symbol names to addresses and attributes (code,
 * data, external, entry) and provides lookup and update utilities.
 */

#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include <stddef.h>

/* Maximum length of a label (not including terminating NUL).  The
 * project specification limits labels to up to 31 characters. */
#define MAX_LABEL_LEN 31

/* Attribute bitmasks for symbols.  Symbols may have multiple
 * attributes combined with bitwise OR. */
#define ATTR_CODE   0x1
#define ATTR_DATA   0x2
#define ATTR_EXTERN 0x4
#define ATTR_ENTRY  0x8

/* Representation of a single symbol in the table.  The `name`
 * contains the label name, `address` holds the offset (relative to
 * start of code) where the symbol is defined and `attributes` holds
 * bitflags describing whether the symbol refers to code/data and
 * whether it is external or an entry. */
typedef struct Symbol {
    char name[MAX_LABEL_LEN + 1];
    int address;
    int attributes;
    struct Symbol *next;
} Symbol;

/* Symbol table structure implemented as a singly linked list. */
typedef struct {
    Symbol *head;
    Symbol *tail;
} SymbolTable;

/* Initialise an empty symbol table. */
void st_init(SymbolTable *table);

/* Free all symbols and clear the table. */
void st_free(SymbolTable *table);

/* Add a new symbol to the table.  Returns 0 on success or 1 if a
 * symbol with the same name already exists. */
int st_add(SymbolTable *table, const char *name, int address, int attributes);

/* Find a symbol in the table by name.  Returns a pointer to the
 * symbol or NULL if not found. */
Symbol *st_find(const SymbolTable *table, const char *name);

/* Update all data symbols by adding an offset to their addresses.
 * Returns the number of symbols updated. */
int st_update_data_addresses(SymbolTable *table, int offset);

#endif /* SYMBOL_TABLE_H */