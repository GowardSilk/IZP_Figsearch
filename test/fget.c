#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

static int resize_counter = 0;

typedef struct BitmapVector {
    char*  data;
    size_t size;
    size_t cap;
} BitmapVector;

BitmapVector bmp_vector_ctor() {
    return (BitmapVector) {
        .data = malloc(1),
        .size = 0,
        .cap  = 1
    };
}

void bmp_vector_dtor(BitmapVector* bmp) {
    if (bmp->data) {
        free(bmp->data);
        bmp->data = NULL;
        bmp->size = 0;
        bmp->cap = 0;
    }
}

BitmapVector* bmp_vector_resize(BitmapVector* bmp) {
    char* data = realloc(bmp->data, bmp->cap + bmp->cap/2);
    if (!data) { return NULL; }
    bmp->cap  += ceil(bmp->cap/2.f);
    bmp->data = data;
    resize_counter++;
    return bmp;
}

BitmapVector* bmp_vector_add(BitmapVector* bmp, char val) {
    if (bmp->size + 1 >= bmp->cap) {
        if (!bmp_vector_resize(bmp)) {
            return NULL;
        }
    }
    bmp->data[bmp->size++] = val;
    return bmp;
}

bool valid_whitespace(char c) {
    return c == ' ' || c == '\r' || c == '\n';
}
bool valid_pix(char c) {
    return c == '1' || c == '0';
}

int main(int argc, char** argv) {
    /* note: directly assumes correct sys args */
    FILE* file = fopen(argv[2], "r");
    if (!file) {
        return -1;
    }
    /* validate size anyway... */
    uint32_t dimensions[2] = {0};
    if (fscanf(file, "%u", &dimensions[1]) != 1) {
        return -1;
    }
    if (fscanf(file, "%u", &dimensions[0]) != 1) {
        return -1;
    }

    BitmapVector bmp = bmp_vector_ctor();
    int c = fgetc(file);
    for (; c != EOF; c = fgetc(file)) {
        if (valid_whitespace(c)) { continue; }
        if (valid_pix(c)) {
            bmp_vector_add(&bmp, c);
            continue;
        }
        // cleanup on error
        fclose(file);
        bmp_vector_dtor(&bmp);
        return -1; // invalid char
    }

    printf("Number of resizes: %d\n", resize_counter);

    // main exit cleanup
    bmp_vector_dtor(&bmp);
    fclose(file);

    return 0;
}
