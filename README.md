# Assembler Project

A two-pass assembler written in C for a custom assembly language. Built as a systems programming project.

## What it does

Takes assembly source files (.as) as input and produces machine code output. The assembler runs in two passes over the source code to resolve all symbols and labels before generating the final binary output.

## Features

- Two-pass assembly process
- Macro expansion (preprocessor stage)
- Symbol table with support for code, data, external, and entry attributes
- Error reporting with line numbers
- Supports labels, directives, and instructions

## Project Structure

| File | Description |
|---|---|
| main.c | Entry point, handles file arguments |
| assembler.c / .h | Core assembly logic, two-pass processing |
| preprocessor.c / .h | Macro expansion before assembly |
| symbol_table.c / .h | Symbol table implementation using a linked list |
| macro_table.c / .h | Macro definition storage |
| Makefile | Build configuration |

## How to build and run

`ash
make
./assembler example2.as
`

## Language

C (C90 standard)