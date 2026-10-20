// Lexer coverage: line and block comments in every position, including
// comment-looking text inside comments and a comment at end of file.

// line comment at the top
int main(void)
{
    // a line comment containing /* and */
    int a = 1; /* trailing block comment */
    int b = 2; // trailing line comment

    /*
     * multi-line block comment
     */
    int c = 3;

    /**/
    int d = 4;

    int e = /* inline */ 5;

    return 0;
}

// comment at end of file (no trailing newline)