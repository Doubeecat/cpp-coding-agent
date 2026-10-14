# Empty C11 project

Generate a small C program from the user's requirements. There is no prebuilt application in src/.

- Create src/main.c; put additional .c files and headers directly under src/.
- make test compiles all src/*.c into build/app using a C11 compiler, then runs build/app --self-test with noninteractive stdin.
- Implement main(int argc, char **argv). For --self-test, exercise the real feature functions with normal and edge cases. Print PASS/FAIL; return zero only when every check passes.
- Without --self-test, run the actual requested program and explain its usage.
- Preserve earlier features and their tests when extending a program. Temporary test files belong in build/.
- Read real compiler and sanitizer output and repair failures. Use Read/Write/Edit to inspect and create files, and Bash to run make test. A code block alone does not create a program.
