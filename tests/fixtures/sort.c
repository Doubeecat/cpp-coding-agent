#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare(const void *left, const void *right) {
    int a = *(const int *)left, b = *(const int *)right;
    return (a > b) - (a < b);
}
static void sort(int *values, size_t count) {
    if (count > 1)
        qsort(values, count, sizeof(*values), compare);
}
static void reverse(int *values, size_t count) {
    for (size_t i = 0; i < count / 2; ++i) {
        int value = values[i];
        values[i] = values[count - 1 - i];
        values[count - 1 - i] = value;
    }
}
static int self_test(void) {
    int values[] = {3, -1, 3, 0};
    const int expected[] = {-1, 0, 3, 3};
    sort(values, 4);
    if (memcmp(values, expected, sizeof(values)))
        return 1;
    puts("PASS ascending with duplicates");
    sort(values, 0);
    puts("PASS empty input");
    reverse(values, 4);
    if (values[0] != 3 || values[3] != -1)
        return 1;
    puts("PASS descending order");
    FILE *file = fopen("build/self-test.txt", "w");
    if (!file)
        return 1;
    fputs("generated program", file);
    if (fclose(file))
        return 1;
    char text[32] = {0};
    file = fopen("build/self-test.txt", "r");
    if (!file)
        return 1;
    int passed = fgets(text, sizeof(text), file) && !strcmp(text, "generated program");
    fclose(file);
    if (!passed)
        return 1;
    puts("PASS file roundtrip");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test"))
        return self_test();
    int *values = NULL, value;
    size_t count = 0;
    while (scanf("%d", &value) == 1) {
        int *next = realloc(values, (count + 1) * sizeof(*values));
        if (!next) {
            free(values);
            return 1;
        }
        values = next;
        values[count++] = value;
    }
    sort(values, count);
    for (size_t i = 0; i < count; ++i)
        printf("%d ", values[i]);
    putchar('\n');
    free(values);
    return 0;
}
