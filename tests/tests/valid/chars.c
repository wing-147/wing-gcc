// Lexer coverage: character constants with every escape, octal/hex/UCN forms,
// multi-character constants, and the u/U/L prefixes.

int main(void)
{
    int a = 'a';
    int b = '\n';
    int c = '\t';
    int d = '\\';
    int e = '\'';
    int f = '\"';
    int g = '\?';
    int h = '\a';
    int i = '\b';
    int j = '\f';
    int k = '\r';
    int l = '\v';
    int m = '\e';
    int n = '\0';
    int o = '\101';
    int p = '\377';
    int q = '\x41';
    int r = '\xff';
    int s = '\u00e9';
    int t = '\u20ac';
    int u = '\U0001F600';
    int high5 = '\U00200000';
    int high6 = '\U7FFFFFFF';
    int v = 'ab';
    int w = 'abc';
    int x = u'a';
    int y = U'a';
    int z = L'a';

    return 0;
}
