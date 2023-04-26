/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int num_libs, num_symbols;
char **libs, **target_symbols;
FILE *fptr, *popen_fptr;

/**
 * This struct stores an array of symbols with the size so arrays of
 * symbols are easily accessed within another array.
 */
struct symbol_table {
    int lines;
    char** symbols;
} typedef symbol_table;
symbol_table* symbol_tables;

/**
 * Clean up everything before exit.
 */
void clean_up() {
    if (target_symbols != NULL) {
        for (int symbol = 0; symbol < num_symbols; symbol++) {
            free(target_symbols[symbol]);
            target_symbols[symbol] = NULL;
        }
    }
    free(target_symbols);
    target_symbols = NULL;

    for (int lib = 0; lib < num_libs; lib++) {
        if (libs != NULL) {
            free(libs[lib]);
            libs[lib] = NULL;
        }
        if (symbol_tables != NULL) {
            for (int i = 0; i < symbol_tables[lib].lines; i++) {
                if (symbol_tables[lib].symbols != NULL) {
                    free(symbol_tables[lib].symbols[i]);
                    symbol_tables[lib].symbols[i] = NULL;
                }
            }
            free(symbol_tables[lib].symbols);
            symbol_tables[lib].symbols = NULL;
            // not freeing symbol_tables ptr since that is allocated on main's stack
        }
    }
    free(libs);
    libs = NULL;

    if (popen_fptr != NULL) {
        pclose(popen_fptr);
        popen_fptr = NULL;
    }

    if (fptr != NULL) {
        fclose(fptr);
        fptr = NULL;
    }
}

/**
 * Clean up an array of strings.
 * @param char* output - input arg, the string array to clean up
 * @param int lines - input arg, how many strings are in the output
 */
void clean_up_str_array(char** output, int lines) {
    if (output != NULL) {
        for (int i = 0; i < lines; i++) {
            if (output[i] != NULL) {
                free(output[i]);
                output[i] = NULL;
            }
        }
        free(output);
        output = NULL;
    }
}

/**
 * Generic function for running a command and getting the output
 * as an array of strings.
 * @param char* command - input arg, the string which is the command to run
 * @param int* num_lines - ouput arg, after the command is run, the size of the return  array
 * @param int* status - ouput arg, after the command is run, the exit status of the command
 * @return char** - a pointer to an array of strings that is each line of the output
 */
char** get_command_output(char* command, int* num_lines, int* status) {

    char buffer[1024];
    popen_fptr = popen(command, "r");
    if (popen_fptr == NULL) {
        clean_up();
        printf("Failed to run command: %s\n", command);
        exit(1);
    }

    char** output = NULL;
    int index = 0;
    int line_len = 0;

    while (fgets(buffer, sizeof(buffer), popen_fptr) != NULL) {
        if (output == NULL) {
            output = (char**)malloc(sizeof(char*));
            if (output == NULL) {
                clean_up();
                printf("Failed to allocate memory for command array: %d\n", index);
                exit(1);
            } else {
                output[index] = NULL;
            }
        }

        int buffer_len = strlen(buffer);
        char* tmp_str = (char*)realloc(output[index], (line_len + buffer_len) * sizeof(char));
        if (tmp_str == NULL) {
            clean_up();
            printf("Failed to allocate memory for command output\n");
            exit(1);
        } else {
            output[index] = tmp_str;
        }

        if (buffer[buffer_len - 1] == '\n') {
            memcpy(output[index] + line_len, buffer, buffer_len);
            output[index][line_len + buffer_len - 1] = 0;
            line_len = 0;

            index++;
            char** tmp = (char**)realloc(output, (index + 1) * sizeof(char*));
            if (tmp == NULL) {
                clean_up();
                printf("Failed to allocate memory for command array: %d\n", index);
                exit(1);
            } else {
                output = tmp;
                output[index] = NULL;
            }
        } else {
            memcpy(output[index] + line_len, buffer, buffer_len);
            line_len += buffer_len;
        }
    }

    *num_lines = index;
    *status = pclose(popen_fptr);
    popen_fptr = NULL;
    return output;
}

/**
 * Get the ldd output, and parsing the path for the dependent library.
 * @param char* target - input arg, the binary to run ldd on
 * @param char* ld_libpath - input arg, the LD_LIBRARY_PATH to use to find libraries
 * @param int* return_size - output arg, the size of the array of strings returned
 * @return char** - the array of strings for each line of output
 */
char** get_ldd(char* target, char* ld_libpath, int* return_size) {

#if __linux__
    const char* target_ldd_format =
        "LD_LIBRARY_PATH=%s ldd %s | awk 'NF == 4 {print $3}; NF == 2 {print $1}'";
#elif __APPLE__
    const char* target_ldd_format =
        "for LIB in $(otool -L %s | awk '{gsub(/@rpath\\//, \"\", $1); print $1;}'); "
        "do for PATHLIB in %s; do if [ -e "
        "$PATHLIB/$LIB ]; then echo $PATHLIB/$LIB; break; fi; done; done;";
#endif

    char* target_ldd = (char*)malloc(
        (strlen(ld_libpath) + strlen(target) + strlen(target_ldd_format)) * sizeof(char));
    if (target_ldd == NULL) {
        printf("Failed to allocate memory for target ldd: %s %s\n", target, ld_libpath);
        clean_up();
        exit(1);
    }
#if __linux__
    sprintf(target_ldd, target_ldd_format, ld_libpath, target);
#elif __APPLE__
    sprintf(target_ldd, target_ldd_format, target, ld_libpath);
#endif

    int status = -1;
    char** output = get_command_output(target_ldd, return_size, &status);
    free(target_ldd);
    target_ldd = NULL;
    if (status == 0) {
        return output;
    } else {
        clean_up_str_array(output, *return_size);
        return NULL;
    }
}

/**
 * Get the nm of undefined symbols for the target library so we can search for
 * those missing symbols in other libraries.
 * @param char* target - input arg, the binary to run nm on
 * @param int* return_size - output arg, the size of the array of strings returned
 * @return char** - the array of strings for each line of output
 */
char** get_target_symbols(char* target, int* return_size) {

#if __linux__
    const char* target_symbols_format = "nm -D -u %s 2>/dev/null | awk '{print $NF}'";
#elif __APPLE__
    const char* target_symbols_format =
        "nm --dyldinfo-only --undefined-only %s 2>/dev/null | awk '{print $NF}'";
#endif

    char* target_symbols_command =
        (char*)malloc((strlen(target) + strlen(target_symbols_format)) * sizeof(char));
    if (target_symbols_command == NULL) {
        printf("Failed to allocate memory for target_symbols: %s\n", target);
        clean_up();
        exit(1);
    }
    sprintf(target_symbols_command, target_symbols_format, target);

    int status;
    char** output = get_command_output(target_symbols_command, return_size, &status);
    free(target_symbols_command);
    target_symbols_command = NULL;
    if (status == 0) {
        return output;
    } else {
        clean_up_str_array(output, *return_size);
        return NULL;
    }
}

/**
 * Get the nm of defined symbols for the dependent library so we can search for
 * matching symbols.
 * @param char* lib - input arg, the binary to run nm on
 * @param int* return_size - output arg, the size of the array of strings returned
 * @return char** - the array of strings for each line of output
 */
char** get_lib_symbols(char* lib, int* return_size) {

#if __linux__
    const char* lib_symbols_format = "nm -D --defined-only %s 2>/dev/null | awk '{print $NF}'";
#elif __APPLE__
    const char* lib_symbols_format =
        "nm --dyldinfo-only --defined-only %s 2>/dev/null | awk '{print $NF}'";
#endif

    char* lib_symbols = (char*)malloc((strlen(lib) + strlen(lib_symbols_format)) * sizeof(char));
    if (lib_symbols == NULL) {
        printf("Failed to allocate memory for lib_symbols: %s\n", lib);
        clean_up();
        exit(1);
    }
    sprintf(lib_symbols, lib_symbols_format, lib);

    int status;
    char** output = get_command_output(lib_symbols, return_size, &status);
    free(lib_symbols);
    lib_symbols = NULL;
    if (status == 0) {
        return output;
    } else {
        clean_up_str_array(output, *return_size);
        return NULL;
    }
}

int main(int argc, char** argv) {

    if (argc < 4) {
        printf("%s: A small tool to resolve undefined symbols in linked dynamic libraries.\n",
               argv[0]);
        printf("USAGE : %s <target_shared_library> <LD_LIBRARY_PATH> <output_file.json>\n",
               argv[0]);
        exit(1);
    }

    num_libs = num_symbols = 0;
    libs = target_symbols = NULL;
    symbol_tables = NULL;
    popen_fptr = fptr = NULL;

    // For the given target library first we will extract the libraries
    // it depends on and any symbols that are undefined that should
    // be defined in one of those libraries.
    libs = get_ldd(argv[1], argv[2], &num_libs);
    target_symbols = get_target_symbols(argv[1], &num_symbols);

    if (num_libs == 0 || num_symbols == 0) {
        FILE* fptr;
        fptr = fopen(argv[3], "w");
        fprintf(fptr, "{}\n");
        fclose(fptr);
        fptr = NULL;
        clean_up();
        exit(0);
    }

    // Now pre-emptively collect all the symbols from all the libraries
    // the target library depends on.
    symbol_table tmp[num_libs];
    symbol_tables = tmp;
    for (int lib = 0; lib < num_libs; lib++) {
        symbol_tables[lib].lines = 0;
        symbol_tables[lib].symbols = NULL;
    }
    for (int lib = 0; lib < num_libs; lib++) {
        symbol_tables[lib].symbols = get_lib_symbols(libs[lib], &symbol_tables[lib].lines);
    }

    // Now loop through all the undefined symbols and discover the first library
    // that the missing symbol is defined in. Output to target file in json format.
    FILE* fptr;
    fptr = fopen(argv[3], "w");
    fprintf(fptr, "{");
    int found_symbol_dep = 0;
    for (int symbol = 0; symbol < num_symbols; symbol++) {
        for (int lib = 0; lib < num_libs; lib++) {
            for (int i = 0; i < symbol_tables[lib].lines; i++) {
                if (strcmp(target_symbols[symbol], symbol_tables[lib].symbols[i]) == 0) {
                    found_symbol_dep = 1;
                    fprintf(fptr, "\n\t\"%s\":\"%s\",", target_symbols[symbol], libs[lib]);
                    goto found_symbol;
                }
            }
        }
        found_symbol:;
    }
    if (found_symbol_dep == 1) {
        // delete the last comma
        fseek(fptr, -1, SEEK_CUR);
    }
    fprintf(fptr, "\n}\n");
    fclose(fptr);
    fptr = NULL;

    clean_up();
    exit(0);
}
