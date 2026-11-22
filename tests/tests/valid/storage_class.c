static int file_value;
extern int external_value;

static int accumulate(register int value)
{
    auto int local = value;
    register int result = local;
    for (auto int index = 0; index < 1; index = index + 1)
        result = result + index;
    return result;
}

int main(void)
{
    return accumulate(file_value);
}
