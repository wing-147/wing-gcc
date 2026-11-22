// C11 6.7.2 - Invalid type-specifier combinations

// Duplicate type specifiers
int int bad_duplicate_int;
char char bad_duplicate_char;
short short bad_duplicate_short;
float float bad_duplicate_float;
double double bad_duplicate_double;
void void bad_duplicate_void;
_Bool _Bool bad_duplicate_bool;

// Duplicate signed / unsigned
signed signed bad_duplicate_signed;
unsigned unsigned bad_duplicate_unsigned;

// Too many long specifiers
long long long bad_too_many_long;
long long long int bad_too_many_long_int;

// signed and unsigned cannot coexist
signed unsigned bad_signed_unsigned;
signed unsigned int bad_signed_unsigned_int;
unsigned signed char bad_unsigned_signed_char;

// Incompatible base types
char int bad_char_int;
short char bad_short_char;
short double bad_short_double;
short long int bad_short_long_int;
long char bad_long_char;
int float bad_int_float;
int double bad_int_double;
float double bad_float_double;

// Invalid floating-point combinations
long float bad_long_float;
signed float bad_signed_float;
unsigned float bad_unsigned_float;
signed double bad_signed_double;
unsigned double bad_unsigned_double;
long long double bad_long_long_double;

// void and _Bool cannot combine with other type specifiers
void int bad_void_int;
void unsigned bad_void_unsigned;
_Bool int bad_bool_int;
_Bool long bad_bool_long;
