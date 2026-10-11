# Empty C++17 project

Generate a small program from the user's requirements. There is no prebuilt application in src/.

Build contract:

- Create src/main.cpp. Put additional .cpp files directly under src/; headers may also go there.
- Only src/ is writable. Makefile is fixed and read-only.
- make test compiles all src/*.cpp into build/app, then runs build/app --self-test.
- Implement main(int argc, char** argv). When argv[1] is --self-test, run noninteractive tests of your real feature functions. Include normal and edge cases. Print PASS/FAIL results; return 0 only if all checks pass, otherwise return nonzero.
- Keep the tests in src/ together with the code. Self-tests must not wait for user input. Temporary test files belong in build/.
- Keep the program compact, preferably under 80 lines. Test the same feature functions used by normal execution; use a small table of inputs and expected outputs rather than duplicating test blocks.
- Without --self-test, run the actual requested program. You may read stdin or command-line arguments; explain how to use it in your final answer.
- Preserve earlier features and add checks when extending the program in later questions.
- The build enables undefined-behavior and float-cast-overflow checks. Read its actual output and repair failures.

Use write_file to create real source files and build_and_test to verify them. A code block alone does not create a program.
