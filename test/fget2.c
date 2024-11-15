#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct BitmapVector {
    char* data;
    size_t size;
    size_t cap;
} BitmapVector;

BitmapVector bmp_vector_ctor(uint32_t dimensions[2]) {
    size_t vec_cap = dimensions[0] * dimensions[1] + 1;
    return (BitmapVector){
        .data = malloc(vec_cap * sizeof(char)), .size = 0, .cap = vec_cap + 1};
}

BitmapVector* bmp_vector_add(BitmapVector* bmp, char val) {
    if (bmp->size + 1 >= bmp->cap) {
        return NULL;
    }
    bmp->data[bmp->size++] = val;
    return bmp;
}

bool valid_whitespace(char c) { return c == ' ' || c == '\r' || c == '\n'; }
bool valid_pix(char c) { return c == '1' || c == '0'; }

void bmp_vector_dtor(BitmapVector* bmp) {
    free(bmp->data);
    bmp->data = NULL;
    bmp->size = 0;
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

    BitmapVector bmp = bmp_vector_ctor(dimensions);
    int c = fgetc(file);
    for (; c != EOF; c = fgetc(file)) {
        if (valid_whitespace(c)) {
            continue;
        }
        if (valid_pix(c)) {
            if (!bmp_vector_add(&bmp, c)) {
                printf("Invalid!");
                return -1;
            }
            continue;
        }
        // cleanup on error
        fclose(file);
        bmp_vector_dtor(&bmp);
        return -1;  // invalid char
    }

    // main exit cleanup
    bmp_vector_dtor(&bmp);
    fclose(file);

    return 0;
}
