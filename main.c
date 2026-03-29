/*
 * main.c
 *
 * Entry point for the assembler program.  This file provides a
 * simple command line interface that accepts one or more source
 * filenames (without extension), preprocesses each file to expand
 * macros and then assembles the resulting code into object (.ob),
 * entry (.ent) and external (.ext) files.  Errors encountered
 * during preprocessing or assembly are reported to standard output
 * or standard error as appropriate.
 *
 * Usage:
 *   assembler file1 file2 ...
 *
 * For each given base filename, the program attempts to read
 * `fileX.as`, preprocess it and assemble it.  The output files are
 * written using the base name with appropriate extensions.  If any
 * errors occur while processing a file, its output files are not
 * generated (or are removed if partially written).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocessor.h"
#include "assembler.h"
#include "macro_table.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 [file2 ...]\n", argv[0]);
        return 1;
    }
    int overall_errors = 0;
    /* Process each filename provided on the command line */
    for (int i = 1; i < argc; i++) {
        const char *base = argv[i];
        /* Build the input filename by appending .as */
        char input_filename[512];
        snprintf(input_filename, sizeof(input_filename), "%s.as", base);
        MacroTable macro_table;
        mt_init(&macro_table);
        PreprocessedLine *lines = NULL;
        size_t line_count = 0;
        /* Preprocess the source file */
        if (pp_process_file(input_filename, &macro_table, &lines, &line_count) != 0) {
            /* Preprocessor already reported an error */
            mt_free(&macro_table);
            overall_errors++;
            continue;
        }
        /* Assemble the preprocessed lines.  The assembler will
         * generate .ob, .ent and .ext files based on the base name.
         */
        int rc = assemble_file(base, lines, line_count);
        if (rc != 0) {
            /* assembly errors have been printed by assemble_file */
            overall_errors++;
        }
        /* Clean up */
        pp_free_lines(lines, line_count);
        mt_free(&macro_table);
    }
    return (overall_errors > 0) ? 1 : 0;
}