// Lexer coverage: every integer and floating literal spelling the front end
// accepts, including prefixes, suffixes, exponents and hex floats.

int main(void)
{
    int decimal = 0;
    int octal = 0123;
    int hex = 0x1f;
    int HEX = 0X1F;
    int binary = 0b1010;
    int BINARY = 0B1010;

    unsigned int uint_suffix = 42u;
    unsigned int UINT_suffix = 42U;
    long long_suffix = 42l;
    long LONG_suffix = 42L;
    long long llong_suffix = 42ll;
    long long LLONG_suffix = 42LL;

    unsigned long ulong_suffix = 42ul;
    unsigned long ULONG_suffix = 42UL;
    unsigned long lu_suffix = 42lu;
    unsigned long LU_suffix = 42LU;
    unsigned long long ullong_suffix = 42ull;
    unsigned long long ULLONG_suffix = 42ULL;
    unsigned long long llu_suffix = 42llu;
    unsigned long long LLU_suffix = 42LLU;
    unsigned long long uLL_suffix = 42uLL;
    unsigned long long Ull_suffix = 42Ull;

    double point_five = 0.5;
    double leading_dot = .5;
    double trailing_dot = 1.;
    double exponent = 1e10;
    double negative_exponent = 1E-5;
    double positive_exponent = 1e+5;

    float float_suffix = 1.5f;
    float FLOAT_suffix = 1.5F;
    long double ldouble_suffix = 1.5l;
    long double LDOUBLE_suffix = 1.5L;

    double hex_exponent = 0x1p3;
    double hex_fraction = 0x1.fp3;
    double hex_leading_dot = 0x.8p1;
    double hex_negative = 0x1P-2;
    float hex_float_suffix = 0x1p-2f;

    double octal_float = 08.5;
    double octal_exponent = 01e+2;
    double zero_exponent = 0.e10;
    unsigned long long huge = 18446744073709551615;

    return (int)(decimal + octal + hex + HEX + binary + BINARY + uint_suffix + UINT_suffix +
                 long_suffix + LONG_suffix + llong_suffix + LLONG_suffix + ulong_suffix +
                 ULONG_suffix + lu_suffix + LU_suffix + ullong_suffix + ULLONG_suffix +
                 llu_suffix + LLU_suffix + uLL_suffix + Ull_suffix + point_five + leading_dot +
                 trailing_dot + exponent + negative_exponent + positive_exponent + float_suffix +
                 FLOAT_suffix + ldouble_suffix + LDOUBLE_suffix + hex_exponent + hex_fraction +
                 hex_leading_dot + hex_negative + hex_float_suffix + octal_float + octal_exponent +
                 zero_exponent);
}
