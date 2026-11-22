int consume_pointer(double *value);
int consume_array_pointer(int (*values)[2]);
int consume_callback(int (*callback)(int));
int consume_abstract_array_pointer(int (*)[2]);

int *make_pointer(void);
int (*make_callback(void))(int);
