// Lexer coverage: escape sequences that are tolerated but not portable,
// alongside the portable ones, in both characters and strings.

int main(void)
{
    const char *unknown_char = "\q";
    int unknown_hex = '\q';
    const char *no_octal_digits = "\8";
    const char *trailing_backslash = "\\";
    int escaped_quote = '\"';
    int escaped_backslash = '\\';

    return 0;
}
