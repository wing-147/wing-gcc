int returns_array(void)[3];
int array_of_functions[3](void);

int main(void)
{
    return sizeof(int ()()) + sizeof(int ()[3]);
}
