#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t ErrorNum;
#define ERR_NONE (ErrorNum)0
#define ERR_INTERNAL (ErrorNum)0x000000B5
#define ERR_INVALID_NUMBER_ARGS (ErrorNum)0xB16B00B5
#define ERR_INVALID_COMMAND (ErrorNum)0xBAADF00D
#define ERR_INVALID_BITMAP_FILE (ErrorNum)0xDEADBAAD
#define ERR_READ_RAW_BITMAP (ErrorNum)0xBAD8EAD
#define ERR_BITMAP_TEST (ErrorNum)0x8BADF00D
#define ERR_INVALID_DIMENSION (ErrorNum)0xABADBABE

typedef struct Error {
    ErrorNum err;
    char *msg;
} Error;

Error error_ctor(ErrorNum err, const char *fmt, ...) {
    va_list fmt_args;
    va_start(fmt_args, fmt);
    int size = vsnprintf(NULL, 0, fmt, fmt_args) + 1;
    // todo: check allocation
    char *msg = calloc(size, sizeof(char));
    // todo: check printf
    size = vsnprintf(msg, size, fmt, fmt_args);
    va_end(fmt_args);

    return (Error){err, msg};
}

static inline Error error_none() { return (Error){ERR_NONE, NULL}; }

static inline void error_dispatch(Error err) { fprintf(stderr, "%s", err.msg); }

void error_dtor(Error *err) {
    if (err->msg) {
        free(err->msg);
        err->msg = NULL;
    }
}

typedef struct BitmapSize {
    uint32_t width;
    uint32_t height;
} BitmapSize;

typedef char *BitmapData;

typedef struct Bitmap {
    BitmapSize size;
    BitmapData data;
} Bitmap;

size_t bmp_ignore_whitespace(char *raw, size_t raw_size) {
    const char *raw_begin = raw;
    char *raw_dst = raw;
    for (; raw_dst != raw_begin + raw_size;) {
        /* skip new line characters (Windows: \r\n; Linux: \n) */
#ifdef _WIN32
        if (*raw == '\r') {
            raw++;
        }
#endif
        if (*raw == '\n') {
            raw++;
        }
        /* skip spaces */
        if (*raw == ' ') {
            raw++;
        } else {
            *raw_dst++ = *raw++;
        }
    }
    return raw_dst - raw_begin;
}

static inline Error bmp_load_dimension(FILE *file, uint32_t *dimension) {
    int ret = fscanf(file, "%u", dimension);
    if (ret != 1) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Given dimension [%d], is not an int value!",
                          dimension);
    }
    return error_none();
}

static inline Error bmp_load_size(FILE *file, BitmapSize *out_size) {
    Error err = bmp_load_dimension(file, &out_size->height);
    if (err.err) {
        return err;
    }
    return bmp_load_dimension(file, &out_size->width);
}

/**
 * @brief loads bitmap into "out_bmp"
 */
Error bmp_load(const char *file_name, Bitmap *out_bmp) {
    /* try to open the file */
    FILE *file = fopen(file_name, "rb");
    /* return on error */
    if (file == NULL) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Given bitmap file [%s] does not exist!\n",
                          file_name);
    }
    /* if success, load raw */

    /* load the data size from the raw buffer */
    Error err = bmp_load_size(file, &out_bmp->size);
    if (err.err) {
        return err;
    }
    printf("Loaded size: %dx%d\n", out_bmp->size.width, out_bmp->size.height);

    /* calculate data size (in bytes) */
    size_t data_fpos = ftell(file);
    int success = fseek(file, 0, SEEK_END);
    if (success != 0) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "The file seems to be corrupted, raw bitmap could "
                          "not be loaded! Error code: %d\n",
                          errno);
    }
    size_t size = ftell(file) - data_fpos;

    // todo: common safe function for seeking
    success = fseek(file, data_fpos, SEEK_SET);
    if (success != 0) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "The file seems to be corrupted, raw bitmap could "
                          "not be loaded! Error code: %d\n",
                          errno);
    }

    /* allocate */
    char *raw = malloc(size * sizeof(char));
    if (!raw) {
        fclose(file);
        return error_ctor(ERR_INTERNAL, "Failed to allocate raw bitmap buffer");
    }

    size_t bytes_read = fread(raw, sizeof(char), size, file);
    if (bytes_read != size) {
        fclose(file);
        free(raw);

        if (feof(file)) {
            return error_ctor(
                ERR_READ_RAW_BITMAP,
                "Could not read the whole file (unexpected end of file)!");
        } else {
            return error_ctor(ERR_READ_RAW_BITMAP,
                              "Could not read the whole file");
        }
    }

    /* close file, we do not need it anymore */
    fclose(file);

    /* normalize the raw buffer from whitespace (note: without size) */
    size_t new_size = bmp_ignore_whitespace(raw, size);
    printf("Raw normalized: %s\n", raw);

    // todo: because of bad sizing, this does not work: see "Raw" data
    // if (new_size != (size_t)out_bmp->size.width * out_bmp->size.height) {
    //    return error_ctor(ERR_INVALID_BITMAP_FILE, "Bitmap contains illegal
    //    characters!");
    //}

    out_bmp->data = malloc(new_size);
    if (!out_bmp->data) {
        free(raw);
        return error_ctor(ERR_INTERNAL, "Failed to allocate bitmap data!");
    }
    // todo: buffer is larger than expected
    memcpy(out_bmp->data, raw, new_size);

    /* release resources */
    free(raw);

    return error_none();
}

#define bmp_pxl_is_valid(pxl) ((pxl) == '0') || ((pxl) == '1')

bool bmp_is_valid(const Bitmap bmp) {
    char *begin = bmp.data;
    char *end = bmp.data + bmp.size.width * bmp.size.height;
    /* check bitmap values */
    for (; begin != end; begin++) {
        printf("[ '%c' (%d) ], ", *begin, (int)*begin);
        /*
        if (!bmp_pxl_is_valid(*begin)) {
            return false;
        }*/
    }
    return true;
}

typedef enum CommandType { HELP = 0, TEST, HLINE, VLINE, SQUARE } CommandType;

typedef struct Command {
    CommandType type;
    Bitmap bmp;
} Command;

Error command_help(void) {
    printf("Some help message here...");
    return error_none();
}
Error command_test(Bitmap bmp) {
    if (!bmp_is_valid(bmp)) {
        return error_ctor(ERR_BITMAP_TEST, "Invalid");
    }
    return error_ctor(ERR_NONE, "Valid");
}
Error command_hline(Bitmap bmp) {
    printf("Hello from command_hline");
    return error_none();
}
Error command_vline(Bitmap bmp) {
    printf("Hello from command_vline");
    return error_none();
}
Error command_square(Bitmap bmp) {
    printf("Hello from command_square");
    return error_none();
}

Error command_execute(Command *cmd) {
    switch (cmd->type) {
        case HELP:
            return command_help();
        case TEST:
            return command_test(cmd->bmp);
        case HLINE:
            return command_hline(cmd->bmp);
        case VLINE:
            return command_vline(cmd->bmp);
        case SQUARE:
            return command_square(cmd->bmp);
    }
    return error_ctor(ERR_INTERNAL, "Invalid control path executed. Line: %d",
                      __LINE__);
}

Error command_parse(int argc, char **argv, Command *out_cmd) {
    /* ensure that the command is of correct size */
    if (argc > 3 || argc < 2) {
        return error_ctor(
            ERR_INVALID_NUMBER_ARGS,
            "Invalid number of arguments given! Expected: "
            "1=[--help] or 2 but given: %d. See \"%s --help\" for more info\n",
            argc - 1, argv[0]);
    }

    /* validate command args */
    if (argc == 2) /* HELP command */ {
        if (strcmp(argv[1], "--help") == 0) {
            out_cmd->type = HELP;
            return error_none();
        }
        return error_ctor(ERR_INVALID_COMMAND,
                          "Invalid command given [%s]! Expected: --help. Did "
                          "you forget to add bitmap file name?",
                          argv[1]);
    }
    /* todo: fix redudancy ??? */
    if (strcmp(argv[1], "test") == 0) {
        out_cmd->type = TEST;
        Error err = bmp_load(argv[2], &out_cmd->bmp);
        if (err.err) {
            return err;
        }
        return error_none();
    }
    if (strcmp(argv[1], "hline") == 0) {
        out_cmd->type = HLINE;
        Error err = bmp_load(argv[2], &out_cmd->bmp);
        if (err.err) {
            return err;
        }
        return error_none();
    }
    if (strcmp(argv[1], "vline") == 0) {
        out_cmd->type = VLINE;
        Error err = bmp_load(argv[2], &out_cmd->bmp);
        if (err.err) {
            return err;
        }
        return error_none();
    }
    if (strcmp(argv[1], "square") == 0) {
        out_cmd->type = SQUARE;
        Error err = bmp_load(argv[2], &out_cmd->bmp);
        if (err.err) {
            return err;
        }
        return error_none();
    }
    return error_ctor(ERR_INVALID_COMMAND,
                      "Invalid command given [%s]! Expected one of: --help, "
                      "test, hline, vline, sqaure.",
                      argv[1]);
}

int main(int argc, char **argv) {
    /* parse command line */
    Command cmd = {0};
    argv = (char *[]){"exe", "test", "pic.txt"};
    argc = 3;
    {
        Error err = command_parse(argc, argv, &cmd);
        if (err.err) {
            error_dispatch(err);
            return err.err;
        }
        error_dtor(&err);
    }

    /* execute given command */
    {
        Error err = command_execute(&cmd);
        if (err.err) {
            error_dispatch(err);
            return err.err;
        }
        error_dtor(&err);
    }

    return ERR_NONE;
}
