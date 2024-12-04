#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================
 *                Constants
 * ========================================= */

#define ERR_NONE                (0)
#define ERR_ALLOCATION_FAILURE  (0x00FA75E5)
#define ERR_INTERNAL            (0x000000B5)
#define ERR_INVALID_NUMBER_ARGS (0xB16B00B5)
#define ERR_INVALID_COMMAND     (0xBAADF00D)
#define ERR_INVALID_BITMAP_FILE (0x8BADF00D)
#define ERR_INVALID_DIMENSION   (0xABADBABE)

#define PXL_FILLED ('1')
#define PXL_EMPTY  ('0')

#define CMD_MAX_ARGS (3)
#define CMD_MIN_ARGS (2)

#define COORD_INVALID (UINT32_MAX)

#define BMP_LOADER_READ_CHUNK_SIZE (512)

/* =========================================
 *                  Error
 * ========================================= */

typedef int ErrorNum;
typedef struct Error {
    ErrorNum code;
    char    *msg;
} Error;

/**
 * @brief constructs Error struct object
 * @param fmt is a format for error message */
static Error error_ctor(ErrorNum err, const char *fmt, ...) {
    /* construct the formatted message of the error */
    va_list fmt_args;
    va_start(fmt_args, fmt);

    /* required err msg size */
    int size = vsnprintf(NULL, 0, fmt, fmt_args) + 1;

    va_end(fmt_args);
    va_start(fmt_args, fmt);

    char *err_msg = calloc(size, sizeof(char));
    if (err_msg == NULL) {
        /* note: since we cannot allocate, we cannot really create the message
         * and pass it through the call stack */
        fprintf(stderr,
                "Internal error: allocation failed while creating error "
                "object. Requested size: %d bytes.\n (Error code: %#x)",
                size, ERR_ALLOCATION_FAILURE);
        return (Error){ERR_ALLOCATION_FAILURE, NULL};
    }

    /* write formatted message */
    vsnprintf(err_msg, size, fmt, fmt_args);
    va_end(fmt_args);

    return (Error){err, err_msg};
}

static inline Error error_none(void) { return (Error){ERR_NONE, NULL}; }

static inline void error_print(Error err) { fprintf(stderr, "%s\n", err.msg); }

/**
 * @brief destroys the error's allocated memory
 * @return the last error code of `err` param */
static ErrorNum error_dtor(Error *err) {
    ErrorNum err_code = err->code;
    if (err->msg != NULL) {
        free(err->msg);
        err->msg = NULL;
        err->code = ERR_NONE;
    }
    return err_code;
}

/* =========================================
 *                  Bitmap
 * ========================================= */

typedef struct BitmapSize {
    uint32_t width;
    uint32_t height;
} BitmapSize;

typedef char   Pixel;
typedef Pixel *BitmapData;

/** @brief representation of "bitmap" file */
typedef struct Bitmap {
    BitmapSize dimensions;
    /** @note whole bitmap is stored linearly, in order to receive the
     * appropriate linear size of this buffer @see bmp_size_raw */
    BitmapData data;
} Bitmap;

/** @brief loader is used for loading bitmap and validating bitmap files, to
 * retrieve the loaded bitmap @see bmp_loader_get_bitmap */
typedef struct BitmapLoader {
    /** @brief staging bitmap buffer */
    Bitmap staging;
    /** @brief stores the file location of the bitmap file */
    const char *file_name;
    /** @brief holds the current number of pixels stored in staging buffer */
    size_t size;
} BitmapLoader;

/**
 * @return linear size of the bitmap data from given dimensions */
static inline size_t bmp_size_raw(BitmapSize dimension) {
    return (size_t)dimension.width * dimension.height;
}

/**
 * @brief creates bitmap based on the bitmap size
 * @return ERR_NONE when out_bmp was successfully populated */
static Error bmp_ctor(BitmapSize dimensions, Bitmap *out_bmp) {
    out_bmp->dimensions = dimensions;
    out_bmp->data = malloc(sizeof(Pixel) * bmp_size_raw(dimensions) + 1);
    if (out_bmp->data == NULL) {
        return error_ctor(ERR_ALLOCATION_FAILURE,
                          "Failed to allocate bitmap data buffer!\n");
    }
    return error_none();
}

#define bmp_at(bmp, row, col) (bmp)->data[row * (bmp)->dimensions.width + col]

/**
 * @brief destructs given bitmap if it is populated with data */
static void bmp_dtor(Bitmap *bmp) {
    if (bmp->data != NULL) {
        free(bmp->data);
        bmp->dimensions = (BitmapSize){0};
    }
}

/** @brief creates default bitmap loader */
static inline BitmapLoader bmp_loader_ctor(const char *file_name) {
    return (BitmapLoader){
        .staging.dimensions = (BitmapSize){0},
        .staging.data = NULL,
        .size = 0,
        .file_name = file_name,
    };
}

/** @brief destroys bitmap loader */
static void bmp_loader_dtor(BitmapLoader *restrict loader) {
    if (loader->staging.data != NULL) {
        free(loader->staging.data);
        loader->size = 0;
        loader->file_name = NULL;
    }
}

/**
 * @brief returns bmp loader's staging buffer to a bitmap
 * @note the function does not check whether a given bitmap load was successfull
 * @note if bitmap loader's buffer is invalidated, cannot be retrieved again */
static inline Bitmap bmp_loader_get_bitmap(BitmapLoader *restrict loader) {
    /* copy the final bitmap before invalidation */
    Bitmap final = loader->staging;
    /* invalidate loader's staging buffer */
    loader->staging = (Bitmap){0};
    return final;
}

/**
 * @brief adds pixel to the bmp loader's staging buffer
 * @return NULL when loader's memory is exceeded */
static BitmapLoader *bmp_loader_add_pixel(BitmapLoader *restrict loader,
                                          Pixel c) {
    if (loader->size >= bmp_size_raw(loader->staging.dimensions)) {
        return NULL;
    }
    loader->staging.data[loader->size++] = c;
    return loader;
}

/** @brief checks whether given character is a valid whitespace character */
static inline bool bmp_valid_whitespace(char c) { return isspace(c); }
/** @brief checks whether given pixel value is populated with valid data */
static inline bool bmp_valid_pix(Pixel pxl) {
    return pxl == PXL_FILLED || pxl == PXL_EMPTY;
}

/**
 * @brief populates loader's staging buffer with the file data
 * @note function assumes that the file pointer is pointing to a pixel value
 * @return error when staging buffer oveflowed or when an invalid character
 * encountered */
static Error bmp_loader_ignore_whitespace(FILE *file,
                                          BitmapLoader *restrict loader) {
    /* read until end of file */
    while (!feof(file)) {
        /* buffered reading (we will read everything in smaller chunks to avoid
         * fgetc call overhead) */
        char   buffer[BMP_LOADER_READ_CHUNK_SIZE] = {0};
        size_t read = fread(&buffer, sizeof(char), sizeof(buffer), file);
        /* iterate over the read chunk and determine its validity and populate
         * the loader's staging buffer */
        for (size_t i = 0; i < read; i++) {
            if (bmp_valid_whitespace(buffer[i])) {
                continue;
            }
            if (bmp_valid_pix(buffer[i])) {
                if (!bmp_loader_add_pixel(loader, buffer[i])) {
                    return error_ctor(ERR_INVALID_BITMAP_FILE,
                                      "The raw bitmap size does not match given"
                                      " dimensions!");
                }
                continue;
            }
            return error_ctor(ERR_INVALID_BITMAP_FILE,
                              "Unexpected character encountered: '%c'",
                              buffer[i]);
        }
    }
    return error_none();
}

/**
 * @brief loads bitmap dimension into `out_dimension`
 * @return error_none when loaded successfully, otherwsise Error::code > 0 */
static inline Error bmp_loader_load_dimension(FILE     *file,
                                              uint32_t *out_dimension) {
    int ret = fscanf(file, "%" SCNu32, out_dimension);
    if (ret != 1) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Given dimension [%" PRIu32 "], is not an int value!",
                          *out_dimension);
    }
    /* we do not consider 0 being a valid dimension size... */
    if (*out_dimension == 0) {
        return error_ctor(ERR_INVALID_DIMENSION,
                          "Dimension size cannot be zero!\n");
    }
    return error_none();
}

/**
 * @brief loads bitmap dimensions
 * @return error_none when loaded successfully, otherwise Error::code > 0 */
static inline Error bmp_loader_load_size(FILE *file, BitmapSize *out_size) {
    Error err = bmp_loader_load_dimension(file, &out_size->height);
    if (err.code != ERR_NONE) {
        return err;
    }
    return bmp_loader_load_dimension(file, &out_size->width);
}

/**
 * @brief loads a bitmap into the staging buffer
 * @note ensure the loader has a unique staging buffer to avoid violations */
static Error bmp_loader_load(BitmapLoader *restrict loader) {
    /* try to open bitmap */
    FILE *file = fopen(loader->file_name, "r");
    if (file == NULL) {
        return error_ctor(ERR_INVALID_BITMAP_FILE,
                          "Failed to open file [%s]! Os error: %s\n",
                          loader->file_name, strerror(errno));
    }
    /* load the data size from file */
    BitmapSize size = {0};
    Error      err = bmp_loader_load_size(file, &size);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* allocate (blank) staging buffer for bitmap */
    err = bmp_ctor(size, &loader->staging);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* filter whitespace from file and write to the staging buffer */
    err = bmp_loader_ignore_whitespace(file, loader);
    if (err.code != ERR_NONE) {
        fclose(file);
        return err;
    }
    /* assure that the bitmap file has been read correctly */
    size_t expected_size = bmp_size_raw(loader->staging.dimensions);
    if (loader->size != expected_size) {
        fclose(file);
        return error_ctor(ERR_INVALID_DIMENSION,
                          "The number of pixels found (%zu) is not the same as "
                          "defined in the header size (%" PRIu32 "x%" PRIu32
                          "=%" PRIu32 ")\n",
                          loader->size, loader->staging.dimensions.height,
                          loader->staging.dimensions.width,
                          bmp_size_raw(loader->staging.dimensions));
    }
    /* cleanup and return */
    fclose(file);
    return error_none();
}

/* =========================================
 *                 Point
 * ========================================= */

/** @brief stores coordinate value of pixel from a bitmap */
typedef struct Point {
    /** @brief represents bitmap's column index */
    uint32_t x;
    /** @brief represents bitmap's row index */
    uint32_t y;
} Point;

/** @brief constructs Point */
static inline Point point_ctor(uint32_t x, uint32_t y) { return (Point){x, y}; }

/** @brief constructs Point and populates it with invalid data */
static inline Point point_invalid_ctor() {
    return (Point){COORD_INVALID, COORD_INVALID};
}

/** @brief checks whether a given point has invalid coordinates */
static inline bool point_is_invalid(Point p) {
    return p.x == COORD_INVALID || p.y == COORD_INVALID;
}

/* =========================================
 *              ShapeGeometry
 * ========================================= */

/** @brief a common structure to represent geometrical shapes (Line/Square) */
typedef struct {
    /** @brief top-left or start point */
    Point start;
    /** @brief bottom-right or end point */
    Point end;
} ShapeGeometry;

/** @brief constructs basic shape from given points */
static inline ShapeGeometry shape_geometry_ctor(Point start, Point end) {
    return (ShapeGeometry){start, end};
}
/** @brief constructs invalid shape */
static inline ShapeGeometry shape_geometry_invalid_ctor() {
    return shape_geometry_ctor(point_invalid_ctor(), point_invalid_ctor());
}
/**
 * @brief compares two shapes based on their size
 * @return <0...lhs<rhs; >0...lhs>rhs; =0...lhs==rhs */
static int shape_geometry_cmp(const ShapeGeometry lhs, const ShapeGeometry rhs,
                              uint32_t (*size_func)(const ShapeGeometry)) {
    uint32_t length_delta = size_func(lhs) - size_func(rhs);
    if (length_delta != 0) {
        return length_delta;
    }
    if (lhs.start.y != rhs.start.y) {
        return rhs.start.y - lhs.start.y;
    }
    return rhs.start.x - lhs.start.x;
}
static inline void shape_geometry_print(const ShapeGeometry shape) {
    printf("%" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", shape.start.y,
           shape.start.x, shape.end.y, shape.end.x);
}
static inline bool shape_geometry_is_invalid(const ShapeGeometry shape) {
    return point_is_invalid(shape.start) || point_is_invalid(shape.end);
}

/* =========================================
 *                  Line
 * ========================================= */

/** @brief defines a line by its begin and end point */
typedef ShapeGeometry Line;
typedef Line          VLine;  // [V]ertical
typedef Line          HLine;  // [H]orizontal

#define line_ctor(start, end) shape_geometry_ctor((start), (end))
#define line_invalid_ctor()   shape_geometry_invalid_ctor()
#define line_is_invalid(line) shape_geometry_is_invalid((line))
#define line_print(line)      shape_geometry_print((line))
#define hline_cmp(lhs, rhs)   shape_geometry_cmp((lhs), (rhs), hline_length)
#define vline_cmp(lhs, rhs)   shape_geometry_cmp((lhs), (rhs), vline_length)

/** @brief calculates length of horizontal line */
static inline uint32_t hline_length(HLine line) {
    return line.end.x - line.start.x + 1;
}
/** @brief calculates length of vertical line */
static inline uint32_t vline_length(VLine line) {
    return line.end.y - line.start.y + 1;
}

/**
 * @brief searches for a vertical line from `begin` in bitmap
 * @return invalid horizontal line if given point is not set */
static inline HLine line_find_hline(const Bitmap *bmp, const Point begin) {
    if (bmp_at(bmp, begin.y, begin.x) == PXL_FILLED) {
        /* iterate until hit empty pixel or end of row */
        uint32_t x_track = begin.x + 1;
        for (; x_track < bmp->dimensions.width &&
               bmp_at(bmp, begin.y, x_track) == PXL_FILLED;
             x_track++)
            ;
        return line_ctor(begin, point_ctor(x_track - 1, begin.y));
    }
    return line_invalid_ctor();
}

/** @see line_find_hline */
static inline VLine line_find_vline(const Bitmap *bmp, const Point begin) {
    if (bmp_at(bmp, begin.y, begin.x) == PXL_FILLED) {
        /* iterate until hit empty pixel or end of column */
        uint32_t y_track = begin.y + 1;
        for (; y_track < bmp->dimensions.height &&
               bmp_at(bmp, y_track, begin.x) == PXL_FILLED;
             y_track++) {
        }
        return line_ctor(begin, point_ctor(begin.x, y_track - 1));
    }
    return line_invalid_ctor();
}

/** @brief scans for longest horizontal line */
static HLine line_find_longest_hline(const Bitmap *bmp) {
    HLine    max = line_invalid_ctor(), temp = {0};
    uint32_t max_length = 0;
    /* iterate over each row */
    for (uint32_t row = 0; row < bmp->dimensions.height; row++) {
        /* scan each line for any horizontal line matches */
        for (uint32_t col = 0; col < bmp->dimensions.width - max_length;
             col++) {
            temp = line_find_hline(bmp, (Point){col, row});
            if (line_is_invalid(temp)) {
                continue;
            }
            col = temp.end.x;
            if (line_is_invalid(max) || hline_cmp(max, temp) < 0) {
                max = temp;
                max_length = hline_length(max);
            }
        }
    }
    return max;
}

/** @brief scans for longest vertical line */
static VLine line_find_longest_vline(const Bitmap *bmp) {
    VLine    max = line_invalid_ctor(), temp = {0};
    uint32_t max_length = 0;
    /* iterate over each column */
    for (uint32_t col = 0; col < bmp->dimensions.width; col++) {
        /* scan each line for any vertical line matches */
        for (uint32_t row = 0; row < bmp->dimensions.height - max_length;
             row++) {
            temp = line_find_vline(bmp, (Point){col, row});
            if (line_is_invalid(temp)) {
                continue;
            }
            row = temp.end.y;
            if (line_is_invalid(max) || vline_cmp(max, temp) < 0) {
                max = temp;
                max_length = vline_length(max);
            }
        }
    }
    return max;
}

/* =========================================
 *                  Square
 * ========================================= */

/** @brief Square is defined by its "top_left" point (ShapeGeometry::start) and
 * its "bottom_right" point (ShapeGeometry::end) */
typedef ShapeGeometry Square;

#define square_ctor(start, end)   shape_geometry_ctor(start, end)
#define square_invalid_ctor()     shape_geometry_invalid_ctor()
#define square_is_invalid(square) shape_geometry_is_invalid(square)
#define square_print(square)      shape_geometry_print(square)
#define square_cmp(lhs, rhs)      shape_geometry_cmp(lhs, rhs, square_side_length)

static inline uint32_t square_side_length(const Square s) {
    return s.end.x - s.start.x + 1;
}
#define square_find_vertical_side(bmp, row, col) \
    (line_find_vline((bmp), (Point){(col), (row)}))
#define square_find_horizontal_side(bmp, row, col) \
    (line_find_hline((bmp), (Point){(col), (row)}))

/** @brief determines whether there exists valid square composed of `top_left`
 * and `bottom_right` corner */
static inline bool square_found_valid_square(const Bitmap *bmp, Point top_left,
                                             Point bottom_right) {
    HLine hline = square_find_horizontal_side(bmp, bottom_right.y, top_left.x);
    VLine vline = square_find_vertical_side(bmp, top_left.y, bottom_right.x);

    return hline.end.x >= bottom_right.x && vline.end.y >= bottom_right.y;
}

static inline void square_set_max_square(Square *max, uint32_t *max_length,
                                         Square rhs) {
    if (square_is_invalid(*max) || square_cmp(*max, rhs) < 0) {
        *max = rhs;
        *max_length = square_side_length(rhs);
    }
}

/** @brief moves from `top_left` point horizontally and vertically, stops at the
 * the nearest end or nearest empty pixel
 * @note it is assumed that `top_left` is non-empty pixel
 * @return point of ended iteration (from `top_left`) */
static Point square_move_along_orthogonals(const Bitmap *bmp,
                                           const Point   top_left) {
    uint32_t x_track = top_left.x, y_track = top_left.y;
    /* move horizontally and vertically until hit end / empty pixel */
    for (;;) {
        if (x_track >= bmp->dimensions.width ||
            bmp_at(bmp, top_left.y, x_track) == PXL_EMPTY) {
            break;
        }

        if (y_track >= bmp->dimensions.height ||
            bmp_at(bmp, y_track, top_left.x) == PXL_EMPTY) {
            break;
        }

        x_track++;
        y_track++;
    }
    return (Point){x_track - 1, y_track - 1};
}

/**
 * @brief scans for the largest square in a bitmap
 * @return invalid square if no square was found */
static Square square_find_largest_square(const Bitmap *bmp) {
    Square   max = square_invalid_ctor();
    uint32_t max_length = 0;
    for (uint32_t i = 0; i < bmp->dimensions.width * bmp->dimensions.height;
         i++) {
        /* for every pixel, check whether this pixel extends
         * to orthogonal sides of a square (or itself in case of 1x1) */
        uint32_t row = i / bmp->dimensions.width;
        uint32_t col = i % bmp->dimensions.width;

        /* only PXL_FILLED can be potential anchors of square */
        if (bmp->data[i] == PXL_EMPTY) {
            continue;
        }

        /* check if the remaining scan area is still larger than the largest
         * square we have found */
        uint32_t remaining_area =
            (bmp->dimensions.height - row) * bmp->dimensions.width;
        if (max_length * max_length >= remaining_area) {
            return max;
        }

        /* determine the expected bottom point and check for the square, if
         * parallel sides were not found, check for each square "inside" the
         * range of left_up to the expected_bottom_right point */
        const Point top_left = {col, row};
        Point       expected_bottom_right =
            square_move_along_orthogonals(bmp, top_left);

        /* if (potential) square side length is lower than the current, no
         * reason to continue */
        if (max_length > expected_bottom_right.x - col + 1) {
            continue;
        }

        /* if (potential) square is indeed valid square set it to max (if
         * larger) */
        if (square_found_valid_square(bmp, top_left, expected_bottom_right)) {
            square_set_max_square(&max, &max_length,
                                  square_ctor(top_left, expected_bottom_right));
            continue;
        }

        /* check each (potential) square inside the orthogonals */
        for (; expected_bottom_right.x >= col;
             expected_bottom_right.x--, expected_bottom_right.y--) {
            if (square_found_valid_square(bmp, top_left,
                                          expected_bottom_right)) {
                square_set_max_square(
                    &max, &max_length,
                    square_ctor(top_left, expected_bottom_right));
                break;
            }
        }
    }
    return max;
}

/* =========================================
 *                 Command
 * ========================================= */

/** @brief specifies the type of command the user can execute */
typedef enum UserCommandAction {
    HELP = 0,
    TEST,
    HLINE,
    VLINE,
    SQUARE
} UserCommandAction;
/** @brief struct containing command information passed by the user */
typedef struct UserCommand {
    UserCommandAction action_type;
    /** @brief path to the bitmap file */
    const char *file_name;
} UserCommand;

static const char *HELP_MESSAGE =
    "Figsearch Algorithm\n"
    "===================\n"
    "A tool to analyze bitmap images for specific geometric patterns.\n\n"
    "USAGE:\n"
    "    figsearch [command] [bitmap location]\n\n"
    "COMMANDS:\n"
    "    --help       Displays this help message.\n"
    "    test         Validates the specified bitmap file.\n"
    "                 Requires: [bitmap location].\n"
    "    hline        Finds the longest horizontal line in the bitmap.\n"
    "                 Requires: [bitmap location].\n"
    "    vline        Finds the longest vertical line in the bitmap.\n"
    "                 Requires: [bitmap location].\n"
    "    square       Detects the largest square in the bitmap.\n"
    "                 Requires: [bitmap location].\n\n"
    "NOTES:\n"
    "    - All commands (except --help) require the [bitmap location] "
    "argument.\n"
    "    - hline, vline and square commands implicitly check the validity of "
    "the file.\n"
    "    - The bitmap location should be a valid path to a bitmap file.\n"
    "    - Example usage: figsearch hline my_image.bmp\n";

/**
 * @brief executes "--help" figsearch command by printing basic data about the
 * command options */
static inline Error cmd_display_help_message(void) {
    printf("%s", HELP_MESSAGE);
    return error_none();
}

/**
 * @brief executes "test" figsearch command by trying to load the bitmap file
 * @return ERR_INVALID_BITMAP_FILE with message "Invalid" when bitmap file is
 * not valid */
static inline Error cmd_validate_bitmap_file(const char *file_name) {
    /* try to load bmp, if caught any errors, print invalid */
    BitmapLoader temp = bmp_loader_ctor(file_name);
    Error        err = bmp_loader_load(&temp);
    if (err.code != ERR_NONE) {
        error_dtor(&err);
        bmp_loader_dtor(&temp);
        return error_ctor(ERR_INVALID_BITMAP_FILE, "%s", "Invalid");
    }
    printf("Valid\n");
    /* cleanup and return */
    bmp_loader_dtor(&temp);
    return error_none();
}

/**
 * @brief loads bmp from given `file_name` and executes shape search function */
static Error cmd_execute_shape_search(
    const char *file_name, ShapeGeometry (*shape_search)(const Bitmap *bmp)) {
    /* load bitmap */
    Bitmap bmp = {0};
    {
        BitmapLoader loader = bmp_loader_ctor(file_name);
        Error        err = bmp_loader_load(&loader);
        if (err.code != ERR_NONE) {
            bmp_loader_dtor(&loader);
            return err;
        }
        bmp = bmp_loader_get_bitmap(&loader);
    }
    /* scan for largest shape */
    const ShapeGeometry shape = shape_search(&bmp);
    /* print results */
    if (shape_geometry_is_invalid(shape)) {
        printf("Not found\n");
    } else {
        shape_geometry_print(shape);
    }
    /* cleanup and return */
    bmp_dtor(&bmp);
    return error_none();
}

static Error cmd_execute(UserCommand *cmd) {
    switch (cmd->action_type) {
        case HELP:
            return cmd_display_help_message();
        case TEST:
            return cmd_validate_bitmap_file(cmd->file_name);
        case HLINE:
            return cmd_execute_shape_search(cmd->file_name,
                                            line_find_longest_hline);
        case VLINE:
            return cmd_execute_shape_search(cmd->file_name,
                                            line_find_longest_vline);
        case SQUARE:
            return cmd_execute_shape_search(cmd->file_name,
                                            square_find_largest_square);
    }
    return error_ctor(ERR_INTERNAL, "Invalid control path executed on line: %d",
                      __LINE__);
}

/**
 * @brief parses command input and validates it
 * @return error with appropriate message if `out_cmd` param could not be
 * populated because of invalid input data */
static Error cmd_parse(int argc, char **argv, UserCommand *out_cmd) {
    /* ensure that the command is of correct size */
    if (argc > CMD_MAX_ARGS || argc < CMD_MIN_ARGS) {
        return error_ctor(ERR_INVALID_NUMBER_ARGS,
                          "Invalid number of arguments given! Expected: "
                          "1 or 2 but given: %d.\nFor more info refer to "
                          "the help info:\n%s",
                          argc - 1, HELP_MESSAGE);
    }

    /* validate command args */
    if (argc == CMD_MIN_ARGS) /* HELP command only */ {
        if (strcmp(argv[1], "--help") == 0) {
            out_cmd->action_type = HELP;
            return error_none();
        }
        return error_ctor(ERR_INVALID_COMMAND,
                          "Invalid command given [%s]! Expected: --help. Did "
                          "you forget to add bitmap file name?",
                          argv[1]);
    }

    /*
     * convenient macro for command type check (cmd_parse
     * function-only)
     */
#define register_command(cmd_input, cmd_name, reg_type, reg_file_name)     \
    do {                                                                   \
        if (strcmp((cmd_input), (cmd_name)) == 0) {                        \
            *out_cmd = (struct UserCommand){.action_type = (reg_type),     \
                                            .file_name = (reg_file_name)}; \
            return error_none();                                           \
        }                                                                  \
    } while (0);

    register_command(argv[1], "test", TEST, argv[2]);
    register_command(argv[1], "hline", HLINE, argv[2]);
    register_command(argv[1], "vline", VLINE, argv[2]);
    register_command(argv[1], "square", SQUARE, argv[2]);

#undef register_command

    return error_ctor(ERR_INVALID_COMMAND,
                      "Invalid command given [%s]! Expected one of: --help, "
                      "test, hline, vline, sqaure.",
                      argv[1]);
}

int main(int argc, char **argv) {
    /* parse command line */
    UserCommand cmd = {0};
    {
        Error err = cmd_parse(argc, argv, &cmd);
        if (err.code != ERR_NONE) {
            error_print(err);
            return error_dtor(&err);
        }
    }

    /* execute given command */
    {
        Error err = cmd_execute(&cmd);
        if (err.code != ERR_NONE) {
            error_print(err);
            return error_dtor(&err);
        }
    }

    return EXIT_SUCCESS;
}
