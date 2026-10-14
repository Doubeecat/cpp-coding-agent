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
static int self_test(void) {
    int values[] = {3, -1, 3, 0};
    const int expected[] = {-1, 0, 3, 3};
    sort(values, 4);
    if (memcmp(values, expected, sizeof(values)))
        return 1;
    puts("PASS ascending with duplicates");
    sort(values, 0);
    puts("PASS empty input");
    int one = 7;
    sort(&one, 1);
    if (one != 7)
        return 1;
    puts("PASS single value");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test"))
        return self_test();
    int count;
    if (scanf("%d", &count) != 1 || count < 0 || count > 1000000) {
        fputs("Expected a count between 0 and 1000000, followed by that many integers.\n", stderr);
        return 1;
    }
    int *values = count ? malloc((size_t)count * sizeof(*values)) : NULL;
    if (count && !values)
        return 1;
    for (int i = 0; i < count; ++i) {
        if (scanf("%d", &values[i]) != 1) {
            free(values);
            return 1;
        }
    }
    sort(values, (size_t)count);
    for (int i = 0; i < count; ++i)
        printf("%d ", values[i]);
    putchar('\n');
    free(values);
    return 0;
}
