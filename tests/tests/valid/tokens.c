// Lexer coverage: every keyword, identifier forms, and every punctuator.

#define STRINGIFY(x) #x
#define JOIN(a, b) a##b

typedef unsigned long size_type;

struct point {
    int x;
    int y;
};

union value {
    int i;
    double d;
};

enum color {
    RED,
    GREEN = 2,
    BLUE
};

extern int external_value;
static int static_value = 1;
const volatile int cv = 2;

_Alignas(16) static int aligned_value;
_Atomic int atomic_value;
_Bool bool_value;
static _Thread_local int tls_value;
__thread int gnu_tls_value;
static int attributed __attribute__((unused));
static inline int inline_fn(void) { return 0; }
_Static_assert(sizeof(int) >= 2, "int size");
static int generic_value = _Generic(1, int: 1, default: 0);
static _Complex double complex_value = 1.0;
static int $dollar = 0;

int takes_many(int first, ...);
int restrict_param(int *restrict p);
int gnu_restrict_param(int *__restrict p);

static int helper(int a, int b)
{
    auto int auto_value = 1;
    register int reg_value = 2;
    short short_value = 3;
    long long_value = 4;
    float float_value = 5.0f;
    double double_value = 6.0;
    char char_value = 'c';
    signed int signed_value = -1;
    unsigned int unsigned_value = 1u;
    void *void_pointer = 0;
    typeof(static_value) copy = static_value;
    int array[4] = { 0, 1, 2, 3 };
    struct point p = { 1, 2 };
    struct point *p_ptr = &p;
    union value v = { .i = 3 };
    enum color c = RED;
    int joined = JOIN(12, 34);
    const char *name = STRINGIFY(identifier);
    int result = 0;
    int returning = 2;
    int charlie = 3;
    int _hidden = 4;

    asm("nop");

    if (a && b || !a) {
        result += 1;
        result -= 1;
        result *= 2;
        result /= 2;
        result %= 3;
        result <<= 1;
        result >>= 1;
        result &= 3;
        result |= 4;
        result ^= 5;
    } else if (a == b) {
        result = a != b;
    } else {
        result = 0;
    }

    for (int i = 0; i < 4; i++)
        array[i] = i;

    while (result > 0)
        result--;

    do {
        result++;
    } while (result < 3);

    switch (result) {
    case 0:
        result = ~result;
        break;
    case 1:
        result = result << 1;
        break;
    case 2:
        result = result >> 1;
        break;
    default:
        result = result & a;
        break;
    }

    result = a ? b : 0;
    result = (a, b);
    result = sizeof(int) + _Alignof(int);
    result = p_ptr->x + p.y + v.i + c;
    result = (int)array[0];
    result = takes_many(a, b);
    result = restrict_param(&result);
    result = gnu_restrict_param(&result);
    result = copy;
    result += joined;
    result += (int)(name[0]);
    result += (int)(size_type)sizeof(size_type);
    result += returning + charlie + _hidden;
    result += inline_fn() + generic_value + (int)complex_value + $dollar;

    if (result < 10)
        goto done;
    result -= 1;

done:
    return result;
}

int main(void)
{
    return helper(1, 2);
}
