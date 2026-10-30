/**
 * @brief api/Common.h declares main without arguments, so define the parameterized host entry
 * point in C rather than a conflicting C++ unit.
 */
int host_main(int argc, char **argv);

/**
 * @brief Parse options and run the host main validation or packaging command.
 */
int main(int argc, char **argv)
{
    return host_main(argc, argv);
}
