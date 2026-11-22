int static_array[static 3];
int qualified_array[const 3];
int star_array[*];
int mixed_qualifiers(int values[const static volatile 3]);

int star_definition(int values[*])
{
    return 0;
}
