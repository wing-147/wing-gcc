// Lexer coverage: string literals with every escape, prefixes, adjacent
// concatenation, and universal character names.

int main(void)
{
    const char *empty = "";
    const char *plain = "hello";
    const char *adjacent = "hello" "world";
    const char *quotes = "\"\\\'";
    const char *controls = "\a\b\f\n\r\t\v\?";
    const char *gnu_escape = "\e";
    const char *octal = "\0\101\377";
    const char *hex = "\x41\x0a\xff";
    const char *ucn2 = "\u00e9";
    const char *ucn4 = "\U0001F600";
    const char *ucn5 = "\U00200000";
    const char *ucn6 = "\U7FFFFFFF";
    const char *utf8 = u8"utf8";
    const char *utf16 = u"utf16";
    const char *utf32 = U"utf32";
    const char *wide = L"wide";

    return 0;
}
