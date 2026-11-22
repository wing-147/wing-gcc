// C11 6.7.2 - Valid type-specifier combinations

// void
void f_void(void);

// char
char t_char;
signed char t_signed_char;
unsigned char t_unsigned_char;

// short
short t_short;
signed short t_signed_short;
short int t_short_int;
signed short int t_signed_short_int;
unsigned short t_unsigned_short;
unsigned short int t_unsigned_short_int;

// int
int t_int;
signed t_signed;
signed int t_signed_int;
unsigned t_unsigned;
unsigned int t_unsigned_int;

// long
long t_long;
signed long t_signed_long;
long int t_long_int;
signed long int t_signed_long_int;
unsigned long t_unsigned_long;
unsigned long int t_unsigned_long_int;

// long long
long long t_long_long;
signed long long t_signed_long_long;
long long int t_long_long_int;
signed long long int t_signed_long_long_int;
unsigned long long t_unsigned_long_long;
unsigned long long int t_unsigned_long_long_int;

// float
float t_float;

// double
double t_double;
long double t_long_double;

// _Bool
_Bool t_bool;

// Different specifier orders
int short t_short_int_reversed;
int unsigned short t_unsigned_short_reversed;
int long unsigned t_unsigned_long_reversed;
int long long unsigned t_unsigned_long_long_reversed;
double long t_long_double_reversed;
char signed t_signed_char_reversed;
long int long t_long_long_reversed;
