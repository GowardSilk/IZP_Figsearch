#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t ErrorNum;
#define ERR_NONE (ErrorNum)0
#define ERR_INVALID_NUMBER_ARGS (ErrorNum)0xB16B00B5
#define ERR_INVALID_COMMAND (ErrorNum)0xBAADF00D

typedef struct Error {
    ErrorNum err;
    char *msg;
} Error;

Error error_ctor(ErrorNum err, const char *fmt, ...) {
    va_list fmt_args;
    va_start(fmt_args, fmt);
    int size = vsprintf(NULL, fmt, fmt_args) + 1;
    // todo: check allocation
    char *msg = calloc(size, sizeof(char));
    // todo: check printf
    size = vsprintf(msg, fmt, fmt_args);
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

typedef enum CommandType { HELP = 0, TEST, HLINE, VLINE, SQUARE } CommandType;

typedef struct Command {
    CommandType type;
    const char *file_name;
} Command;

Error command_help(void) {
    printf("Some help message here...");
    return error_none();
}
Error command_test(const char *file_name) {
    printf("Hello from command_test");
    return error_none();
}
Error command_hline(const char *file_name) {
    printf("Hello from command_hline");
    return error_none();
}
Error command_vline(const char *file_name) {
    printf("Hello from command_vline");
    return error_none();
}
Error command_square(const char *file_name) {
    printf("Hello from command_square");
    return error_none();
}

Error command_execute(Command *cmd) {
    switch (cmd->type) {
        case HELP:
            return command_help();
        case TEST:
            return command_test(cmd->file_name);
        case HLINE:
            return command_hline(cmd->file_name);
        case VLINE:
            return command_vline(cmd->file_name);
        case SQUARE:
            return command_square(cmd->file_name);
    }
}

Error command_parse(int argc, char **argv, Command *out_cmd) {
    // ensure that the command is of correct size
    printf("num args: %d", argc);
    if (argc > 3 || argc < 2) {
        return error_ctor(ERR_INVALID_NUMBER_ARGS,
                          "Invalid number of arguments given! Expected: "
                          "1=[--help] or 2 but given: %d\n",
                          argc - 1);
    }

    // validate command
    if (argc == 2) /* HELP command */ {
        if (strcmp(argv[1], "--help") == 0) {
            out_cmd->type = HELP;
            out_cmd->file_name = NULL;
            return error_none();
        }
        return error_ctor(ERR_INVALID_COMMAND,
                          "Invalid command given [%s]! Expected: --help. Did "
                          "you forget to add bitmap file name?",
                          argv[1]);
    }
    if (strcmp(argv[1], "test") == 0) {
        out_cmd->type = TEST;
        out_cmd->file_name = argv[2];
        return error_none();
    }
    if (strcmp(argv[1], "hline") == 0) {
        out_cmd->type = HLINE;
        out_cmd->file_name = argv[2];
        return error_none();
    }
    if (strcmp(argv[1], "vline") == 0) {
        out_cmd->type = VLINE;
        out_cmd->file_name = argv[2];
        return error_none();
    }
    if (strcmp(argv[1], "square") == 0) {
        out_cmd->type = SQUARE;
        out_cmd->file_name = argv[2];
        return error_none();
    }
    return error_ctor(ERR_INVALID_COMMAND,
                      "Invalid command given [%s]! Expected one of: --help, "
                      "test, hline, vline, sqaure.",
                      argv[1]);
}

int main(int argc, char **argv) {
    // parse command line
    Command cmd = {0};
    {
        Error err = command_parse(argc, argv, &cmd);
        if (err.err) {
            error_dispatch(err);
            return err.err;
        }
        error_dtor(&err);
    }

    // execute given command
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
